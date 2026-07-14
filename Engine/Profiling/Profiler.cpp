// Profiler.cpp — Contains the Profiler file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/Profiler.h"

#include "Physics/PhysicsDiagnostics.h"
#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace TerraEngine::Profiling
{
namespace
{
// struct ThreadSampleState groups related state and operations so callers have one documented contract.
struct ThreadSampleState
{
	std::vector<ProfilerSample> openSamples;
	std::unordered_set<std::uint64_t> fullyExcludedSamples;
	std::chrono::steady_clock::time_point editorWorkStart{};
	std::uint32_t editorWorkDepth = 0u;
};

thread_local ThreadSampleState t_sampleState;

// CurrentThreadLabel is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string CurrentThreadLabel()
{
	std::ostringstream stream;
	stream << std::this_thread::get_id();
	return stream.str();
}

// FindModule is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerModuleSummary* FindModule(std::vector<ProfilerModuleSummary>& modules, ProfilerModuleId moduleId)
{
	for (ProfilerModuleSummary& module : modules)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (module.moduleId == moduleId)
		{
			return &module;
		}
	}
	return nullptr;
}

// AddOrReplaceMetric is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AddOrReplaceMetric(ProfilerModuleSummary& module, std::string name, double value, std::string unit)
{
	for (ProfilerMetric& metric : module.metrics)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metric.name == name)
		{
			metric.value = value;
			metric.unit = std::move(unit);
			return;
		}
	}
	module.metrics.push_back({std::move(name), value, std::move(unit)});
}

ProfilerTimingStreamSample MakeTimingSample(
	std::string id,
	std::string label,
	ProfilerTimingStreamScope scope,
	ProfilerTimingStreamSource source,
	ProfilerTimingStreamStatus status,
	double milliseconds,
	std::string reason = {})
{
	ProfilerTimingStreamSample sample;
	sample.id = std::move(id);
	sample.label = std::move(label);
	sample.scope = scope;
	sample.source = source;
	sample.status = status;
	sample.milliseconds = status == ProfilerTimingStreamStatus::Valid ? milliseconds : 0.0;
	sample.statusReason = std::move(reason);
	return sample;
}

// Viewport CPU timing must come from profiler scopes instead of renderer frame
// stats. RenderStatsSnapshot::frameTimeMs mirrors the view's frame delta and can
// be much larger than the CPU work done by the viewport submission itself.
bool IsViewportCpuSubmitSample(const ProfilerSample& sample)
{
	if (sample.moduleId != ProfilerModuleId::CpuEngine)
	{
		return false;
	}
	return sample.name == "RenderSystem: Submit Scene View"
		|| sample.name == "RenderSystem: Submit Game View Cameras"
		|| sample.name == "RenderSystem: Submit Blank Game View";
}

bool TryMeasureViewportCpuMilliseconds(const ProfilerFrameRecord& frame, double& viewportCpuMs)
{
	viewportCpuMs = 0.0;
	bool foundViewportSubmit = false;
	for (const ProfilerSample& sample : frame.samples)
	{
		if (!IsViewportCpuSubmitSample(sample))
		{
			continue;
		}
		foundViewportSubmit = true;
		viewportCpuMs += std::max(0.0, sample.durationMs);
	}
	return foundViewportSubmit;
}
}

// Profiler::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Profiler& Profiler::Instance()
{
	static Profiler profiler;
	return profiler;
}

