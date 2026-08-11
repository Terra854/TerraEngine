// EditorProfilerService.h — Editor profiling service interface
//
// Defines the high-level service for managing engine profiling sessions within the editor. it handles the coordination of capture recordings, session persistence via the ProfilerCaptureStore, and asynchronous loading of profile artifacts.

#pragma once

#include "ProfilerCaptureStore.h"
#include "Engine/Profiling/Profiler.h"

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct EditorContext;

// EditorProfilerFrameTiming is the previous completed host-frame delta used to
// compare profiler-window presentation cost while recording is intentionally
// idle. It is runtime-only evidence and is never persisted into a capture.
struct EditorProfilerFrameTiming
{
	std::uint64_t frameIndex = 0u;
	double frameTimeMs = 0.0;
	bool valid = false;
	// This is only ProfilerWindow::OnGui CPU work. It intentionally excludes
	// ImGui draw-data generation, Vulkan UI presentation, and scheduler work so
	// automation can attribute an open-idle delta to the correct boundary.
	std::uint64_t profilerWindowDrawFrameIndex = 0u;
	double profilerWindowDrawMs = 0.0;
	bool profilerWindowDrawValid = false;
	// These post-window stages isolate CPU draw-data construction from the UI
	// submission/presentation boundary. They are diagnostic scalars only and do
	// not enable capture recording or alter renderer scheduling.
	std::uint64_t imguiRenderFrameIndex = 0u;
	double imguiRenderMs = 0.0;
	bool imguiRenderValid = false;
	std::uint64_t presentUiFrameIndex = 0u;
	double presentUiMs = 0.0;
	bool presentUiValid = false;
};

// EditorProfilerCaptureArtifact pairs project-contained capture metadata with
// an opaque identifier. Automation clients never receive or submit an
// arbitrary filesystem path.
struct EditorProfilerCaptureArtifact
{
	std::string artifactId;
	TerraEngine::Profiling::ProfilerCaptureArtifact artifact;
};

// EditorProfilerOperationKind identifies independent asynchronous service work.
// It deliberately replaces the old single busy flag: listing a catalog must not
// make a finalizing capture appear complete, and vice versa.
enum class EditorProfilerOperationKind : std::uint8_t
{
	None,
	Listing,
	Persisting,
	Opening,
	LoadingIndex,
	LoadingDetail
};

// EditorProfilerOperationState preserves a compact terminal outcome for the
// most recent operation of each kind.  Automation can poll that outcome by
// identifier without retaining frame history or worker-owned payloads.
enum class EditorProfilerOperationState : std::uint8_t
{
	Queued,
	Running,
	Completed,
	Failed,
	Cancelled
};

[[nodiscard]] constexpr std::string_view ToString(EditorProfilerOperationKind kind) noexcept
{
	switch (kind)
	{
	case EditorProfilerOperationKind::Listing: return "listing";
	case EditorProfilerOperationKind::Persisting: return "persisting";
	case EditorProfilerOperationKind::Opening: return "opening";
	case EditorProfilerOperationKind::LoadingIndex: return "loading_index";
	case EditorProfilerOperationKind::LoadingDetail: return "loading_detail";
	case EditorProfilerOperationKind::None: break;
	}
	return "none";
}

[[nodiscard]] constexpr std::string_view ToString(EditorProfilerOperationState state) noexcept
{
	switch (state)
	{
	case EditorProfilerOperationState::Queued: return "queued";
	case EditorProfilerOperationState::Running: return "running";
	case EditorProfilerOperationState::Completed: return "completed";
	case EditorProfilerOperationState::Failed: return "failed";
	case EditorProfilerOperationState::Cancelled: return "cancelled";
	}
	return "cancelled";
}

inline constexpr std::size_t kEditorProfilerOperationCount = 5u;

// EditorProfilerIndexPage is the only history-shaped value allowed in an
// immutable presentation. Its capacity is bounded by ProfilerCaptureStore.
struct EditorProfilerIndexPage
{
	std::string artifactId;
	std::size_t offset = 0u;
	std::size_t pageSize = 0u;
	std::size_t totalFrameCount = 0u;
	std::vector<TerraEngine::Profiling::ProfilerFrameListItem> frames;
};

// EditorProfilerOperationStatus is a bounded UI/automation view of one kind of
// work. The operation id changes for every request so late worker results can be
// rejected even when they belong to the current project epoch.
struct EditorProfilerOperationStatus
{
	EditorProfilerOperationKind kind = EditorProfilerOperationKind::None;
	std::uint64_t operationId = 0u;
	bool active = false;
	EditorProfilerOperationState state = EditorProfilerOperationState::Completed;
};

