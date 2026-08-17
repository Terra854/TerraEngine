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
#include <cmath>
#include <iomanip>
#include <sstream>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerCaptureMetadata;
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

}

// EditorProfilerService::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Initialize(EditorContext& context)
{
	ctx = &context;
	{
		std::lock_guard<std::mutex> lock(mutex);
		statusText = "No project open";
		PublishPresentationLocked();
	}
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
		selectedCapture.reset();
		selectedSummary.reset();
		selectedIndexPage.reset();
		selectedFrame.reset();
		selectedFrameArtifactId.clear();
		decodedDetailCache.clear();
		pendingIndexRequest.reset();
		pendingDetailRequest.reset();
		activeJournalPath.reset();
		activeSessionArtifactId.clear();
		finalizationTicket.reset();
		operations = {};
		sessionAccepting = true;
		discardedStaleCompletionCount = 0u;
		health = {};
		healthPublishAccumulatorSeconds = 0.0f;
		statusText = "Profiler ready";
		lastError.clear();
		PublishPresentationLocked();
	}
	RefreshCaptureList();
}

// EditorProfilerService::BeginProjectClose invalidates work before the caller
// starts destroying scene resources. A worker can finish its filesystem call,
// but its old epoch can no longer publish a document or UI result.
void EditorProfilerService::BeginProjectClose()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		++projectGeneration;
		if (cancellationToken)
		{
			cancellationToken->store(true, std::memory_order_release);
		}
		captureStore.ClearProjectRoot();
		captures.clear();
		lastSavedCaptureArtifact.reset();
		selectedCapture.reset();
		selectedSummary.reset();
		selectedIndexPage.reset();
		selectedFrame.reset();
		selectedFrameArtifactId.clear();
		decodedDetailCache.clear();
		pendingIndexRequest.reset();
		pendingDetailRequest.reset();
		activeJournalPath.reset();
		activeSessionArtifactId.clear();
		finalizationTicket.reset();
		// Preserve cancelled terminal operation states in the no-project snapshot.
		// A caller that polls during close can distinguish cancellation from a
		// successful finalization without retaining any capture history.
		for (EditorProfilerOperationStatus& operation : operations)
		{
			if (operation.active)
			{
				operation.active = false;
				operation.state = EditorProfilerOperationState::Cancelled;
			}
		}
		sessionAccepting = false;
		health = {};
		healthPublishAccumulatorSeconds = 0.0f;
		statusText = "No project open";
		lastError.clear();
		// StartRecording holds this same lock while creating an engine journal.
		// Clearing here closes the only gap where a newly opened project could be
		// cleared by the outgoing project's deferred engine cleanup.
		Profiler::Instance().Clear();
		PublishPresentationLocked();
	}
}

// EditorProfilerService::OnProjectClosed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::OnProjectClosed()
{
	BeginProjectClose();
}