// Profiler::StartRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::StartRecording(const ProfilerCaptureMetadata& metadata, const ProfilerCaptureSettings& requestedSettings)
{
	std::lock_guard<std::mutex> lock(mutex);
	settings = requestedSettings;
	settings.captureMode = ProfilerCaptureMode::LiveRolling;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!AnyModuleEnabled(settings))
	{
		settings.cpuEngineEnabled = true;
	}

	document = {};
	document.metadata = metadata;
	document.metadata.settings = settings;
	document.metadata.startedAt = document.metadata.startedAt.empty() ? MakeReadableTimestamp() : document.metadata.startedAt;
	document.frames.clear();
	document.sourcePath.reset();
	currentFrame.reset();
	selectedFrameIndex.reset();
	sessionStart = std::chrono::steady_clock::now();
	currentFrameStart = sessionStart;
	currentExcludedEditorMs = 0.0;
	nextFrameIndex = 0u;
	nextSampleId = 1u;
	queuedTaskCount = 0u;
	startedTaskCount = 0u;
	finishedTaskCount = 0u;
	lastError.clear();
	statusText = "Recording";
	state = ProfilerSessionState::Recording;
	UpdateFastRecordingFlagsLocked();
}

// Profiler::PauseRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::PauseRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state == ProfilerSessionState::Recording)
	{
		state = ProfilerSessionState::Paused;
		statusText = "Paused";
		UpdateFastRecordingFlagsLocked();
	}
}

// Profiler::ResumeRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::ResumeRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state == ProfilerSessionState::Paused)
	{
		state = ProfilerSessionState::Recording;
		statusText = "Recording";
		UpdateFastRecordingFlagsLocked();
	}
}

// Profiler::FinishRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureDocument Profiler::FinishRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		currentFrame->complete = false;
		AggregateSamplesForFrame(*currentFrame);
		document.frames.push_back(*currentFrame);
		currentFrame.reset();
	}

	TrimFrameBuffer(document.frames, settings);
	document.metadata.savedAt = MakeReadableTimestamp();
	document.metadata.settings.captureMode = ProfilerCaptureMode::FinalizingToArtifact;
	state = ProfilerSessionState::Finalizing;
	statusText = "Finalizing capture";
	UpdateFastRecordingFlagsLocked();
	return document;
}

// Profiler::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	document = {};
	currentFrame.reset();
	selectedFrameIndex.reset();
	t_sampleState.openSamples.clear();
	t_sampleState.fullyExcludedSamples.clear();
	t_sampleState.editorWorkDepth = 0u;
	currentExcludedEditorMs = 0.0;
	state = ProfilerSessionState::Idle;
	statusText = "Idle";
	lastError.clear();
	UpdateFastRecordingFlagsLocked();
}

// Profiler::LoadDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::LoadDocument(const ProfilerCaptureDocument& loadedDocument)
{
	std::lock_guard<std::mutex> lock(mutex);
	document = loadedDocument;
	settings = document.metadata.settings;
	settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;
	currentFrame.reset();
	selectedFrameIndex = document.frames.empty() ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{document.frames.front().frameIndex};
	state = ProfilerSessionState::Loaded;
	statusText = "Loaded capture";
	lastError.clear();
	UpdateFastRecordingFlagsLocked();
}

// Profiler::BeginFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::BeginFrame(bool playModeActive)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording)
	{
		return;
	}
	if (!playModeActive)
	{
		// Record remains armed outside Play Mode, but no partial editor-preview
		// frame or hot-path instrumentation is allowed to survive this boundary.
		currentFrame.reset();
		t_sampleState.openSamples.clear();
		t_sampleState.fullyExcludedSamples.clear();
		t_sampleState.editorWorkDepth = 0u;
		currentExcludedEditorMs = 0.0;
		statusText = "Recording (waiting for Play Mode)";
		UpdateFastRecordingFlagsLocked();
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		currentFrame->complete = false;
		AggregateSamplesForFrame(*currentFrame);
		document.frames.push_back(*currentFrame);
	}

	currentFrameStart = std::chrono::steady_clock::now();
	currentExcludedEditorMs = 0.0;
	ProfilerFrameRecord frame;
	frame.frameIndex = nextFrameIndex++;
	frame.frameStartMs = NowMs();
	frame.complete = true;
	frame.moduleSummaries = MakeDefaultModuleSummaries(settings);
	currentFrame = std::move(frame);
	t_sampleState.openSamples.clear();
	t_sampleState.fullyExcludedSamples.clear();
	t_sampleState.editorWorkDepth = 0u;
	statusText = "Recording";
	UpdateFastRecordingFlagsLocked();
}

