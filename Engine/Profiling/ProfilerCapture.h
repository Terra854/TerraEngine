// ProfilerCapture.h — Contains declarations for the Profiler Capture file-specific types, helper functions, and module integration code used by runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Profiling
{
// ProfilerSessionState names the supported states explicitly so serialization and switches stay readable.
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

// ProfilerModuleId names the supported states explicitly so serialization and switches stay readable.
enum class ProfilerModuleId : std::uint8_t
{
	CpuEngine,
	TaskExecution
};

// ProfilerCaptureMode names the supported states explicitly so serialization and switches stay readable.
enum class ProfilerCaptureMode : std::uint8_t
{
	LiveRolling,
	FinalizingToArtifact,
	LoadedReadOnly
};

// struct ProfilerCaptureSettings groups related state and operations so callers have one documented contract.
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

// struct ProfilerMetric groups related state and operations so callers have one documented contract.
struct ProfilerMetric
{
	std::string name;
	double value = 0.0;
	std::string unit;
};

// struct ProfilerModuleSummary groups related state and operations so callers have one documented contract.
struct ProfilerModuleSummary
{
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	bool available = false;
	double totalMs = 0.0;
	std::vector<ProfilerMetric> metrics;
};

// struct ProfilerSample groups related state and operations so callers have one documented contract.
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

// struct ProfilerFrameRecord groups related state and operations so callers have one documented contract.
struct ProfilerFrameRecord
{
	std::uint64_t frameIndex = 0u;
	double frameStartMs = 0.0;
	double durationMs = 0.0;
	bool complete = true;
	std::vector<ProfilerModuleSummary> moduleSummaries;
	std::vector<ProfilerSample> samples;
};

// struct ProfilerFrameListItem keeps the editor frame list cheap to redraw.
// It intentionally mirrors only the columns shown in the list so opening the
// profiler window does not copy every sample string for every retained frame.
struct ProfilerFrameListItem
{
	std::uint64_t frameIndex = 0u;
	double durationMs = 0.0;
	bool complete = true;
};

// struct ProfilerCaptureMetadata groups related state and operations so callers have one documented contract.
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

// struct ProfilerCaptureArtifact groups related state and operations so callers have one documented contract.
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

// struct ProfilerCaptureDocument groups related state and operations so callers have one documented contract.
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
bool TryReadCaptureArtifactMetadata(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
}
