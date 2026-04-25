#include "Profiling/ProfilerCapture.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

namespace TerraEngine::Profiling
{
namespace
{
constexpr std::uint32_t kProfilerCaptureVersion = 1u;

std::string ModuleIdToStableName(ProfilerModuleId moduleId)
{
	switch (moduleId)
	{
	case ProfilerModuleId::CpuEngine:
		return "cpu_engine";
	case ProfilerModuleId::TaskExecution:
		return "task_execution";
	}
	return "cpu_engine";
}

std::optional<ProfilerModuleId> TryModuleIdFromStableName(const std::string& value)
{
	if (value == "cpu_engine")
	{
		return ProfilerModuleId::CpuEngine;
	}
	if (value == "task_execution")
	{
		return ProfilerModuleId::TaskExecution;
	}
	return std::nullopt;
}

std::tm ToLocalTime(std::time_t timeValue)
{
	std::tm result{};
#ifdef _WIN32
	localtime_s(&result, &timeValue);
#else
	localtime_r(&timeValue, &result);
#endif
	return result;
}

nlohmann::json SettingsToJson(const ProfilerCaptureSettings& settings)
{
	return {
		{"cpuEngineEnabled", settings.cpuEngineEnabled},
		{"taskExecutionEnabled", settings.taskExecutionEnabled},
		{"recordMarkers", settings.recordMarkers},
		{"recordTaskContext", settings.recordTaskContext},
		{"maxFrames", settings.maxFrames},
		{"maxDurationSeconds", settings.maxDurationSeconds}
	};
}

ProfilerCaptureSettings SettingsFromJson(const nlohmann::json& json)
{
	ProfilerCaptureSettings settings{};
	settings.cpuEngineEnabled = json.value("cpuEngineEnabled", true);
	settings.taskExecutionEnabled = json.value("taskExecutionEnabled", true);
	settings.recordMarkers = json.value("recordMarkers", true);
	settings.recordTaskContext = json.value("recordTaskContext", true);
	settings.maxFrames = std::max(1u, json.value("maxFrames", 10000u));
	settings.maxDurationSeconds = std::max(0.0, json.value("maxDurationSeconds", 0.0));
	return settings;
}
}

const char* ToString(ProfilerSessionState state)
{
	switch (state)
	{
	case ProfilerSessionState::Idle:
		return "Idle";
	case ProfilerSessionState::Recording:
		return "Recording";
	case ProfilerSessionState::Paused:
		return "Paused";
	case ProfilerSessionState::Finalizing:
		return "Finalizing";
	case ProfilerSessionState::Saving:
		return "Saving";
	case ProfilerSessionState::OpeningSavedCapture:
		return "Opening Saved Capture";
	case ProfilerSessionState::Loaded:
		return "Loaded";
	case ProfilerSessionState::Error:
		return "Error";
	}
	return "Unknown";
}

const char* ToString(ProfilerModuleId moduleId)
{
	switch (moduleId)
	{
	case ProfilerModuleId::CpuEngine:
		return "CPU/Engine";
	case ProfilerModuleId::TaskExecution:
		return "Task Execution";
	}
	return "CPU/Engine";
}

std::string MakeTimestampForFileName()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
	const std::tm localTime = ToLocalTime(timeValue);
	std::ostringstream stream;
	stream << std::put_time(&localTime, "%Y%m%d-%H%M%S");
	return stream.str();
}

std::string MakeReadableTimestamp()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
	const std::tm localTime = ToLocalTime(timeValue);
	std::ostringstream stream;
	stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
	return stream.str();
}

bool AnyModuleEnabled(const ProfilerCaptureSettings& settings)
{
	return settings.cpuEngineEnabled || settings.taskExecutionEnabled;
}

std::vector<ProfilerModuleSummary> MakeDefaultModuleSummaries(const ProfilerCaptureSettings& settings)
{
	std::vector<ProfilerModuleSummary> modules;
	auto addModule = [&modules](ProfilerModuleId moduleId, bool enabled) {
		ProfilerModuleSummary summary{};
		summary.moduleId = moduleId;
		summary.available = enabled;
		modules.push_back(std::move(summary));
	};
	addModule(ProfilerModuleId::CpuEngine, settings.cpuEngineEnabled);
	addModule(ProfilerModuleId::TaskExecution, settings.taskExecutionEnabled);
	return modules;
}