// Profiler::EndFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndFrame()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	const double wallDurationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - currentFrameStart).count();
	const double excludedEditorMs = std::clamp(currentExcludedEditorMs, 0.0, wallDurationMs);
	// Profiler frame statistics intentionally describe the runnable game/engine
	// portion of the frame. Editor UI time is internal subtraction bookkeeping.
	currentFrame->durationMs = std::max(0.0, wallDurationMs - excludedEditorMs);
	AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
		"engine_cpu",
		"Engine CPU",
		ProfilerTimingStreamScope::EngineCpu,
		ProfilerTimingStreamSource::ProfilerClock,
		ProfilerTimingStreamStatus::Valid,
		currentFrame->durationMs));
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (!t_sampleState.openSamples.empty())
	{
		ProfilerSample sample = t_sampleState.openSamples.back();
		t_sampleState.openSamples.pop_back();
		sample.complete = false;
		const bool fullyExcluded = t_sampleState.fullyExcludedSamples.erase(sample.sampleId) != 0u;
		// Sample starts use the same compressed, editor-excluded clock as the
		// frame duration, so incomplete samples cannot extend past the frame.
		sample.durationMs = std::max(0.0, currentFrame->durationMs - sample.startMs);
		if (fullyExcluded)
		{
			continue;
		}
		currentFrame->samples.push_back(std::move(sample));
	}
	double viewportCpuMs = 0.0;
	if (TryMeasureViewportCpuMilliseconds(*currentFrame, viewportCpuMs))
	{
		AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
			"viewport_cpu",
			"Viewport CPU",
			ProfilerTimingStreamScope::ViewportCpu,
			ProfilerTimingStreamSource::ProfilerClock,
			ProfilerTimingStreamStatus::Valid,
			viewportCpuMs));
	}
	// Capture compact immutable physics diagnostics at the ordinary profiler
	// frame boundary. This producer is independent of whether the optional
	// Physics Diagnostics editor window has ever been opened.
	if (const auto physics = TerraEngine::Physics::AcquirePhysicsDiagnosticsSnapshot())
	{
		ProfilerPhysicsFrameCounters counters;
		counters.tick = physics->tick;
		counters.bodyCount = physics->counters.bodyCount;
		counters.contactCount = physics->counters.contactCount;
		counters.queryCount = physics->counters.queryCount;
		counters.eventCount = physics->counters.eventCount;
		counters.rejectedCount = physics->counters.rejectedCount;
		counters.staleCount = physics->counters.staleCount;
		counters.commandQueueDepth = physics->queues.commandDepth;
		counters.shapeCacheBytes = physics->cache.payloadBytes;
		counters.shapeCacheHits = physics->cache.hitCount;
		counters.shapeCacheMisses = physics->cache.missCount;
		counters.snapshotBytes = physics->memory.snapshotBytes;
		counters.snapshotHighWaterBytes = physics->memory.highWaterBytes;
		counters.providerAllocations = physics->memory.outstandingProviderAllocations;
		counters.providerAllocationHighWater = physics->memory.highWaterProviderAllocations;
		counters.prepareMs = physics->timings.prepareMs;
		counters.commitMs = physics->timings.commitMs;
		counters.simulateMs = physics->timings.simulateMs;
		counters.queryMs = physics->timings.queryMs;
		counters.publishMs = physics->timings.publishMs;
		currentFrame->physics = counters;
	}
	AggregateSamplesForFrame(*currentFrame);
	document.frames.push_back(*currentFrame);
	TrimFrameBuffer(document.frames, settings);
	currentFrame.reset();
	UpdateFastRecordingFlagsLocked();
}

