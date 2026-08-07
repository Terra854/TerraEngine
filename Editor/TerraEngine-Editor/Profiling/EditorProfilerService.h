// EditorProfilerService.h — Editor profiling service interface
//
// Defines the high-level service for managing engine profiling sessions within the editor. it handles the coordination of capture recordings, session persistence via the ProfilerCaptureStore, and asynchronous loading of profile artifacts.

#pragma once

#include "ProfilerCaptureStore.h"

#include <array>
#include <atomic>
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

// EditorProfilerMetricSummary keeps a percentile series self-describing for
// both the window and automation.  An unavailable source stays unavailable
// instead of being represented as a misleading zero-millisecond sample.
struct EditorProfilerMetricSummary
{
	std::string id;
	std::string label;
	std::string unit;
	std::uint64_t sampleCount = 0u;
	std::uint64_t unavailableCount = 0u;
	bool available = false;
	std::string unavailableReason;
	double median = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
};

// EditorProfilerWorkerSummary preserves a bounded aggregate for each worker.
// The raw timeline remains inside the capture artifact, so summaries cannot
// accidentally make durable automation results unbounded.
struct EditorProfilerWorkerSummary
{
	std::uint32_t workerId = 0u;
	std::uint64_t sampleCount = 0u;
	std::uint64_t maximumQueueDepth = 0u;
	std::uint64_t totalDispatchCount = 0u;
	std::uint64_t totalUsefulWorkItems = 0u;
	EditorProfilerMetricSummary runDuration;
	EditorProfilerMetricSummary waitDuration;
	EditorProfilerMetricSummary idleDuration;
};

// EditorProfilerWorkVolumeSummary reports the largest observed body counts
// for the capture.  It keeps processed-versus-published work visible without
// requiring callers to retrieve a full per-tick trace.
struct EditorProfilerWorkVolumeSummary
{
	std::uint64_t measuredTickCount = 0u;
	std::uint64_t totalBodies = 0u;
	std::uint64_t activeBodies = 0u;
	std::uint64_t examinedBodies = 0u;
	std::uint64_t extractedBodies = 0u;
	std::uint64_t convertedBodies = 0u;
	std::uint64_t changedBodies = 0u;
	std::uint64_t publishedBodies = 0u;
};

// EditorProfilerRenderOptimizationSummary is a bounded whole-capture reduction
// of the typed per-frame renderer evidence. Maxima describe scene scale while
// totals explain whether grouping and reuse remained active throughout the run.
struct EditorProfilerRenderOptimizationSummary
{
	std::uint32_t viewType = 0U;
	std::uint64_t measuredFrameCount = 0U;
	std::uint64_t maximumVisibleInstances = 0U;
	std::uint64_t maximumGroupedInstances = 0U;
	std::uint64_t maximumFallbackInstances = 0U;
	std::uint64_t groupedSubmissions = 0U;
	std::uint64_t fallbackSubmissions = 0U;
	std::uint64_t avoidedSubmissions = 0U;
	std::uint64_t topologyReuseHits = 0U;
	std::uint64_t topologyInvalidations = 0U;
	std::uint64_t viewEvidenceReuseHits = 0U;
	std::uint64_t viewEvidenceInvalidations = 0U;
	std::uint64_t rejectedCandidates = 0U;
	std::uint64_t optimizationFallbacks = 0U;
	std::uint64_t topologyBuildNanoseconds = 0U;
	std::uint64_t dynamicMaterializationNanoseconds = 0U;
	std::uint64_t instanceStreamUploadNanoseconds = 0U;
	std::uint64_t groupedCommandRecordingNanoseconds = 0U;
};

// Capture-wide animation evidence stays bounded regardless of recording length.
// Peak population values and summed work/reuse counters are intentionally kept
// separate so a developer can distinguish scale from repeated per-frame cost.
struct EditorProfilerAnimationOptimizationSummary
{
	std::uint64_t measuredFrameCount = 0U;
	std::uint64_t maximumActiveAnimators = 0U;
	std::uint64_t maximumCompactProcessed = 0U;
	std::uint64_t maximumCompleteProcessed = 0U;
	std::uint64_t compactToCompleteTransitions = 0U;
	std::uint64_t completeToCompactTransitions = 0U;
	std::uint64_t retainedTopologyHits = 0U;
	std::uint64_t retainedTopologyMisses = 0U;
	std::uint64_t compactPlanGroups = 0U;
	std::uint64_t compactDispatches = 0U;
	std::uint64_t rejectedCandidates = 0U;
	std::uint64_t fallbackReasons = 0U;
	std::array<std::uint64_t, 8U> totalTimingNanoseconds{};
};