// EditorProfilerService::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Update(float deltaTime)
{
	std::deque<WorkerCompletion> completions;
	{
		std::lock_guard<std::mutex> lock(completionMutex);
		completions.swap(pendingCompletions);
	}

	// This is the sole crossing from worker-owned file data to editor-owned
	// profiler/session state. Keeping it on the protected EditorSystem update
	// prevents a project close or a newer request from seeing a partial result.
	for (WorkerCompletion& completion : completions)
	{
		AcceptWorkerCompletion(std::move(completion));
	}

	// A request is queued while its worker is being submitted.  The protected
	// boundary advances it to running so automation can observe a truthful,
	// bounded lifecycle without a worker mutating editor-owned state directly.
	{
		std::lock_guard<std::mutex> lock(mutex);
		bool advanced = false;
		for (EditorProfilerOperationStatus& operation : operations)
		{
			if (operation.active && operation.state == EditorProfilerOperationState::Queued)
			{
				operation.state = EditorProfilerOperationState::Running;
				advanced = true;
			}
		}
		if (advanced)
		{
			PublishPresentationLocked();
		}
	}

	// Health is a bounded scalar snapshot, unlike frame history. Refresh it four
	// times per second so an open Profiler remains useful during long captures
	// without turning the UI into a per-frame consumer of mutable engine state.
	healthPublishAccumulatorSeconds += std::max(0.0f, deltaTime);
	if (healthPublishAccumulatorSeconds >= 0.25f)
	{
		healthPublishAccumulatorSeconds = 0.0f;
		const auto liveState = Profiler::Instance().GetState();
		if (liveState == ProfilerSessionState::Recording
			|| liveState == ProfilerSessionState::Paused
			|| liveState == ProfilerSessionState::Finalizing)
		{
			const auto liveHealth = Profiler::Instance().GetCaptureHealth();
			std::lock_guard<std::mutex> lock(mutex);
			// A different session can begin after project close. Do not publish its
			// health into an editor snapshot that no longer owns a project.
			if (sessionAccepting)
			{
				health = liveHealth;
				PublishPresentationLocked();
			}
		}
	}

	std::optional<TerraEngine::Profiling::ProfilerJournalFinalizationTicket> ticket;
	{
		std::lock_guard<std::mutex> lock(mutex);
		ticket = finalizationTicket;
	}
	if (!ticket.has_value())
	{
		return;
	}
	const auto status = Profiler::Instance().GetJournalFinalizationStatus(*ticket);
	std::optional<std::filesystem::path> finalizedPath;
	ProfilerCaptureStore finalizedStore;
	std::shared_ptr<std::atomic_bool> finalizedCancelToken;
	std::uint64_t finalizedProjectEpoch = 0u;
	std::uint64_t finalizedOperationId = 0u;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!finalizationTicket.has_value()
			|| finalizationTicket->journalGeneration != ticket->journalGeneration)
		{
			return;
		}
		health = status.health;
		if (status.state == TerraEngine::Profiling::ProfilerJournalFinalizationState::Completed)
		{
			finalizationTicket.reset();
			if (!activeJournalPath.has_value())
			{
				auto& operation = operations[static_cast<std::size_t>(EditorProfilerOperationKind::Persisting) - 1u];
				operation.active = false;
				operation.state = EditorProfilerOperationState::Failed;
				lastError = "The finalized profiler journal path is unavailable.";
				statusText = "Profiler journal finalization failed";
			}
			else
			{
				// Finalization only proves the writer reached its terminal record. Keep
				// the persistence operation active while a worker validates the V1
				// index and publishes the exact artifact identity used by automation.
				finalizedPath = *activeJournalPath;
				finalizedStore = captureStore;
				finalizedCancelToken = cancellationToken;
				finalizedProjectEpoch = projectGeneration;
				finalizedOperationId = operations[static_cast<std::size_t>(
					EditorProfilerOperationKind::Persisting) - 1u].operationId;
				statusText = "Validating finalized profiler journal";
				lastError.clear();
			}
			PublishPresentationLocked();
		}
		else if (status.state == TerraEngine::Profiling::ProfilerJournalFinalizationState::Failed
			|| status.state == TerraEngine::Profiling::ProfilerJournalFinalizationState::Stale)
		{
			auto& operation = operations[static_cast<std::size_t>(EditorProfilerOperationKind::Persisting) - 1u];
			operation.active = false;
			operation.state = EditorProfilerOperationState::Failed;
			finalizationTicket.reset();
			lastError = status.error.empty() ? "Profiler journal finalization failed." : status.error;
			statusText = "Profiler journal finalization failed";
			PublishPresentationLocked();
		}
	}
	if (finalizedPath.has_value())
	{
		std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
		auto task = ThreadPool::instance().createTask([
			weakSelf,
			store = std::move(finalizedStore),
			cancel = std::move(finalizedCancelToken),
			epoch = finalizedProjectEpoch,
			operationId = finalizedOperationId,
			path = *finalizedPath]() mutable {
			if (cancel && cancel->load(std::memory_order_acquire)) return;
			TerraEngine::Profiling::ProfilerJournalV1OpenResult journal;
			std::string error;
			if (!store.ReadJournalIndex(path, journal, error))
			{
				if (auto self = weakSelf.lock())
				{
					self->EnqueueWorkerCompletion({WorkerCompletionKind::OperationFailed,
						EditorProfilerOperationKind::Persisting, epoch, operationId, {}, {}, std::move(error)});
				}
				return;
			}
			if (cancel && cancel->load(std::memory_order_acquire)) return;
			TerraEngine::Profiling::ProfilerCaptureArtifact artifact;
			artifact.fileName = path.filename().string();
			artifact.absolutePath = path;
			artifact.metadata = journal.metadata;
			artifact.frameCount = journal.summary.frameCount;
			artifact.committedFrameCount = journal.summary.frameCount;
			artifact.complete = journal.complete;
			artifact.recoveredIncompleteTail = journal.recoveredIncompleteTail;
			artifact.supported = true;
			artifact.status = journal.recoveredIncompleteTail
				? "Recovered incomplete tail" : (journal.complete ? "Ready" : "Incomplete");
			WorkerCompletion completion;
			completion.kind = WorkerCompletionKind::CapturePersisted;
			completion.operationKind = EditorProfilerOperationKind::Persisting;
			completion.projectEpoch = epoch;
			completion.operationId = operationId;
			completion.savedCaptureArtifact = EditorProfilerCaptureArtifact{
				MakeArtifactId(artifact.fileName), std::move(artifact)};
			if (auto self = weakSelf.lock()) self->EnqueueWorkerCompletion(std::move(completion));
		});
		ThreadPool::instance().enqueue(task);
	}
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
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> startCancelToken;
	std::uint64_t startProjectEpoch = 0u;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!sessionAccepting || !captureStore.HasProjectRoot())
		{
			lastError = "Open a project before profiling.";
			statusText = lastError;
			PublishPresentationLocked();
			return;
		}
		storeSnapshot = captureStore;
		startCancelToken = cancellationToken;
		startProjectEpoch = projectGeneration;
	}
	if (startCancelToken && startCancelToken->load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = "Open a project before profiling.";
		statusText = lastError;
		PublishPresentationLocked();
		return;
	}

	const ProfilerCaptureMetadata metadata = MakeMetadata();
	std::filesystem::path journalPath;
	std::string startError;
	if (!storeSnapshot.ReserveJournalArtifact(metadata, journalPath, startError)
		|| (startCancelToken && startCancelToken->load(std::memory_order_acquire)))
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = startError.empty() ? "Failed to reserve the profiler V1 journal." : std::move(startError);
		statusText = "Profiler recording did not start";
		PublishPresentationLocked();
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!sessionAccepting || projectGeneration != startProjectEpoch
			|| (startCancelToken && startCancelToken->load(std::memory_order_acquire)))
		{
			// Reserve is worker-safe filesystem work. Starting the engine session is
			// serialized with project close so an outgoing epoch cannot create an
			// orphaned capture after BeginProjectClose clears the old session.
			return;
		}
		if (!Profiler::Instance().StartJournalRecording(metadata, settings, journalPath, startError))
		{
			lastError = startError.empty() ? "Failed to start the profiler V1 journal." : std::move(startError);
			statusText = "Profiler recording did not start";
			PublishPresentationLocked();
			return;
		}
		// A previous finalize-validation read may still be queued. It is safe to
		// let it finish, but it may no longer publish into this recording session.
		// A newly accepted recording supersedes a prior finalization validation.
		// Keep its terminal result explicit so a stale automation poll cannot
		// mistake the superseded request for a completed capture.
		auto& priorPersistence = operations[static_cast<std::size_t>(EditorProfilerOperationKind::Persisting) - 1u];
		if (priorPersistence.active)
		{
			priorPersistence.active = false;
			priorPersistence.state = EditorProfilerOperationState::Cancelled;
		}
		lastSavedCaptureArtifact.reset();
		// A recording session has no stable journal index yet. Retaining the last
		// loaded capture here would clone and re-sort its entire index every health
		// publication while the new capture is running.
		selectedCapture.reset();
		selectedSummary.reset();
		selectedIndexPage.reset();
		selectedFrame.reset();
		selectedFrameArtifactId.clear();
		decodedDetailCache.clear();
		pendingIndexRequest.reset();
		pendingDetailRequest.reset();
		activeJournalPath = std::move(journalPath);
		activeSessionArtifactId = MakeArtifactId(activeJournalPath->filename().string());
		finalizationTicket.reset();
		statusText = MakeRecordingStatus(settings);
		lastError.clear();
		PublishPresentationLocked();
	}
}

