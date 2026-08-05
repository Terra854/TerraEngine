// EditorProfilerService.cpp — Editor profiling service implementation
//
// Implements the profiling session management logic, including background tasks for saving and loading large capture documents. It integrates with the engine's main thread pool to ensure that capture processing doesn't block the editor UI.

#include "EditorProfilerService.h"

#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <future>
#include <algorithm>
#include <map>
#include <iomanip>
#include <numeric>
#include <sstream>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerCaptureDocument;
using TerraEngine::Profiling::ProfilerCaptureSettings;
using TerraEngine::Profiling::ProfilerSessionState;

namespace
{
std::string MakeRecordingStatus(const ProfilerCaptureSettings& settings)
{
	if (settings.detailLevel != TerraEngine::Profiling::ProfilerCaptureDetailLevel::Detailed)
	{
		return "Recording";
	}
	if (settings.maxFrames == 0u)
	{
		return "Recording every frame (detailed, unlimited history)";
	}
	return "Recording every frame (detailed, " + std::to_string(settings.maxFrames) + "-frame rolling limit)";
}

// PercentileFromSorted deliberately interpolates between neighbouring samples.
// This produces a stable answer for small captures instead of rounding a P95
// to a different whole frame as capture length changes.
double PercentileFromSorted(const std::vector<double>& sortedValues, double percentile)
{
	if (sortedValues.empty())
	{
		return 0.0;
	}
	const double index = std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sortedValues.size() - 1u);
	const std::size_t lower = static_cast<std::size_t>(index);
	const std::size_t upper = std::min(lower + 1u, sortedValues.size() - 1u);
	return sortedValues[lower] + (sortedValues[upper] - sortedValues[lower]) * (index - static_cast<double>(lower));
}

// MetricAccumulator collects only valid telemetry samples.  Availability is
// retained separately so consumers cannot mistake a missing timing source for
// an observed zero duration.
struct MetricAccumulator
{
	EditorProfilerMetricSummary summary;
	std::vector<double> values;

	void Add(const TerraEngine::Profiling::TelemetryDuration& duration)
	{
		if (duration.availability != TerraEngine::Profiling::TelemetryAvailability::Available)
		{
			++summary.unavailableCount;
			if (summary.unavailableReason.empty())
			{
				summary.unavailableReason = duration.unavailableReason;
			}
			return;
		}
		values.push_back(static_cast<double>(duration.nanoseconds) / 1'000'000.0);
	}

	[[nodiscard]] EditorProfilerMetricSummary Finalize() const
	{
		EditorProfilerMetricSummary result = summary;
		result.sampleCount = values.size();
		result.available = !values.empty();
		if (values.empty())
		{
			return result;
		}
		std::vector<double> sorted = values;
		std::sort(sorted.begin(), sorted.end());
		result.median = PercentileFromSorted(sorted, 0.50);
		result.p95 = PercentileFromSorted(sorted, 0.95);
		result.p99 = PercentileFromSorted(sorted, 0.99);
		result.maximum = sorted.back();
		return result;
	}
};

void AddDuration(
	std::map<std::string, MetricAccumulator>& accumulators,
	std::string id,
	std::string label,
	const TerraEngine::Profiling::TelemetryDuration& duration)
{
	MetricAccumulator& accumulator = accumulators[id];
	if (accumulator.summary.label.empty())
	{
		accumulator.summary.id = id;
		accumulator.summary.label = std::move(label);
		accumulator.summary.unit = "ms";
	}
	accumulator.Add(duration);
}

std::vector<EditorProfilerMetricSummary> FinalizeMetrics(const std::map<std::string, MetricAccumulator>& accumulators)
{
	std::vector<EditorProfilerMetricSummary> result;
	result.reserve(accumulators.size());
	for (const auto& [id, accumulator] : accumulators)
	{
		EditorProfilerMetricSummary summary = accumulator.Finalize();
		// The map key is stable even if a producer omitted its optional display
		// label, which keeps automation comparisons deterministic.
		if (summary.id.empty())
		{
			summary.id = id;
		}
		if (summary.label.empty())
		{
			summary.label = id;
		}
		result.push_back(std::move(summary));
	}
	return result;
}