void TrimFrameBuffer(std::vector<ProfilerFrameRecord>& frames, const ProfilerCaptureSettings& settings)
{
	const std::size_t maxFrames = std::max<std::size_t>(1u, settings.maxFrames);
	if (frames.size() > maxFrames)
	{
		frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(frames.size() - maxFrames));
	}

	if (settings.maxDurationSeconds > 0.0 && !frames.empty())
	{
		const double newestStart = frames.back().frameStartMs;
		const double cutoff = newestStart - settings.maxDurationSeconds * 1000.0;
		const auto firstKept = std::find_if(frames.begin(), frames.end(), [cutoff](const ProfilerFrameRecord& frame) {
			return frame.frameStartMs >= cutoff;
		});
		if (firstKept != frames.begin())
		{
			frames.erase(frames.begin(), firstKept);
		}
	}
}

bool SaveCaptureDocument(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError)
{
	try
	{
		nlohmann::json json;
		json["format"] = "TerraEngineProfilerCapture";
		json["payloadVersion"] = kProfilerCaptureVersion;
		json["metadata"] = {
			{"terraEngineVersion", document.metadata.terraEngineVersion},
			{"projectName", document.metadata.projectName},
			{"projectPathHint", document.metadata.projectPathHint},
			{"scenePath", document.metadata.scenePath},
			{"startedAt", document.metadata.startedAt},
			{"savedAt", document.metadata.savedAt},
			{"settings", SettingsToJson(document.metadata.settings)}
		};
		json["frames"] = nlohmann::json::array();

		for (const ProfilerFrameRecord& frame : document.frames)
		{
			nlohmann::json frameJson;
			frameJson["frameIndex"] = frame.frameIndex;
			frameJson["frameStartMs"] = frame.frameStartMs;
			frameJson["durationMs"] = frame.durationMs;
			frameJson["complete"] = frame.complete;
			frameJson["modules"] = nlohmann::json::array();
			for (const ProfilerModuleSummary& module : frame.moduleSummaries)
			{
				nlohmann::json moduleJson;
				moduleJson["moduleId"] = ModuleIdToStableName(module.moduleId);
				moduleJson["available"] = module.available;
				moduleJson["totalMs"] = module.totalMs;
				moduleJson["metrics"] = nlohmann::json::array();
				for (const ProfilerMetric& metric : module.metrics)
				{
					moduleJson["metrics"].push_back({{"name", metric.name}, {"value", metric.value}, {"unit", metric.unit}});
				}
				frameJson["modules"].push_back(std::move(moduleJson));
			}
			frameJson["samples"] = nlohmann::json::array();
			for (const ProfilerSample& sample : frame.samples)
			{
				frameJson["samples"].push_back({
					{"sampleId", sample.sampleId},
					{"parentSampleId", sample.parentSampleId},
					{"moduleId", ModuleIdToStableName(sample.moduleId)},
					{"name", sample.name},
					{"threadLabel", sample.threadLabel},
					{"taskLabel", sample.taskLabel},
					{"startMs", sample.startMs},
					{"durationMs", sample.durationMs},
					{"depth", sample.depth},
					{"occurrenceCount", sample.occurrenceCount},
					{"complete", sample.complete}
				});
			}
			json["frames"].push_back(std::move(frameJson));
		}

		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			outError = "Failed to open profiler capture for writing.";
			return false;
		}
		output << json.dump(2);
		return true;
	}
	catch (const std::exception& exception)
	{
		outError = exception.what();
		return false;
	}
}