// EditorProfilerService::PauseRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::PauseRecording()
{
	Profiler::Instance().PauseRecording();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = "Paused";
	PublishPresentationLocked();
}

// EditorProfilerService::ResumeRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::ResumeRecording()
{
	Profiler::Instance().ResumeRecording();
	const ProfilerCaptureSettings settings = Profiler::Instance().GetSettings();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = MakeRecordingStatus(settings);
	PublishPresentationLocked();
}

void EditorProfilerService::PublishEditorFrameTiming(
	std::uint64_t frameIndex,
	double frameTimeMs) noexcept
{
	// Publish the value before its identity. Acquire-loading the identity makes
	// the paired duration visible without taking the capture-store mutex.
	editorFrameTimeMs.store(frameTimeMs, std::memory_order_relaxed);
	editorFrameIndex.store(frameIndex, std::memory_order_release);
}

void EditorProfilerService::PublishProfilerWindowDrawTiming(
	std::uint64_t frameIndex,
	double drawDurationMs) noexcept
{
	// Publish the duration before its frame identity. A later acquire load of the
	// identity therefore observes a completed ProfilerWindow::OnGui measurement.
	profilerWindowDrawMs.store(drawDurationMs, std::memory_order_relaxed);
	profilerWindowDrawFrameIndex.store(frameIndex, std::memory_order_release);
}