bool Profiler::BeginEditorWork()
{
	// The atomic check keeps the normal non-recording editor path lock-free.
	if (!frameRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return false;
	}
	if (t_sampleState.editorWorkDepth++ == 0u)
	{
		t_sampleState.editorWorkStart = std::chrono::steady_clock::now();
		if (!t_sampleState.openSamples.empty()
			&& t_sampleState.openSamples.back().taskLabel == "EditorSystem")
		{
			// The immediate enclosing scheduler scope represents EditorSystem itself.
			// Drop that wrapper completely; its parent remains an engine sample whose
			// duration is reduced by the excluded editor interval below.
			t_sampleState.fullyExcludedSamples.insert(t_sampleState.openSamples.back().sampleId);
		}
	}
	return true;
}

void Profiler::EndEditorWork(bool active)
{
	if (!active)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (t_sampleState.editorWorkDepth == 0u)
	{
		return;
	}
	if (--t_sampleState.editorWorkDepth != 0u)
	{
		return;
	}
	const double durationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t_sampleState.editorWorkStart).count();
	if (state == ProfilerSessionState::Recording && currentFrame.has_value())
	{
		currentExcludedEditorMs += std::max(0.0, durationMs);
	}
}

// Profiler::BeginSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel)
{
	// Most frames run with profiling idle. Check the cheap flag first so the
	// many RAII scope markers compiled into engine systems avoid a mutex round
	// trip until a real capture is recording.
	if (!markerRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}
	if (t_sampleState.editorWorkDepth != 0u)
	{
		// Editor work is outside the game profiler. Do not let nested ImGui,
		// managed editor, or editor-presentation scopes enter CPU Engine tables.
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value() || !settings.recordMarkers)
	{
		return false;
	}

	ProfilerSample sample;
	sample.sampleId = nextSampleId++;
	sample.parentSampleId = t_sampleState.openSamples.empty() ? 0u : t_sampleState.openSamples.back().sampleId;
	sample.moduleId = moduleId;
	sample.name = name != nullptr ? name : "Unnamed Sample";
	sample.threadLabel = CurrentThreadLabel();
	sample.taskLabel = taskLabel != nullptr ? taskLabel : "";
	const double wallStartMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - currentFrameStart).count();
	// Compress the timeline by editor time already completed this frame. Every
	// sample start and duration then shares the same game-only frame clock.
	sample.startMs = std::max(0.0, wallStartMs - currentExcludedEditorMs);
	sample.depth = static_cast<std::uint32_t>(t_sampleState.openSamples.size());
	t_sampleState.openSamples.push_back(std::move(sample));
	return true;
}

// Profiler::EndSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndSample()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!currentFrame.has_value() || t_sampleState.openSamples.empty())
	{
		return;
	}

	ProfilerSample sample = t_sampleState.openSamples.back();
	t_sampleState.openSamples.pop_back();
	const bool fullyExcluded = t_sampleState.fullyExcludedSamples.erase(sample.sampleId) != 0u;
	// If recording stopped while a worker-owned scope was open, discard that
	// unfinished sample after popping it so thread-local stacks do not leak
	// stale parent ids into a later capture.
	if (state != ProfilerSessionState::Recording)
	{
		return;
	}
	const double wallEndMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - currentFrameStart).count();
	const double gameEndMs = std::max(0.0, wallEndMs - currentExcludedEditorMs);
	sample.durationMs = std::max(0.0, gameEndMs - sample.startMs);
	if (fullyExcluded)
	{
		return;
	}
	currentFrame->samples.push_back(std::move(sample));
}