TerraEngine::Profiling::TelemetryDuration CumulativeDurationDelta(
	const TerraEngine::Profiling::TelemetryDuration& current,
	const TerraEngine::Profiling::TelemetryDuration& previous)
{
	if (current.availability != TerraEngine::Profiling::TelemetryAvailability::Available)
	{
		return current;
	}
	if (previous.availability != TerraEngine::Profiling::TelemetryAvailability::Available)
	{
		return {
			TerraEngine::Profiling::TelemetryAvailability::Unavailable,
			0u,
			"The previous cumulative worker sample was unavailable."
		};
	}
	// A decreasing counter means ThreadPool telemetry was reset during capture.
	// In that case the current value is precisely the work completed since reset.
	return {
		TerraEngine::Profiling::TelemetryAvailability::Available,
		current.nanoseconds >= previous.nanoseconds
			? current.nanoseconds - previous.nanoseconds
			: current.nanoseconds,
		{}
	};
}

std::uint64_t CumulativeCounterDelta(std::uint64_t current, std::uint64_t previous)
{
	return current >= previous ? current - previous : current;
}
}

// EditorProfilerService::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Initialize(EditorContext& context)
{
	ctx = &context;
	OnProjectOpened();
}

// EditorProfilerService::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Shutdown()
{
	OnProjectClosed();
	ctx = nullptr;
}

// EditorProfilerService::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::OnProjectOpened()
{
	if (!ctx || !ctx->projectManager || !ctx->projectManager->HasActiveProject())
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		++projectGeneration;
		if (cancellationToken)
		{
			cancellationToken->store(true, std::memory_order_release);
		}
		cancellationToken = std::make_shared<std::atomic_bool>(false);
		captureStore.SetProjectRoot(ctx->projectManager->GetProjectRoot());
		lastSavedCaptureArtifact.reset();
		statusText = "Profiler ready";
		lastError.clear();
	}
	RefreshCaptureList();
}

// EditorProfilerService::OnProjectClosed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::OnProjectClosed()
{
	Profiler::Instance().Clear();
	std::lock_guard<std::mutex> lock(mutex);
	++projectGeneration;
	if (cancellationToken)
	{
		cancellationToken->store(true, std::memory_order_release);
	}
	captureStore.ClearProjectRoot();
	captures.clear();
	lastSavedCaptureArtifact.reset();
	busy = false;
	statusText = "No project open";
	lastError.clear();
}

// EditorProfilerService::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Update(float)
{
}

// EditorProfilerService::StartRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::StartRecording()
{
	ProfilerCaptureSettings settings{};
	// Thread-owned fixed buffers make ordinary detailed capture safe to run on
	// every frame: hot scopes take no shared profiler mutex and a full producer
	// reports drops instead of growing without limit.
	settings.detailLevel = TerraEngine::Profiling::ProfilerCaptureDetailLevel::Detailed;
	settings.recordMarkers = true;
	settings.taskExecutionEnabled = true;
	settings.recordTaskContext = true;
	// The editor's primary Record action keeps the complete session. The Frames
	// presentation remains frozen until Finish, so history growth does not turn
	// the open profiler window into a frame-rate consumer.
	settings.maxFrames = 0u;
	settings.maxDurationSeconds = 0.0;
	StartRecording(settings);
}

void EditorProfilerService::StartRecording(const ProfilerCaptureSettings& settings)
{
	if (!HasActiveProject())
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = "Open a project before profiling.";
		statusText = lastError;
		return;
	}

	Profiler::Instance().StartRecording(MakeMetadata(), settings);
	std::lock_guard<std::mutex> lock(mutex);
	lastSavedCaptureArtifact.reset();
	statusText = MakeRecordingStatus(settings);
	lastError.clear();
}