void EditorProfilerService::ClearProfilerWindowDrawTiming() noexcept
{
	// Hiding the window invalidates its previous sample. Without this reset, a
	// closed A/B measurement could accidentally report an earlier open frame.
	profilerWindowDrawMs.store(0.0, std::memory_order_relaxed);
	profilerWindowDrawFrameIndex.store(0u, std::memory_order_release);
}

void EditorProfilerService::PublishImGuiRenderTiming(
	std::uint64_t frameIndex,
	double durationMs) noexcept
{
	imguiRenderMs.store(durationMs, std::memory_order_relaxed);
	imguiRenderFrameIndex.store(frameIndex, std::memory_order_release);
}

void EditorProfilerService::PublishPresentUiTiming(
	std::uint64_t frameIndex,
	double durationMs) noexcept
{
	presentUiMs.store(durationMs, std::memory_order_relaxed);
	presentUiFrameIndex.store(frameIndex, std::memory_order_release);
}

EditorProfilerFrameTiming EditorProfilerService::GetEditorFrameTiming() const noexcept
{
	EditorProfilerFrameTiming result;
	result.frameIndex = editorFrameIndex.load(std::memory_order_acquire);
	result.frameTimeMs = editorFrameTimeMs.load(std::memory_order_relaxed);
	result.valid = result.frameIndex != 0u && std::isfinite(result.frameTimeMs)
		&& result.frameTimeMs > 0.0;
	result.profilerWindowDrawFrameIndex = profilerWindowDrawFrameIndex.load(std::memory_order_acquire);
	result.profilerWindowDrawMs = profilerWindowDrawMs.load(std::memory_order_relaxed);
	result.profilerWindowDrawValid = result.profilerWindowDrawFrameIndex != 0u
		&& std::isfinite(result.profilerWindowDrawMs) && result.profilerWindowDrawMs >= 0.0;
	result.imguiRenderFrameIndex = imguiRenderFrameIndex.load(std::memory_order_acquire);
	result.imguiRenderMs = imguiRenderMs.load(std::memory_order_relaxed);
	result.imguiRenderValid = result.imguiRenderFrameIndex != 0u
		&& std::isfinite(result.imguiRenderMs) && result.imguiRenderMs >= 0.0;
	result.presentUiFrameIndex = presentUiFrameIndex.load(std::memory_order_acquire);
	result.presentUiMs = presentUiMs.load(std::memory_order_relaxed);
	result.presentUiValid = result.presentUiFrameIndex != 0u
		&& std::isfinite(result.presentUiMs) && result.presentUiMs >= 0.0;
	return result;
}

// EditorProfilerService::FinishCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::FinishCapture()
{
	std::uint64_t finishProjectEpoch = 0u;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!sessionAccepting || !captureStore.HasProjectRoot()) return false;
		finishProjectEpoch = projectGeneration;
	}

	const auto ticket = Profiler::Instance().FinishRecording();
	if (!ticket.accepted)
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = "The active profiler session has no V1 journal to finalize.";
		statusText = "Profiler finalization was rejected";
		PublishPresentationLocked();
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex);
	if (!sessionAccepting || projectGeneration != finishProjectEpoch)
	{
		// BeginProjectClose already cleared the engine capture; never attach its
		// ticket to the project that won the lifecycle race.
		return false;
	}
	(void)BeginOperationLocked(EditorProfilerOperationKind::Persisting);
	finalizationTicket = ticket;
	lastSavedCaptureArtifact.reset();
	statusText = "Finalizing profiler journal";
	lastError.clear();
	PublishPresentationLocked();
	return true;
}

// EditorProfilerService::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Clear()
{
	Profiler::Instance().Clear();
	const bool hasProject = HasActiveProject();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = hasProject ? "Profiler ready" : "No project open";
	lastError.clear();
	PublishPresentationLocked();
}