// Profiler::RecordRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordRenderStats(const RenderStatsSnapshot& gameStats, const RenderStatsSnapshot& sceneStats)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!currentFrame.has_value() || state != ProfilerSessionState::Recording)
	{
		return;
	}

	ProfilerModuleSummary* engineCpu = FindModule(currentFrame->moduleSummaries, ProfilerModuleId::CpuEngine);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (engineCpu == nullptr)
	{
		return;
	}
	engineCpu->available = gameStats.valid || sceneStats.valid || engineCpu->available;
	// RenderStatsSnapshot::frameTimeMs is the input delta copied into a delayed,
	// completed renderer snapshot. It does not measure CPU work in this profiler
	// frame, so exposing it here would create two unrelated "frame time" values.
	// The immediate viewport overlays remain the owner of that renderer diagnostic.
	AddOrReplaceMetric(*engineCpu, "Game draw calls", static_cast<double>(gameStats.drawCalls), "count");
	AddOrReplaceMetric(*engineCpu, "Scene draw calls", static_cast<double>(sceneStats.drawCalls), "count");
	AddOrReplaceMetric(*engineCpu, "Game triangles", static_cast<double>(gameStats.triangleCount), "count");
	AddOrReplaceMetric(*engineCpu, "Scene triangles", static_cast<double>(sceneStats.triangleCount), "count");

	const bool gameGpuValid = gameStats.valid && gameStats.gpuTimestampValid;
	const bool sceneGpuValid = sceneStats.valid && sceneStats.gpuTimestampValid;
	const bool viewportGpuValid = gameGpuValid || sceneGpuValid;
	const double viewportGpuMs =
		(gameGpuValid ? static_cast<double>(gameStats.gpuTimestampMs) : 0.0)
		+ (sceneGpuValid ? static_cast<double>(sceneStats.gpuTimestampMs) : 0.0);
	std::string viewportGpuReason;
	if (!viewportGpuValid)
	{
		const auto appendReason = [&viewportGpuReason](const std::string& reason) {
			if (reason.empty())
			{
				return;
			}
			if (!viewportGpuReason.empty())
			{
				viewportGpuReason += " ";
			}
			viewportGpuReason += reason;
		};
		appendReason(gameStats.valid ? gameStats.gpuTimestampReason : std::string{});
		appendReason(sceneStats.valid ? sceneStats.gpuTimestampReason : std::string{});
		if (viewportGpuReason.empty())
		{
			viewportGpuReason = "No viewport GPU timestamp was published for this frame.";
		}
	}
	AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
		"viewport_gpu",
		"Viewport GPU",
		ProfilerTimingStreamScope::ViewportGpu,
		viewportGpuValid ? ProfilerTimingStreamSource::BackendGpuTimestamp : ProfilerTimingStreamSource::Unavailable,
		viewportGpuValid ? ProfilerTimingStreamStatus::Valid : ProfilerTimingStreamStatus::Unavailable,
		viewportGpuMs,
		viewportGpuReason));
}

// Profiler::RecordTaskQueued is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskQueued()
{
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !settings.taskExecutionEnabled)
	{
		return;
	}
	++queuedTaskCount;
}

// Profiler::RecordTaskStarted is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::RecordTaskStarted(const char* taskLabel)
{
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (state != ProfilerSessionState::Recording || !settings.taskExecutionEnabled)
		{
			return false;
		}
		++startedTaskCount;
	}
	return BeginSample("ThreadPool Task", ProfilerModuleId::TaskExecution, taskLabel);
}

// Profiler::RecordTaskFinished is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskFinished(bool sampleActive)
{
	if (sampleActive)
	{
		EndSample();
	}
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !settings.taskExecutionEnabled)
	{
		return;
	}
	++finishedTaskCount;
}

// Profiler::SelectFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::SelectFrame(std::uint64_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex);
	selectedFrameIndex = frameIndex;
}

// Profiler::GetState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerSessionState Profiler::GetState() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return state;
}

// Profiler::GetSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureSettings Profiler::GetSettings() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return settings;
}

// Profiler::GetDocumentSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureDocument Profiler::GetDocumentSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	ProfilerCaptureDocument snapshot = document;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		snapshot.frames.push_back(*currentFrame);
	}
	return snapshot;
}

