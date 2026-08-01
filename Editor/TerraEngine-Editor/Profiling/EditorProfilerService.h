// EditorProfilerService.h — Editor profiling service interface
//
// Defines the high-level service for managing engine profiling sessions within the editor. it handles the coordination of capture recordings, session persistence via the ProfilerCaptureStore, and asynchronous loading of profile artifacts.

#pragma once

#include "ProfilerCaptureStore.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct EditorContext;

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

	void StartRecording();
	void StartRecording(const TerraEngine::Profiling::ProfilerCaptureSettings& settings);
	void PauseRecording();
	void ResumeRecording();
	void FinishCapture();
	void Clear();
	void RefreshCaptureList();
	void LoadCapture(const std::filesystem::path& capturePath);

	TerraEngine::Profiling::ProfilerSessionState GetState() const;
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
};
