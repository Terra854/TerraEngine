#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Profiling
{
enum class ProfilerSessionState : std::uint8_t
{
	Idle,
	Recording,
	Paused,
	Finalizing,
	Saving,
	OpeningSavedCapture,
	Loaded,
	Error
};

enum class ProfilerModuleId : std::uint8_t
{
	CpuEngine,
	TaskExecution
};

enum class ProfilerCaptureMode : std::uint8_t
{
	LiveRolling,
	FinalizingToArtifact,
	LoadedReadOnly
};

struct ProfilerCaptureSettings
{
	bool cpuEngineEnabled = true;
	bool taskExecutionEnabled = true;
	bool recordMarkers = true;
	bool recordTaskContext = true;
	std::uint32_t maxFrames = 10000u;
	double maxDurationSeconds = 0.0;
	ProfilerCaptureMode captureMode = ProfilerCaptureMode::LiveRolling;
};

struct ProfilerMetric
{
	std::string name;
	double value = 0.0;
	std::string unit;
};

struct ProfilerModuleSummary
{
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	bool available = false;
	double totalMs = 0.0;
	std::vector<ProfilerMetric> metrics;
};

struct ProfilerSample
{
	std::uint64_t sampleId = 0u;
	std::uint64_t parentSampleId = 0u;
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	std::string name;
	std::string threadLabel;
	std::string taskLabel;
	double startMs = 0.0;
	double durationMs = 0.0;
	std::uint32_t depth = 0u;
	std::uint32_t occurrenceCount = 1u;
	bool complete = true;
};

struct ProfilerFrameRecord
{
	std::uint64_t frameIndex = 0u;
	double frameStartMs = 0.0;
	double durationMs = 0.0;
	bool complete = true;
	std::vector<ProfilerModuleSummary> moduleSummaries;
	std::vector<ProfilerSample> samples;
};

struct ProfilerCaptureMetadata
{
	std::string terraEngineVersion = "unknown";
	std::string projectName;
	std::string projectPathHint;
	std::string scenePath;
	std::string startedAt;
	std::string savedAt;
	ProfilerCaptureSettings settings;
};

struct ProfilerCaptureArtifact
{
	std::string fileName;
	std::filesystem::path absolutePath;
	ProfilerCaptureMetadata metadata;
	std::uint32_t payloadVersion = 1u;
	std::uint64_t frameCount = 0u;
	std::vector<ProfilerModuleId> moduleIds;
	bool supported = true;
	std::string status;
};

struct ProfilerCaptureDocument
{
	ProfilerCaptureMetadata metadata;
	std::vector<ProfilerFrameRecord> frames;
	std::optional<std::filesystem::path> sourcePath;
	std::string lastError;
};

const char* ToString(ProfilerSessionState state);
const char* ToString(ProfilerModuleId moduleId);
std::string MakeTimestampForFileName();
std::string MakeReadableTimestamp();
bool AnyModuleEnabled(const ProfilerCaptureSettings& settings);
std::vector<ProfilerModuleSummary> MakeDefaultModuleSummaries(const ProfilerCaptureSettings& settings);
void TrimFrameBuffer(std::vector<ProfilerFrameRecord>& frames, const ProfilerCaptureSettings& settings);
bool SaveCaptureDocument(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError);
bool LoadCaptureDocument(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError);
bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
}