// Profiler::GetFrameListSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<ProfilerFrameListItem> Profiler::GetFrameListSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<ProfilerFrameListItem> frames;
	frames.reserve(document.frames.size() + (currentFrame.has_value() ? 1u : 0u));
	auto appendFrame = [&frames](const ProfilerFrameRecord& frame) {
		ProfilerFrameListItem item;
		item.frameIndex = frame.frameIndex;
		item.durationMs = frame.durationMs;
		item.complete = frame.complete;
		frames.push_back(item);
	};
	for (const ProfilerFrameRecord& frame : document.frames)
	{
		appendFrame(frame);
	}
	if (currentFrame.has_value())
	{
		appendFrame(*currentFrame);
	}
	return frames;
}

// Profiler::GetFramesSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<ProfilerFrameRecord> Profiler::GetFramesSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<ProfilerFrameRecord> frames = document.frames;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		frames.push_back(*currentFrame);
	}
	return frames;
}

// Profiler::GetSelectedFrameSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<ProfilerFrameRecord> Profiler::GetSelectedFrameSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!selectedFrameIndex.has_value())
	{
		if (!document.frames.empty())
		{
			return document.frames.back();
		}
		return currentFrame;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ProfilerFrameRecord& frame : document.frames)
	{
		if (frame.frameIndex == *selectedFrameIndex)
		{
			return frame;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value() && currentFrame->frameIndex == *selectedFrameIndex)
	{
		return currentFrame;
	}
	return std::nullopt;
}

// Profiler::GetSourcePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<std::filesystem::path> Profiler::GetSourcePath() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return document.sourcePath;
}

// Profiler::GetLastError is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Profiler::GetLastError() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return lastError;
}

// Profiler::GetStatusText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Profiler::GetStatusText() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return statusText;
}

// Profiler::IsCapturing is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::IsCapturing() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return state == ProfilerSessionState::Recording || state == ProfilerSessionState::Paused;
}

// Profiler::NowMs is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
double Profiler::NowMs() const
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sessionStart).count();
}

// Profiler::AggregateSamplesForFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::AggregateSamplesForFrame(ProfilerFrameRecord& frame) const
{
	for (ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		module.totalMs = 0.0;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ProfilerSample& sample : frame.samples)
	{
		if (ProfilerModuleSummary* module = FindModule(frame.moduleSummaries, sample.moduleId))
		{
			module->available = true;
			module->totalMs += sample.durationMs;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ProfilerModuleSummary* engineCpu = FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine))
	{
		engineCpu->available = settings.cpuEngineEnabled;
		AddOrReplaceMetric(*engineCpu, "Frame duration", frame.durationMs, "ms");
		AddOrReplaceMetric(*engineCpu, "Engine sample count", static_cast<double>(std::count_if(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& sample) {
			return sample.moduleId == ProfilerModuleId::CpuEngine;
		})), "count");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ProfilerModuleSummary* tasks = FindModule(frame.moduleSummaries, ProfilerModuleId::TaskExecution))
	{
		tasks->available = settings.taskExecutionEnabled;
		AddOrReplaceMetric(*tasks, "Queued tasks", static_cast<double>(queuedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Started tasks", static_cast<double>(startedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Finished tasks", static_cast<double>(finishedTaskCount), "count");
	}

	std::sort(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& left, const ProfilerSample& right) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (left.startMs == right.startMs)
		{
			return left.depth < right.depth;
		}
		return left.startMs < right.startMs;
	});
}

// Profiler::UpdateFastRecordingFlagsLocked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::UpdateFastRecordingFlagsLocked()
{
	// An armed recording outside Play Mode owns no current frame. Keeping all
	// fast flags false there prevents editor-preview instrumentation from paying
	// profiler synchronization costs or leaking counters into the next play frame.
	const bool frameActive = state == ProfilerSessionState::Recording && currentFrame.has_value();
	frameRecordingActive.store(frameActive, std::memory_order_release);
	markerRecordingActive.store(frameActive && settings.recordMarkers, std::memory_order_release);
	taskRecordingActive.store(frameActive && settings.taskExecutionEnabled, std::memory_order_release);
}
}