// EditorProfilerPresentationSnapshot is immutable once published. The window
// receives this small value instead of reading the mutable service catalog while
// background workers are discovering or opening captures. Detailed V1 index and
// frame payload fields will be added when the engine journal contract lands.
struct EditorProfilerPresentationSnapshot
{
	std::uint64_t projectEpoch = 0u;
	std::uint64_t presentationRevision = 0u;
	TerraEngine::Profiling::ProfilerSessionState sessionState = TerraEngine::Profiling::ProfilerSessionState::Idle;
	bool hasActiveProject = false;
	std::string statusText;
	std::string lastError;
	TerraEngine::Profiling::ProfilerCaptureHealth health;
	// The reserved V1 filename is an opaque session identity. It lets bounded
	// health polling prove which live journal produced its counters.
	std::string activeSessionArtifactId;
	std::uint64_t discardedStaleCompletionCount = 0u;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	std::filesystem::path profilerDirectory;
	std::optional<EditorProfilerCaptureArtifact> selectedCapture;
	std::optional<TerraEngine::Profiling::ProfilerJournalV1Summary> selectedSummary;
	std::optional<EditorProfilerIndexPage> indexPage;
	std::optional<TerraEngine::Profiling::ProfilerFrameRecord> selectedFrame;
	std::string selectedFrameArtifactId;
	std::array<EditorProfilerOperationStatus, kEditorProfilerOperationCount> operations{};
};

// class EditorProfilerService groups related state and operations so callers have one documented contract.
class EditorProfilerService : public std::enable_shared_from_this<EditorProfilerService>
{
public:
	void Initialize(EditorContext& context);
	void Shutdown();
	void OnProjectOpened();
	// Call before scene/resource teardown. It invalidates every queued worker
	// completion before any outgoing-project state can be observed again.
	void BeginProjectClose();
	void OnProjectClosed();
	void Update(float deltaTime);
	void PublishEditorFrameTiming(std::uint64_t frameIndex, double frameTimeMs) noexcept;
	void PublishProfilerWindowDrawTiming(std::uint64_t frameIndex, double drawDurationMs) noexcept;
	void ClearProfilerWindowDrawTiming() noexcept;
	void PublishImGuiRenderTiming(std::uint64_t frameIndex, double durationMs) noexcept;
	void PublishPresentUiTiming(std::uint64_t frameIndex, double durationMs) noexcept;
	[[nodiscard]] EditorProfilerFrameTiming GetEditorFrameTiming() const noexcept;

	void StartRecording();
	void StartRecording(const TerraEngine::Profiling::ProfilerCaptureSettings& settings);
	void PauseRecording();
	void ResumeRecording();
	// Returns true only when a non-empty document was handed to background
	// persistence. Callers can therefore fail an empty stopped-mode request
	// immediately instead of leaving a durable job waiting forever.
	bool FinishCapture();
	void Clear();
	void RefreshCaptureList();
	void LoadCapture(const std::filesystem::path& capturePath);
	// A page request is asynchronous and coalesced by artifact/offset/size.
	// Callers never obtain the journal's complete frame index.
	void RequestIndexPage(std::size_t offset, std::size_t pageSize = 128u);
	void RequestSelectedFrame(std::uint64_t frameIndex);

	TerraEngine::Profiling::ProfilerSessionState GetState() const;
	// This revision changes only when a user-visible profiler document, capture
	// list, or status changes.  It deliberately does not follow the engine's
	// per-frame profiling revision, so idle editor windows can avoid copying the
	// same completed document on every host frame.
	[[nodiscard]] std::uint64_t GetPresentationRevision() const noexcept;
	[[nodiscard]] std::shared_ptr<const EditorProfilerPresentationSnapshot> GetPresentationSnapshot() const noexcept;
	std::string GetStatusText() const;
	std::string GetLastError() const;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> GetCaptureList() const;
	std::vector<EditorProfilerCaptureArtifact> GetAutomationCaptureList() const;
	// The stop job consumes this exact result instead of inferring the new capture
	// from a list delta that may race asynchronous project-open discovery.
	std::optional<EditorProfilerCaptureArtifact> GetLastSavedCaptureArtifact() const;
	bool RequestLoadCaptureById(std::string_view artifactId, std::string& outError);
	bool HasActiveProject() const;
	bool IsBusy() const;
	// Automation finish jobs wait only for their own terminal-index validation.
	// Catalog refreshes or a user-selected detail read must not delay success.
	bool IsPersisting() const;
	std::filesystem::path GetProfilerDirectory() const;

private:
	enum class WorkerCompletionKind : std::uint8_t
	{
		CaptureList,
		CapturePersisted,
		CaptureLoaded,
		IndexPageLoaded,
		FrameLoaded,
		OperationFailed
	};