// EditorProfilerService::PauseRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::PauseRecording()
{
	Profiler::Instance().PauseRecording();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = "Paused";
}

// EditorProfilerService::ResumeRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::ResumeRecording()
{
	Profiler::Instance().ResumeRecording();
	const ProfilerCaptureSettings settings = Profiler::Instance().GetSettings();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = MakeRecordingStatus(settings);
}

// EditorProfilerService::FinishCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::FinishCapture()
{
	if (!HasActiveProject())
	{
		return;
	}

	ProfilerCaptureDocument document = Profiler::Instance().FinishRecording();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (document.frames.empty())
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = "No frames were captured.";
		statusText = lastError;
		return;
	}

	std::uint64_t generation = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		busy = true;
		lastSavedCaptureArtifact.reset();
		statusText = "Saving profiler capture";
		lastError.clear();
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	// FinishRecording transfers ownership of a potentially large bounded capture.
	// Move it into the worker instead of retaining another full frame-history copy
	// on the editor thread while persistence is in progress.
	auto task = ThreadPool::instance().createTask([
		weakSelf,
		storeSnapshot,
		cancelToken,
		document = std::move(document),
		generation]() mutable {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		std::filesystem::path savedPath;
		std::string error;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!storeSnapshot.SaveNewCaptureRetained(document, savedPath, error))
		{
			std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
			if (!self)
			{
				return;
			}
			std::lock_guard<std::mutex> lock(self->mutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
			{
				self->busy = false;
				self->lastError = error;
				self->statusText = "Failed to save capture";
			}
			return;
		}
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		std::optional<EditorProfilerCaptureArtifact> savedCaptureArtifact;
		TerraEngine::Profiling::ProfilerCaptureArtifact savedCapture;
		if (TerraEngine::Profiling::TryReadCaptureArtifactMetadata(
			savedPath, savedCapture) && savedCapture.supported) {
			if(savedCapture.status == "Ready (metadata)") {
				savedCapture.status = "Ready";
			}
			if(savedCapture.status.empty()) {
				savedCapture.status = savedCapture.recoveredIncompleteTail
					? "recovered_incomplete_tail"
					: (savedCapture.complete ? "complete" : "incomplete");
			}
			savedCaptureArtifact = EditorProfilerCaptureArtifact{
				MakeArtifactId(savedCapture.fileName),
				std::move(savedCapture)
			};
		}
		else {
			error = "Saved profiler capture metadata could not be reopened.";
		}
		// The store retained the exact document it serialized. Transfer it directly
		// into the profiler instead of parsing the file again and copying every frame.
		Profiler::Instance().LoadDocument(std::move(document));

		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(self->mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
		{
			if(savedCaptureArtifact.has_value()) {
				const std::filesystem::path normalizedSavedPath =
					savedCaptureArtifact->artifact.absolutePath.lexically_normal();
				auto existing = std::find_if(
					self->captures.begin(), self->captures.end(),
					[&normalizedSavedPath](
						const TerraEngine::Profiling::ProfilerCaptureArtifact& artifact) {
						return artifact.absolutePath.lexically_normal()
							== normalizedSavedPath;
					});
				if(existing != self->captures.end()) {
					*existing = savedCaptureArtifact->artifact;
				} else {
					self->captures.push_back(savedCaptureArtifact->artifact);
				}
				std::sort(self->captures.begin(), self->captures.end(),
					[](const auto& left, const auto& right) {
						return left.fileName > right.fileName;
					});
			}
			self->lastSavedCaptureArtifact = std::move(savedCaptureArtifact);
			self->busy = false;
			self->lastError = error;
			self->statusText = "Saved and opened capture";
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Clear()
{
	Profiler::Instance().Clear();
	const bool hasProject = HasActiveProject();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = hasProject ? "Profiler ready" : "No project open";
	lastError.clear();
}

// EditorProfilerService::RefreshCaptureList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::RefreshCaptureList()
{
	if (!HasActiveProject())
	{
		return;
	}

	std::uint64_t generation = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		busy = true;
		statusText = "Refreshing capture list";
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, generation]() {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}
		std::string error;
		auto refreshed = storeSnapshot.ListCaptures(error, [cancelToken]() {
			return cancelToken && cancelToken->load(std::memory_order_acquire);
		});
		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(self->mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
		{
			self->captures = std::move(refreshed);
			self->lastError = error;
			self->statusText = error.empty() ? "Capture list refreshed" : error;
			self->busy = false;
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::LoadCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::LoadCapture(const std::filesystem::path& capturePath)
{
	if (!HasActiveProject())
	{
		return;
	}

	std::uint64_t generation = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		busy = true;
		statusText = "Opening profiler capture";
		lastError.clear();
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, generation, capturePath]() {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}
		ProfilerCaptureDocument document;
		std::string error;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!storeSnapshot.LoadCapture(capturePath, document, error))
		{
			std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
			if (!self)
			{
				return;
			}
			std::lock_guard<std::mutex> lock(self->mutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
			{
				self->lastError = error;
				self->statusText = "Failed to open capture";
				self->busy = false;
			}
			return;
		}
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		Profiler::Instance().LoadDocument(document);
		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(self->mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
		{
			self->statusText = "Loaded capture";
			self->lastError.clear();
			self->busy = false;
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::GetState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerSessionState EditorProfilerService::GetState() const
{
	return Profiler::Instance().GetState();
}

// EditorProfilerService::GetStatusText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EditorProfilerService::GetStatusText() const
{
	const ProfilerSessionState profilerState = Profiler::Instance().GetState();
	if (profilerState == ProfilerSessionState::Recording || profilerState == ProfilerSessionState::Paused)
	{
		// Live status is owned by the engine profiler because it knows whether a
		// session is capturing Play Mode or merely armed and waiting for it.
		return Profiler::Instance().GetStatusText();
	}
	std::lock_guard<std::mutex> lock(mutex);
	return statusText;
}

// EditorProfilerService::GetLastError is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EditorProfilerService::GetLastError() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return lastError;
}

// EditorProfilerService::GetCaptureList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> EditorProfilerService::GetCaptureList() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return captures;
}

std::vector<EditorProfilerCaptureArtifact> EditorProfilerService::GetAutomationCaptureList() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<EditorProfilerCaptureArtifact> result;
	result.reserve(captures.size());
	for (const auto& capture : captures)
	{
		result.push_back({MakeArtifactId(capture.fileName), capture});
	}
	return result;
}

std::optional<EditorProfilerCaptureArtifact> EditorProfilerService::GetLastSavedCaptureArtifact() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return lastSavedCaptureArtifact;
}

bool EditorProfilerService::ReadCaptureDocumentById(
	std::string_view artifactId,
	ProfilerCaptureDocument& outDocument,
	std::string& outError) const
{
	ProfilerCaptureStore storeSnapshot;
	std::optional<std::filesystem::path> capturePath;
	{
		std::lock_guard<std::mutex> lock(mutex);
		capturePath = FindCapturePathLocked(artifactId);
		storeSnapshot = captureStore;
	}
	if (!capturePath)
	{
		outError = "Profiler artifact identifier is stale or unknown for the active project.";
		return false;
	}
	// The store revalidates project containment immediately before reading, so a
	// stale list entry cannot be redirected outside the project's Profiler folder.
	return storeSnapshot.LoadCapture(*capturePath, outDocument, outError);
}

bool EditorProfilerService::RequestLoadCaptureById(std::string_view artifactId, std::string& outError)
{
	std::optional<std::filesystem::path> capturePath;
	{
		std::lock_guard<std::mutex> lock(mutex);
		capturePath = FindCapturePathLocked(artifactId);
	}
	if (!capturePath)
	{
		outError = "Profiler artifact identifier is stale or unknown for the active project.";
		return false;
	}
	LoadCapture(*capturePath);
	outError.clear();
	return true;
}

bool EditorProfilerService::SummarizeCaptureById(
	std::string_view artifactId,
	EditorProfilerCaptureSummary& outSummary,
	std::string& outError) const
{
	ProfilerCaptureDocument document;
	if (!ReadCaptureDocumentById(artifactId, document, outError))
		return false;
	outSummary = SummarizeDocument(std::string(artifactId), document);
	// Filesystem length is captured once at summary creation.  It is metadata,
	// not a stream sample, and therefore must not be inferred from decoded data.
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto& capture : captures)
		{
			if (MakeArtifactId(capture.fileName) == artifactId)
			{
				std::error_code error;
				outSummary.artifactSizeBytes = std::filesystem::file_size(capture.absolutePath, error);
				if (error)
				{
					outSummary.artifactSizeBytes = 0u;
				}
				break;
			}
		}
	}
	outError.clear();
	return true;
}

EditorProfilerCaptureSummary EditorProfilerService::SummarizeDocument(
	std::string artifactId,
	const ProfilerCaptureDocument& document)
{
	EditorProfilerCaptureSummary summary;
	summary.artifactId = std::move(artifactId);
	summary.frameCount = document.frames.size();
	summary.payloadVersion = document.payloadVersion;
	// V1 artifacts have no explicit committed-count field.  Their fully parsed
	// frame vector is the committed boundary, preserving a useful summary for
	// legacy captures without pretending they were rewritten as v2.
	summary.committedFrameCount = document.committedFrameCount == 0u
		? static_cast<std::uint64_t>(document.frames.size())
		: document.committedFrameCount;
	summary.captureComplete = document.complete;
	summary.recoveredIncompleteTail = document.recoveredIncompleteTail;

	std::map<std::string, MetricAccumulator> stageAccumulators;
	std::map<std::string, MetricAccumulator> boundaryAccumulators;
	std::map<std::string, MetricAccumulator> presentationAccumulators;
	std::map<std::string, MetricAccumulator> responsivenessAccumulators;
	std::vector<double> completeFrameDurations;
	completeFrameDurations.reserve(document.frames.size());
	struct WorkerAccumulator
	{
		EditorProfilerWorkerSummary summary;
		MetricAccumulator run;
		MetricAccumulator wait;
		MetricAccumulator idle;
		std::optional<TerraEngine::Profiling::WorkerTelemetryRecord> previous;
	};
	std::map<std::uint32_t, WorkerAccumulator> workerAccumulators;

	if (!document.frames.empty())
	{
		summary.minimumFrameMs = document.frames.front().durationMs;
	}
	for (const auto& frame : document.frames)
	{
		summary.totalDurationMs += frame.durationMs;
		summary.minimumFrameMs = std::min(summary.minimumFrameMs, frame.durationMs);
		summary.maximumFrameMs = std::max(summary.maximumFrameMs, frame.durationMs);
		if (frame.complete)
		{
			++summary.completeFrameCount;
			completeFrameDurations.push_back(frame.durationMs);
		}
		else
			++summary.incompleteFrameCount;

		for (const TerraEngine::Profiling::PerformanceStageRecord& stage : frame.performanceStages)
		{
			const char* domain = stage.domain == TerraEngine::Profiling::PerformanceStageDomain::Physics
				? "physics"
				: stage.domain == TerraEngine::Profiling::PerformanceStageDomain::SceneView
				? "scene_view"
				: "editor";
			const std::string id = std::string(domain) + "." + stage.stageName;
			AddDuration(stageAccumulators, id, std::string(domain) + " / " + stage.stageName, stage.duration);
		}
		for (const TerraEngine::Profiling::ProtectedBoundaryRecord& boundary : frame.protectedBoundaries)
		{
			const std::string prefix = "boundary." + boundary.boundaryName + ".";
			AddDuration(boundaryAccumulators, prefix + "queue", boundary.boundaryName + " queue", boundary.queueDuration);
			AddDuration(boundaryAccumulators, prefix + "ownership_wait", boundary.boundaryName + " ownership wait", boundary.ownershipWaitDuration);
			AddDuration(boundaryAccumulators, prefix + "execution", boundary.boundaryName + " execution", boundary.executionDuration);
			AddDuration(boundaryAccumulators, prefix + "flush", boundary.boundaryName + " flush", boundary.flushDuration);
			AddDuration(boundaryAccumulators, prefix + "publication", boundary.boundaryName + " publication", boundary.publicationDuration);
		}
		for (const TerraEngine::Profiling::PresentationWaitRecord& wait : frame.presentationWaits)
		{
			const std::string id = "presentation." + std::to_string(static_cast<unsigned int>(wait.kind));
			AddDuration(presentationAccumulators, id, "Presentation wait", wait.duration);
		}
		if (frame.responsiveness.has_value())
		{
			const TerraEngine::Profiling::FrameResponsivenessRecord& responsiveness = *frame.responsiveness;
			AddDuration(responsivenessAccumulators, "responsiveness.input_to_presentation", "Input to presentation", responsiveness.inputToPresentationDuration);
			AddDuration(responsivenessAccumulators, "responsiveness.frame", "Frame responsiveness", responsiveness.frameDuration);
		}
		for (const TerraEngine::Profiling::WorkerTelemetryRecord& worker : frame.workerTelemetry)
		{
			WorkerAccumulator& accumulator = workerAccumulators[worker.workerId];
			accumulator.summary.workerId = worker.workerId;
			++accumulator.summary.sampleCount;
			accumulator.summary.maximumQueueDepth = std::max(accumulator.summary.maximumQueueDepth, worker.queueDepth);
			if (accumulator.run.summary.label.empty())
			{
				accumulator.run.summary = {"worker.run", "Worker run", "ms"};
				accumulator.wait.summary = {"worker.wait", "Worker wait", "ms"};
				accumulator.idle.summary = {"worker.idle", "Worker idle", "ms"};
			}
			if (accumulator.previous.has_value())
			{
				// ThreadPool publishes process-lifetime cumulative counters. Convert
				// adjacent observations into capture-local intervals before summing or
				// calculating percentiles; the first observation is only a baseline.
				const TerraEngine::Profiling::WorkerTelemetryRecord& previous = *accumulator.previous;
				accumulator.summary.totalDispatchCount += CumulativeCounterDelta(worker.dispatchCount, previous.dispatchCount);
				accumulator.summary.totalUsefulWorkItems += CumulativeCounterDelta(worker.usefulWorkItemCount, previous.usefulWorkItemCount);
				accumulator.run.Add(CumulativeDurationDelta(worker.runDuration, previous.runDuration));
				accumulator.wait.Add(CumulativeDurationDelta(worker.waitDuration, previous.waitDuration));
				accumulator.idle.Add(CumulativeDurationDelta(worker.idleDuration, previous.idleDuration));
			}
			accumulator.previous = worker;
		}
		if (frame.physicsWorkVolume.has_value())
		{
			const TerraEngine::Profiling::PhysicsWorkVolumeRecord& volume = *frame.physicsWorkVolume;
			++summary.workVolumes.measuredTickCount;
			summary.workVolumes.totalBodies = std::max(summary.workVolumes.totalBodies, volume.totalBodyCount);
			summary.workVolumes.activeBodies = std::max(summary.workVolumes.activeBodies, volume.activeBodyCount);
			summary.workVolumes.examinedBodies = std::max(summary.workVolumes.examinedBodies, volume.examinedBodyCount);
			summary.workVolumes.extractedBodies = std::max(summary.workVolumes.extractedBodies, volume.extractedBodyCount);
			summary.workVolumes.convertedBodies = std::max(summary.workVolumes.convertedBodies, volume.convertedBodyCount);
			summary.workVolumes.changedBodies = std::max(summary.workVolumes.changedBodies, volume.changedBodyCount);
			summary.workVolumes.publishedBodies = std::max(summary.workVolumes.publishedBodies, volume.publishedBodyCount);
		}
	}
	if (summary.frameCount != 0u)
	{
		summary.averageFrameMs = summary.totalDurationMs / static_cast<double>(summary.frameCount);
	}
	if (!completeFrameDurations.empty())
	{
		// A recovered partial tail is useful lifecycle evidence, but it must not
		// enter timing percentiles as if it were a fully observed frame.
		std::sort(completeFrameDurations.begin(), completeFrameDurations.end());
		summary.frameMedianMs = PercentileFromSorted(completeFrameDurations, 0.50);
		summary.frameP95Ms = PercentileFromSorted(completeFrameDurations, 0.95);
		summary.frameP99Ms = PercentileFromSorted(completeFrameDurations, 0.99);
		summary.frameWorstMs = completeFrameDurations.back();
	}
	summary.timingStreams = TerraEngine::Profiling::BuildTimingStreamAggregates(document.frames);
	summary.stageTimings = FinalizeMetrics(stageAccumulators);
	summary.protectedBoundaryTimings = FinalizeMetrics(boundaryAccumulators);
	summary.presentationWaitTimings = FinalizeMetrics(presentationAccumulators);
	summary.responsivenessTimings = FinalizeMetrics(responsivenessAccumulators);
	for (auto& [workerId, accumulator] : workerAccumulators)
	{
		accumulator.summary.runDuration = accumulator.run.Finalize();
		accumulator.summary.waitDuration = accumulator.wait.Finalize();
		accumulator.summary.idleDuration = accumulator.idle.Finalize();
		summary.workerHistories.push_back(std::move(accumulator.summary));
	}
	return summary;
}

std::string EditorProfilerService::MakeArtifactId(std::string_view fileName)
{
	// FNV-1a produces a stable opaque identifier from the project-owned filename.
	// It is an identity token, not a security primitive; containment is still
	// revalidated by ProfilerCaptureStore before every read.
	std::uint64_t hash = 14695981039346656037ull;
	for (const unsigned char character : fileName)
	{
		hash ^= character;
		hash *= 1099511628211ull;
	}
	std::ostringstream stream;
	stream << "profiler-" << std::hex << std::setw(16) << std::setfill('0') << hash;
	return stream.str();
}

std::optional<std::filesystem::path> EditorProfilerService::FindCapturePathLocked(std::string_view artifactId) const
{
	for (const auto& capture : captures)
	{
		if (MakeArtifactId(capture.fileName) == artifactId)
			return capture.absolutePath;
	}
	return std::nullopt;
}

// EditorProfilerService::HasActiveProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::HasActiveProject() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return captureStore.HasProjectRoot();
}

// EditorProfilerService::IsBusy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::IsBusy() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return busy;
}

// EditorProfilerService::GetProfilerDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path EditorProfilerService::GetProfilerDirectory() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return captureStore.GetProfilerDirectory();
}

// EditorProfilerService::MakeMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Profiling::ProfilerCaptureMetadata EditorProfilerService::MakeMetadata() const
{
	TerraEngine::Profiling::ProfilerCaptureMetadata metadata;
	metadata.terraEngineVersion = "TerraEngine";
	metadata.startedAt = TerraEngine::Profiling::MakeReadableTimestamp();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx && ctx->projectManager && ctx->projectManager->HasActiveProject())
	{
		const auto root = ctx->projectManager->GetProjectRoot();
		metadata.projectName = root.filename().string();
		metadata.projectPathHint = root.string();
	}
	return metadata;
}
