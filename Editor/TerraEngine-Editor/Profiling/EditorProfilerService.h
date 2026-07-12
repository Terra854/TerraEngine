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
	std::vector<TerraEngine::Profiling::ProfilerTimingStreamAggregate> timingStreams;
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
	bool busy = false;
	std::string statusText = "Idle";
	std::string lastError;
	std::uint64_t projectGeneration = 0u;
};