	// Workers own this value until they enqueue it. Publishing is deliberately
	// deferred to Update so workers never mutate Profiler, UI, or service state.
	struct WorkerCompletion
	{
		WorkerCompletionKind kind = WorkerCompletionKind::OperationFailed;
		EditorProfilerOperationKind operationKind = EditorProfilerOperationKind::None;
		std::uint64_t projectEpoch = 0u;
		std::uint64_t operationId = 0u;
		std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
		std::optional<EditorProfilerCaptureArtifact> savedCaptureArtifact;
		std::string error;
		std::optional<EditorProfilerCaptureArtifact> selectedCapture;
		std::optional<TerraEngine::Profiling::ProfilerJournalV1Summary> selectedSummary;
		std::filesystem::path capturePath;
		std::optional<EditorProfilerIndexPage> indexPage;
		std::optional<TerraEngine::Profiling::ProfilerFrameRecord> selectedFrame;
		// A detail completion belongs to exactly one opened artifact.  Opening a
		// different capture must never let an older read replace its selection.
		std::string selectedArtifactId;
	};

	TerraEngine::Profiling::ProfilerCaptureMetadata MakeMetadata() const;
	[[nodiscard]] std::uint64_t BeginOperationLocked(EditorProfilerOperationKind kind);
	void EnqueueWorkerCompletion(WorkerCompletion completion);
	void PublishPresentationLocked();
	void AcceptWorkerCompletion(WorkerCompletion&& completion);
	[[nodiscard]] static std::string MakeArtifactId(std::string_view fileName);
	[[nodiscard]] std::optional<std::filesystem::path> FindCapturePathLocked(std::string_view artifactId) const;

	EditorContext* ctx = nullptr;
	ProfilerCaptureStore captureStore;
	// Each opened project gets a token that background profiler tasks check
	// before doing filesystem work and before publishing results. Closing the
	// project flips the token so queued tasks can exit without keeping editor
	// shutdown alive.
	std::shared_ptr<std::atomic_bool> cancellationToken = std::make_shared<std::atomic_bool>(false);
	mutable std::mutex mutex;
	// This queue is the only service state a worker may write. EditorSystem drains
	// it at its protected boundary before ProfilerWindow renders.
	mutable std::mutex completionMutex;
	std::deque<WorkerCompletion> pendingCompletions;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	std::optional<EditorProfilerCaptureArtifact> lastSavedCaptureArtifact;
	std::array<EditorProfilerOperationStatus, kEditorProfilerOperationCount> operations{};
	std::uint64_t nextOperationId = 1u;
	bool sessionAccepting = false;
	std::uint64_t discardedStaleCompletionCount = 0u;
	std::optional<std::filesystem::path> activeJournalPath;
	std::string activeSessionArtifactId;
	std::optional<TerraEngine::Profiling::ProfilerJournalFinalizationTicket> finalizationTicket;
	// The service owns one scalar health copy and publishes it only at the
	// protected editor boundary. ProfilerWindow never polls mutable engine state.
	TerraEngine::Profiling::ProfilerCaptureHealth health;
	// Active recording publishes only scalar health at a bounded cadence. The
	// Profiler window stays responsive without rebuilding frame history or
	// taking a profiler mutex on every editor frame.
	float healthPublishAccumulatorSeconds = 0.0f;
	std::optional<EditorProfilerCaptureArtifact> selectedCapture;
	std::optional<TerraEngine::Profiling::ProfilerJournalV1Summary> selectedSummary;
	std::optional<EditorProfilerIndexPage> selectedIndexPage;
	std::optional<TerraEngine::Profiling::ProfilerFrameRecord> selectedFrame;
	std::string selectedFrameArtifactId;
	struct DetailCacheEntry
	{
		std::string artifactId;
		std::uint64_t frameIndex = 0u;
		TerraEngine::Profiling::ProfilerFrameRecord frame;
	};
	struct PendingIndexRequest
	{
		std::string artifactId;
		std::size_t offset = 0u;
		std::size_t pageSize = 0u;
	};
	struct PendingDetailRequest
	{
		std::string artifactId;
		std::uint64_t frameIndex = 0u;
	};
	std::deque<DetailCacheEntry> decodedDetailCache;
	std::optional<PendingIndexRequest> pendingIndexRequest;
	std::optional<PendingDetailRequest> pendingDetailRequest;
	std::string statusText = "Idle";
	std::string lastError;
	std::uint64_t projectGeneration = 0u;
	std::atomic_uint64_t presentationRevision = 1u;
	// Atomic shared-pointer operations let UI readers retain one immutable
	// presentation value without holding the service mutex across ImGui work.
	std::shared_ptr<const EditorProfilerPresentationSnapshot> presentationSnapshot;
	std::atomic_uint64_t editorFrameIndex = 0u;
	std::atomic<double> editorFrameTimeMs = 0.0;
	std::atomic_uint64_t profilerWindowDrawFrameIndex = 0u;
	std::atomic<double> profilerWindowDrawMs = 0.0;
	std::atomic_uint64_t imguiRenderFrameIndex = 0u;
	std::atomic<double> imguiRenderMs = 0.0;
	std::atomic_uint64_t presentUiFrameIndex = 0u;
	std::atomic<double> presentUiMs = 0.0;
};