// EditorProfilerService::RefreshCaptureList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::RefreshCaptureList()
{
	if (!HasActiveProject())
	{
		return;
	}

	std::uint64_t generation = 0u;
	std::uint64_t operationId = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		operationId = BeginOperationLocked(EditorProfilerOperationKind::Listing);
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		statusText = "Refreshing capture list";
		PublishPresentationLocked();
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, generation, operationId]() {
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
		self->EnqueueWorkerCompletion({
			WorkerCompletionKind::CaptureList,
			EditorProfilerOperationKind::Listing,
			generation,
			operationId,
			std::move(refreshed), {}, std::move(error)});
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
	std::uint64_t operationId = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		operationId = BeginOperationLocked(EditorProfilerOperationKind::Opening);
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		statusText = "Opening profiler capture";
		lastError.clear();
		PublishPresentationLocked();
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, generation, operationId, capturePath]() {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}
		ProfilerCaptureIndexPage page;
		std::string error;
		if (!storeSnapshot.ReadJournalIndexPage(capturePath,
			ProfilerCaptureStore::kNewestIndexPageOffset, 128u, page, error))
		{
			std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
			if (!self)
			{
				return;
			}
			self->EnqueueWorkerCompletion({
				WorkerCompletionKind::OperationFailed,
				EditorProfilerOperationKind::Opening,
				generation,
				operationId,
				{}, {}, std::move(error)});
			return;
		}
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		WorkerCompletion completion;
		completion.kind = WorkerCompletionKind::CaptureLoaded;
		completion.operationKind = EditorProfilerOperationKind::Opening;
		completion.projectEpoch = generation;
		completion.operationId = operationId;
		completion.capturePath = capturePath;
		EditorProfilerIndexPage editorPage;
		editorPage.offset = page.offset;
		editorPage.pageSize = page.pageSize;
		editorPage.totalFrameCount = page.totalFrameCount;
		editorPage.frames = std::move(page.frames);
		TerraEngine::Profiling::ProfilerCaptureArtifact artifact;
		artifact.fileName = capturePath.filename().string();
		artifact.absolutePath = capturePath;
		artifact.metadata = std::move(page.metadata);
		artifact.frameCount = page.summary.frameCount;
		artifact.committedFrameCount = page.summary.frameCount;
		artifact.complete = page.complete;
		artifact.recoveredIncompleteTail = page.recoveredIncompleteTail;
		artifact.supported = true;
		artifact.status = page.recoveredIncompleteTail ? "Recovered incomplete tail" : (page.complete ? "Ready" : "Incomplete");
		completion.selectedCapture = EditorProfilerCaptureArtifact{MakeArtifactId(artifact.fileName), std::move(artifact)};
		editorPage.artifactId = completion.selectedCapture->artifactId;
		completion.indexPage = std::move(editorPage);
		completion.selectedSummary = page.summary;
		self->EnqueueWorkerCompletion(std::move(completion));
	});
	ThreadPool::instance().enqueue(task);
}

void EditorProfilerService::RequestIndexPage(std::size_t offset, std::size_t pageSize)
{
	ProfilerCaptureStore store;
	std::filesystem::path path;
	std::shared_ptr<std::atomic_bool> cancel;
	std::uint64_t epoch = 0u;
	std::uint64_t operation = 0u;
	std::string artifactId;
	const std::size_t boundedPageSize = std::clamp<std::size_t>(
		pageSize, 1u, ProfilerCaptureStore::kMaximumIndexPageSize);
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!selectedCapture.has_value()) return;
		artifactId = selectedCapture->artifactId;
		if (selectedIndexPage.has_value()
			&& selectedIndexPage->artifactId == artifactId
			&& selectedIndexPage->offset == offset
			&& selectedIndexPage->pageSize == boundedPageSize)
		{
			if (pendingIndexRequest.has_value()
				&& (pendingIndexRequest->artifactId != artifactId
					|| pendingIndexRequest->offset != offset
					|| pendingIndexRequest->pageSize != boundedPageSize))
			{
				// Returning to the already published page is a newer user choice.
				// Retire the other in-flight page so its completion cannot make the
				// window jump back after the user navigated away from it.
				auto& retiredOperation = operations[static_cast<std::size_t>(
					EditorProfilerOperationKind::LoadingIndex) - 1u];
				retiredOperation.active = false;
				retiredOperation.state = EditorProfilerOperationState::Cancelled;
				pendingIndexRequest.reset();
			}
			return;
		}
		if (pendingIndexRequest.has_value()
			&& pendingIndexRequest->artifactId == artifactId
			&& pendingIndexRequest->offset == offset
			&& pendingIndexRequest->pageSize == boundedPageSize)
		{
			return;
		}
		store = captureStore;
		path = selectedCapture->artifact.absolutePath;
		cancel = cancellationToken;
		epoch = projectGeneration;
		operation = BeginOperationLocked(EditorProfilerOperationKind::LoadingIndex);
		pendingIndexRequest = PendingIndexRequest{artifactId, offset, boundedPageSize};
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, store, path, cancel, epoch, operation,
		offset, boundedPageSize, artifactId]() {
		if (cancel && cancel->load(std::memory_order_acquire)) return;
		ProfilerCaptureIndexPage page;
		std::string error;
		if (!store.ReadJournalIndexPage(path, offset, boundedPageSize, page, error))
		{
			if (auto self = weakSelf.lock()) self->EnqueueWorkerCompletion({
				WorkerCompletionKind::OperationFailed, EditorProfilerOperationKind::LoadingIndex,
				epoch, operation, {}, {}, std::move(error)});
			return;
		}
		if (cancel && cancel->load(std::memory_order_acquire)) return;
		if (auto self = weakSelf.lock())
		{
			WorkerCompletion completion;
			completion.kind = WorkerCompletionKind::IndexPageLoaded;
			completion.operationKind = EditorProfilerOperationKind::LoadingIndex;
			completion.projectEpoch = epoch;
			completion.operationId = operation;
			completion.indexPage = EditorProfilerIndexPage{
				artifactId, page.offset, page.pageSize, page.totalFrameCount, std::move(page.frames)};
			completion.selectedArtifactId = artifactId;
			self->EnqueueWorkerCompletion(std::move(completion));
		}
	});
	ThreadPool::instance().enqueue(task);
}

