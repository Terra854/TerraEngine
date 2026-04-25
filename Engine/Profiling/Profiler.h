#pragma once

#include "Profiling/ProfilerCapture.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct RenderStatsSnapshot;

namespace TerraEngine::Profiling
{
class Profiler
{
public:
	static Profiler& Instance();

	void StartRecording(const ProfilerCaptureMetadata& metadata, const ProfilerCaptureSettings& settings);
	void PauseRecording();
	void ResumeRecording();
	ProfilerCaptureDocument FinishRecording();
	void Clear();
	void LoadDocument(const ProfilerCaptureDocument& document);

	void BeginFrame();
	void EndFrame();
	void BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel = nullptr);
	void EndSample();
	void RecordRenderStats(const RenderStatsSnapshot& gameStats, const RenderStatsSnapshot& sceneStats);
	void RecordTaskQueued();
	void RecordTaskStarted(const char* taskLabel);
	void RecordTaskFinished();
	void SelectFrame(std::uint64_t frameIndex);

	ProfilerSessionState GetState() const;
	ProfilerCaptureSettings GetSettings() const;
	ProfilerCaptureDocument GetDocumentSnapshot() const;
	std::vector<ProfilerFrameRecord> GetFramesSnapshot() const;
	std::optional<ProfilerFrameRecord> GetSelectedFrameSnapshot() const;
	std::optional<std::filesystem::path> GetSourcePath() const;
	std::string GetLastError() const;
	std::string GetStatusText() const;
	bool IsCapturing() const;

private:
	Profiler() = default;

	double NowMs() const;
	void RebuildCurrentFrameModules();
	void AggregateSamplesForFrame(ProfilerFrameRecord& frame) const;

	mutable std::mutex mutex;
	ProfilerSessionState state = ProfilerSessionState::Idle;
	ProfilerCaptureDocument document;
	ProfilerCaptureSettings settings;
	std::optional<ProfilerFrameRecord> currentFrame;
	std::optional<std::uint64_t> selectedFrameIndex;
	std::chrono::steady_clock::time_point sessionStart{};
	std::chrono::steady_clock::time_point currentFrameStart{};
	std::uint64_t nextFrameIndex = 0u;
	std::uint64_t nextSampleId = 1u;
	std::uint64_t queuedTaskCount = 0u;
	std::uint64_t startedTaskCount = 0u;
	std::uint64_t finishedTaskCount = 0u;
	std::string statusText = "Idle";
	std::string lastError;
};
}