bool LoadCaptureDocument(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError)
{
	try
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			outError = "Failed to open profiler capture for reading.";
			return false;
		}

		nlohmann::json json;
		input >> json;
		if (json.value("format", std::string{}) != "TerraEngineProfilerCapture")
		{
			outError = "File is not a TerraEngine profiler capture.";
			return false;
		}
		if (json.value("payloadVersion", 0u) != kProfilerCaptureVersion)
		{
			outError = "Profiler capture format version is unsupported.";
			return false;
		}

		ProfilerCaptureDocument document;
		document.sourcePath = path;
		const nlohmann::json metadata = json.value("metadata", nlohmann::json::object());
		document.metadata.terraEngineVersion = metadata.value("terraEngineVersion", "unknown");
		document.metadata.projectName = metadata.value("projectName", "");
		document.metadata.projectPathHint = metadata.value("projectPathHint", "");
		document.metadata.scenePath = metadata.value("scenePath", "");
		document.metadata.startedAt = metadata.value("startedAt", "");
		document.metadata.savedAt = metadata.value("savedAt", "");
		document.metadata.settings = SettingsFromJson(metadata.value("settings", nlohmann::json::object()));
		document.metadata.settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;

		for (const nlohmann::json& frameJson : json.value("frames", nlohmann::json::array()))
		{
			ProfilerFrameRecord frame;
			frame.frameIndex = frameJson.value("frameIndex", 0ull);
			frame.frameStartMs = frameJson.value("frameStartMs", 0.0);
			frame.durationMs = frameJson.value("durationMs", 0.0);
			frame.complete = frameJson.value("complete", true);
			for (const nlohmann::json& moduleJson : frameJson.value("modules", nlohmann::json::array()))
			{
				ProfilerModuleSummary module;
				const std::string moduleName = moduleJson.value("moduleId", "");
				const std::optional<ProfilerModuleId> moduleId = TryModuleIdFromStableName(moduleName);
				if (!moduleId.has_value())
				{
					outError = "Profiler capture contains an unknown module id: " + moduleName;
					return false;
				}
				module.moduleId = *moduleId;
				module.available = moduleJson.value("available", false);
				module.totalMs = moduleJson.value("totalMs", 0.0);
				for (const nlohmann::json& metricJson : moduleJson.value("metrics", nlohmann::json::array()))
				{
					module.metrics.push_back({
						metricJson.value("name", ""),
						metricJson.value("value", 0.0),
						metricJson.value("unit", "")
					});
				}
				frame.moduleSummaries.push_back(std::move(module));
			}
			for (const nlohmann::json& sampleJson : frameJson.value("samples", nlohmann::json::array()))
			{
				ProfilerSample sample;
				sample.sampleId = sampleJson.value("sampleId", 0ull);
				sample.parentSampleId = sampleJson.value("parentSampleId", 0ull);
				const std::string moduleName = sampleJson.value("moduleId", "");
				const std::optional<ProfilerModuleId> moduleId = TryModuleIdFromStableName(moduleName);
				if (!moduleId.has_value())
				{
					outError = "Profiler capture contains an unknown sample module id: " + moduleName;
					return false;
				}
				sample.moduleId = *moduleId;
				sample.name = sampleJson.value("name", "");
				sample.threadLabel = sampleJson.value("threadLabel", "");
				sample.taskLabel = sampleJson.value("taskLabel", "");
				sample.startMs = sampleJson.value("startMs", 0.0);
				sample.durationMs = sampleJson.value("durationMs", 0.0);
				sample.depth = sampleJson.value("depth", 0u);
				sample.occurrenceCount = sampleJson.value("occurrenceCount", 1u);
				sample.complete = sampleJson.value("complete", true);
				frame.samples.push_back(std::move(sample));
			}
			document.frames.push_back(std::move(frame));
		}

		outDocument = std::move(document);
		return true;
	}
	catch (const std::exception& exception)
	{
		outError = exception.what();
		return false;
	}
}

bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
{
	ProfilerCaptureDocument document;
	std::string error;
	outArtifact = {};
	outArtifact.absolutePath = path;
	outArtifact.fileName = path.filename().string();
	if (!LoadCaptureDocument(path, document, error))
	{
		outArtifact.supported = false;
		outArtifact.status = error;
		return false;
	}

	outArtifact.metadata = document.metadata;
	outArtifact.frameCount = static_cast<std::uint64_t>(document.frames.size());
	outArtifact.payloadVersion = kProfilerCaptureVersion;
	outArtifact.supported = true;
	outArtifact.status = "Ready";
	outArtifact.moduleIds = {
		ProfilerModuleId::CpuEngine,
		ProfilerModuleId::TaskExecution
	};
	return true;
}
}