void EditorProfilerService::RequestSelectedFrame(std::uint64_t frameIndex)
{
	ProfilerCaptureStore store;
	std::filesystem::path path;
	std::shared_ptr<std::atomic_bool> cancel;
	std::uint64_t epoch = 0u;
	std::uint64_t operation = 0u;
	std::string artifactId;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!selectedCapture.has_value()) return;
		store = captureStore;
		path = selectedCapture->artifact.absolutePath;
		artifactId = selectedCapture->artifactId;
		for (const DetailCacheEntry& cached : decodedDetailCache)
		{
			if (cached.artifactId == artifactId && cached.frameIndex == frameIndex)
			{
				if (pendingDetailRequest.has_value()
					&& (pendingDetailRequest->artifactId != artifactId
						|| pendingDetailRequest->frameIndex != frameIndex))
				{
					// A cached detail selection is still a newer choice than a
					// different queued read. Retire that read so a late worker
					// completion cannot replace the frame the user just selected.
				auto& retiredOperation = operations[static_cast<std::size_t>(
					EditorProfilerOperationKind::LoadingDetail) - 1u];
				retiredOperation.active = false;
				retiredOperation.state = EditorProfilerOperationState::Cancelled;
					pendingDetailRequest.reset();
				}
				selectedFrame = cached.frame;
				selectedFrameArtifactId = artifactId;
				statusText = "Loaded selected frame";
				lastError.clear();
				PublishPresentationLocked();
				return;
			}
		}
		if (pendingDetailRequest.has_value()
			&& pendingDetailRequest->artifactId == artifactId
			&& pendingDetailRequest->frameIndex == frameIndex)
		{
			return;
		}
		cancel = cancellationToken;
		epoch = projectGeneration;
		operation = BeginOperationLocked(EditorProfilerOperationKind::LoadingDetail);
		pendingDetailRequest = PendingDetailRequest{artifactId, frameIndex};
	}
	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, store, path, cancel, epoch, operation, frameIndex, artifactId]() {
		if (cancel && cancel->load(std::memory_order_acquire)) return;
		TerraEngine::Profiling::ProfilerFrameRecord frame;
		std::string error;
		if (!store.ReadSelectedJournalFrame(path, frameIndex, frame, error))
		{
			if (auto self = weakSelf.lock()) self->EnqueueWorkerCompletion({WorkerCompletionKind::OperationFailed, EditorProfilerOperationKind::LoadingDetail, epoch, operation, {}, {}, std::move(error)});
			return;
		}
		if (auto self = weakSelf.lock())
		{
			WorkerCompletion completion;
			completion.kind = WorkerCompletionKind::FrameLoaded;
			completion.operationKind = EditorProfilerOperationKind::LoadingDetail;
			completion.projectEpoch = epoch;
			completion.operationId = operation;
			completion.selectedFrame = std::move(frame);
			completion.selectedArtifactId = artifactId;
			self->EnqueueWorkerCompletion(std::move(completion));
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::GetState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerSessionState EditorProfilerService::GetState() const
{
	return Profiler::Instance().GetState();
}

std::uint64_t EditorProfilerService::GetPresentationRevision() const noexcept
{
	return presentationRevision.load(std::memory_order_acquire);
}

std::shared_ptr<const EditorProfilerPresentationSnapshot> EditorProfilerService::GetPresentationSnapshot() const noexcept
{
	return std::atomic_load_explicit(&presentationSnapshot, std::memory_order_acquire);
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

std::uint64_t EditorProfilerService::BeginOperationLocked(EditorProfilerOperationKind kind)
{
	const std::size_t index = static_cast<std::size_t>(kind) - 1u;
	EditorProfilerOperationStatus& operation = operations.at(index);
	operation.kind = kind;
	operation.operationId = nextOperationId++;
	operation.active = true;
	operation.state = EditorProfilerOperationState::Queued;
	return operation.operationId;
}

void EditorProfilerService::EnqueueWorkerCompletion(WorkerCompletion completion)
{
	// A worker may only transfer its immutable result into this queue. It never
	// accesses the mutable service session, ImGui, or global Profiler instance.
	std::lock_guard<std::mutex> lock(completionMutex);
	pendingCompletions.push_back(std::move(completion));
}

void EditorProfilerService::PublishPresentationLocked()
{
	auto snapshot = std::make_shared<EditorProfilerPresentationSnapshot>();
	snapshot->projectEpoch = projectGeneration;
	snapshot->presentationRevision = presentationRevision.fetch_add(1u, std::memory_order_acq_rel) + 1u;
	snapshot->sessionState = Profiler::Instance().GetState();
	snapshot->hasActiveProject = sessionAccepting && captureStore.HasProjectRoot();
	snapshot->statusText = statusText;
	snapshot->lastError = lastError;
	snapshot->health = health;
	snapshot->activeSessionArtifactId = activeSessionArtifactId;
	snapshot->discardedStaleCompletionCount = discardedStaleCompletionCount;
	snapshot->captures = captures;
	snapshot->profilerDirectory = captureStore.GetProfilerDirectory();
	const std::size_t persistenceIndex = static_cast<std::size_t>(
		EditorProfilerOperationKind::Persisting) - 1u;
	const bool persistenceActive = persistenceIndex < operations.size()
		&& operations[persistenceIndex].active;
	const bool liveCapture = snapshot->sessionState == ProfilerSessionState::Recording
		|| snapshot->sessionState == ProfilerSessionState::Paused
		|| (snapshot->sessionState == ProfilerSessionState::Finalizing && persistenceActive);
	if (!liveCapture)
	{
		// Index/detail snapshots are intentionally idle-only. Health refreshes
		// during a live capture stay scalar and cannot make ProfilerWindow rebuild
		// an old journal's duration statistics. The engine retains Finalizing as
		// its terminal journal state, so completed persistence must expose a later
		// loaded capture even though that scalar state does not change again.
		snapshot->selectedCapture = selectedCapture;
		snapshot->selectedSummary = selectedSummary;
		snapshot->indexPage = selectedIndexPage;
		snapshot->selectedFrame = selectedFrame;
		snapshot->selectedFrameArtifactId = selectedFrameArtifactId;
	}
	snapshot->operations = operations;
	snapshot->terrainDiagnostics = terrainDiagnostics;
	std::shared_ptr<const EditorProfilerPresentationSnapshot> immutableSnapshot = std::move(snapshot);
	std::atomic_store_explicit(&presentationSnapshot, std::move(immutableSnapshot), std::memory_order_release);
}

void EditorProfilerService::PublishTerrainDiagnostics(TerraEngine::Terrain::TerrainDiagnosticSnapshot snapshot)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Publication generation changes for authored edits, while collision
	// generation changes when a derived candidate becomes current. Suppressing an
	// identical pair keeps idle profiler presentation revisions stable.
	if(terrainDiagnostics
		&& terrainDiagnostics->revision == snapshot.revision
		&& terrainDiagnostics->collisionGeneration == snapshot.collisionGeneration
		&& terrainDiagnostics->pendingTransactions == snapshot.pendingTransactions)
	{
		return;
	}
	terrainDiagnostics = std::move(snapshot);
	PublishPresentationLocked();
}

void EditorProfilerService::AcceptWorkerCompletion(WorkerCompletion&& completion)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!sessionAccepting || completion.projectEpoch != projectGeneration)
	{
		// Closing/reopening intentionally abandons old work. Do not let a prior
		// project restore either global profiler data or window presentation.
		++discardedStaleCompletionCount;
		return;
	}

	const std::size_t index = static_cast<std::size_t>(completion.operationKind) - 1u;
	if (completion.operationKind == EditorProfilerOperationKind::None || index >= operations.size()
		|| !operations[index].active || operations[index].operationId != completion.operationId)
	{
		// A newer request of the same kind superseded this result. A stale detail
		// must not replace the selection requested after it was queued.
		++discardedStaleCompletionCount;
		return;
	}
	operations[index].active = false;
	operations[index].state = completion.kind == WorkerCompletionKind::OperationFailed
		? EditorProfilerOperationState::Failed
		: EditorProfilerOperationState::Completed;
	if (completion.operationKind == EditorProfilerOperationKind::LoadingIndex)
	{
		pendingIndexRequest.reset();
	}
	if (completion.operationKind == EditorProfilerOperationKind::LoadingDetail)
	{
		pendingDetailRequest.reset();
	}

	switch (completion.kind)
	{
	case WorkerCompletionKind::CaptureList:
		captures = std::move(completion.captures);
		lastError = std::move(completion.error);
		statusText = lastError.empty() ? "Capture list refreshed" : lastError;
		break;
	case WorkerCompletionKind::CapturePersisted:
		lastSavedCaptureArtifact = std::move(completion.savedCaptureArtifact);
		activeJournalPath.reset();
		activeSessionArtifactId.clear();
		lastError.clear();
		statusText = "Profiler journal finalized";
		if (lastSavedCaptureArtifact.has_value())
		{
			const std::string& fileName = lastSavedCaptureArtifact->artifact.fileName;
			captures.erase(std::remove_if(captures.begin(), captures.end(), [&fileName](const auto& artifact) {
				return artifact.fileName == fileName;
			}), captures.end());
			captures.push_back(lastSavedCaptureArtifact->artifact);
		}
		break;
	case WorkerCompletionKind::CaptureLoaded:
		selectedCapture = std::move(completion.selectedCapture);
		selectedSummary = std::move(completion.selectedSummary);
		selectedIndexPage = std::move(completion.indexPage);
		selectedFrame.reset();
		selectedFrameArtifactId.clear();
		lastError.clear();
		statusText = "Loaded capture";
		break;
	case WorkerCompletionKind::IndexPageLoaded:
		if (!selectedCapture.has_value()
			|| completion.selectedArtifactId != selectedCapture->artifactId)
		{
			++discardedStaleCompletionCount;
			operations[index].state = EditorProfilerOperationState::Cancelled;
			PublishPresentationLocked();
			return;
		}
		selectedIndexPage = std::move(completion.indexPage);
		lastError.clear();
		statusText = "Loaded profiler index page";
		break;
	case WorkerCompletionKind::FrameLoaded:
		if (!selectedCapture.has_value()
			|| completion.selectedArtifactId != selectedCapture->artifactId)
		{
			// An open-capture request completed after its selection changed. The
			// operation is retired, but its frame must remain invisible.
			++discardedStaleCompletionCount;
			operations[index].state = EditorProfilerOperationState::Cancelled;
			PublishPresentationLocked();
			return;
		}
		selectedFrame = std::move(completion.selectedFrame);
		selectedFrameArtifactId = completion.selectedArtifactId;
		if (selectedFrame.has_value())
		{
			decodedDetailCache.push_front({completion.selectedArtifactId,
				selectedFrame->frameIndex, *selectedFrame});
			while (decodedDetailCache.size() > 4u)
			{
				decodedDetailCache.pop_back();
			}
		}
		lastError.clear();
		statusText = "Loaded selected frame";
		break;
	case WorkerCompletionKind::OperationFailed:
		if (completion.operationKind == EditorProfilerOperationKind::Persisting)
		{
			activeJournalPath.reset();
			activeSessionArtifactId.clear();
		}
		lastError = std::move(completion.error);
		statusText = completion.operationKind == EditorProfilerOperationKind::Persisting
			? "Failed to save capture"
			: (completion.operationKind == EditorProfilerOperationKind::Opening
				? "Failed to open capture" : "Failed to refresh capture list");
		break;
	}
	PublishPresentationLocked();
}

// EditorProfilerService::HasActiveProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::HasActiveProject() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return sessionAccepting && captureStore.HasProjectRoot();
}

// EditorProfilerService::IsBusy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::IsBusy() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return std::any_of(operations.begin(), operations.end(), [](const EditorProfilerOperationStatus& operation) {
		return operation.active;
	});
}

bool EditorProfilerService::IsPersisting() const
{
	std::lock_guard<std::mutex> lock(mutex);
	const std::size_t persistenceIndex = static_cast<std::size_t>(
		EditorProfilerOperationKind::Persisting) - 1u;
	return persistenceIndex < operations.size() && operations[persistenceIndex].active;
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