// EditorProfilerCaptureSummary is intentionally bounded: it exposes aggregate
// frame and timing-stream evidence without returning every recorded sample.
struct EditorProfilerCaptureSummary
{
	std::string artifactId;
	std::uint64_t frameCount = 0u;
	std::uint64_t completeFrameCount = 0u;
	std::uint64_t incompleteFrameCount = 0u;
	double totalDurationMs = 0.0;
	double minimumFrameMs = 0.0;
	double maximumFrameMs = 0.0;
	double averageFrameMs = 0.0;
	// Performance gates compare complete frames only. Keeping these bounded
	// aggregates in the public summary avoids exposing the raw frame timeline.
	double frameMedianMs = 0.0;
	double frameP95Ms = 0.0;
	double frameP99Ms = 0.0;
	double frameWorstMs = 0.0;
	std::uint32_t payloadVersion = 1u;
	std::uint64_t committedFrameCount = 0u;
	std::uint64_t artifactSizeBytes = 0u;
	bool captureComplete = true;
	bool recoveredIncompleteTail = false;
	std::vector<TerraEngine::Profiling::ProfilerTimingStreamAggregate> timingStreams;
	std::vector<EditorProfilerMetricSummary> stageTimings;
	std::vector<EditorProfilerMetricSummary> protectedBoundaryTimings;
	std::vector<EditorProfilerMetricSummary> presentationWaitTimings;
	std::vector<EditorProfilerMetricSummary> responsivenessTimings;
	std::vector<EditorProfilerWorkerSummary> workerHistories;
	EditorProfilerWorkVolumeSummary workVolumes;
	EditorProfilerAnimationOptimizationSummary animationOptimization;
	std::array<EditorProfilerRenderOptimizationSummary, 2U> renderOptimization{};
};

// class EditorProfilerService groups related state and operations so callers have one documented contract.
class EditorProfilerService : public std::enable_shared_from_this<EditorProfilerService>
{
public:
	void Initialize(EditorContext& context);
	void Shutdown();
	void OnProjectOpened();
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

	TerraEngine::Profiling::ProfilerSessionState GetState() const;
	// This revision changes only when a user-visible profiler document, capture
	// list, or status changes.  It deliberately does not follow the engine's
	// per-frame profiling revision, so idle editor windows can avoid copying the
	// same completed document on every host frame.
	[[nodiscard]] std::uint64_t GetPresentationRevision() const noexcept;
	std::string GetStatusText() const;
	std::string GetLastError() const;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> GetCaptureList() const;
	std::vector<EditorProfilerCaptureArtifact> GetAutomationCaptureList() const;
	// The stop job consumes this exact result instead of inferring the new capture
	// from a list delta that may race asynchronous project-open discovery.
	std::optional<EditorProfilerCaptureArtifact> GetLastSavedCaptureArtifact() const;
	// ReadCaptureDocumentById is a blocking worker API used by durable jobs. UI
	// callers should use RequestLoadCaptureById, which retains asynchronous load.
	bool ReadCaptureDocumentById(
		std::string_view artifactId,
		TerraEngine::Profiling::ProfilerCaptureDocument& outDocument,
		std::string& outError) const;
	bool RequestLoadCaptureById(std::string_view artifactId, std::string& outError);
	bool SummarizeCaptureById(std::string_view artifactId, EditorProfilerCaptureSummary& outSummary, std::string& outError) const;
	[[nodiscard]] static EditorProfilerCaptureSummary SummarizeDocument(
		std::string artifactId,
		const TerraEngine::Profiling::ProfilerCaptureDocument& document);
	bool HasActiveProject() const;
	bool IsBusy() const;
	std::filesystem::path GetProfilerDirectory() const;

private:
	TerraEngine::Profiling::ProfilerCaptureMetadata MakeMetadata() const;
	void SetBusyStatus(bool busy, std::string status);
	// Publication is lock-free because callers already protect the associated
	// presentation data with mutex.  Release ordering makes completed updates
	// visible before the window observes the new revision.
	void MarkPresentationChanged() noexcept;
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
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	std::optional<EditorProfilerCaptureArtifact> lastSavedCaptureArtifact;
	bool busy = false;
	std::string statusText = "Idle";
	std::string lastError;
	std::uint64_t projectGeneration = 0u;
	std::atomic_uint64_t presentationRevision = 1u;
	std::atomic_uint64_t editorFrameIndex = 0u;
	std::atomic<double> editorFrameTimeMs = 0.0;
	std::atomic_uint64_t profilerWindowDrawFrameIndex = 0u;
	std::atomic<double> profilerWindowDrawMs = 0.0;
	std::atomic_uint64_t imguiRenderFrameIndex = 0u;
	std::atomic<double> imguiRenderMs = 0.0;
	std::atomic_uint64_t presentUiFrameIndex = 0u;
	std::atomic<double> presentUiMs = 0.0;
};
