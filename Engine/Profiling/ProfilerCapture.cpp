// ProfilerCapture.cpp — Contains the Profiler Capture file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/ProfilerCapture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

namespace TerraEngine::Profiling
{
namespace
{
constexpr std::uint32_t kProfilerCaptureVersion = 1u;

// ModuleIdToStableName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// TryModuleIdFromStableName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<ProfilerModuleId> TryModuleIdFromStableName(const std::string& value)
{
	if (value == "cpu_engine")
	{
		return ProfilerModuleId::CpuEngine;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (value == "task_execution")
	{
		return ProfilerModuleId::TaskExecution;
	}
	return std::nullopt;
}

std::string TimingStatusToStableName(ProfilerTimingStreamStatus status)
{
	switch (status)
	{
	case ProfilerTimingStreamStatus::Valid:
		return "valid";
	case ProfilerTimingStreamStatus::Unavailable:
		return "unavailable";
	case ProfilerTimingStreamStatus::Stale:
		return "stale";
	case ProfilerTimingStreamStatus::Delayed:
		return "delayed";
	case ProfilerTimingStreamStatus::Invalid:
		return "invalid";
	}
	return "unavailable";
}

ProfilerTimingStreamStatus TimingStatusFromStableName(const std::string& value)
{
	if (value == "valid")
	{
		return ProfilerTimingStreamStatus::Valid;
	}
	if (value == "stale")
	{
		return ProfilerTimingStreamStatus::Stale;
	}
	if (value == "delayed")
	{
		return ProfilerTimingStreamStatus::Delayed;
	}
	if (value == "invalid")
	{
		return ProfilerTimingStreamStatus::Invalid;
	}
	return ProfilerTimingStreamStatus::Unavailable;
}

std::string TimingScopeToStableName(ProfilerTimingStreamScope scope)
{
	switch (scope)
	{
	case ProfilerTimingStreamScope::EngineCpu:
		return "engine_cpu";
	case ProfilerTimingStreamScope::EditorWall:
		return "editor_wall";
	case ProfilerTimingStreamScope::ViewportCpu:
		return "viewport_cpu";
	case ProfilerTimingStreamScope::ViewportGpu:
		return "viewport_gpu";
	}
	return "engine_cpu";
}

std::optional<ProfilerTimingStreamScope> TryTimingScopeFromStableName(const std::string& value)
{
	if (value == "engine_cpu")
	{
		return ProfilerTimingStreamScope::EngineCpu;
	}
	if (value == "editor_wall")
	{
		return ProfilerTimingStreamScope::EditorWall;
	}
	if (value == "viewport_cpu")
	{
		return ProfilerTimingStreamScope::ViewportCpu;
	}
	if (value == "viewport_gpu")
	{
		return ProfilerTimingStreamScope::ViewportGpu;
	}
	return std::nullopt;
}

std::string TimingSourceToStableName(ProfilerTimingStreamSource source)
{
	switch (source)
	{
	case ProfilerTimingStreamSource::ProfilerClock:
		return "profiler_clock";
	case ProfilerTimingStreamSource::RenderStats:
		return "render_stats";
	case ProfilerTimingStreamSource::BackendGpuTimestamp:
		return "backend_gpu_timestamp";
	case ProfilerTimingStreamSource::Unavailable:
		return "unavailable";
	}
	return "unavailable";
}

ProfilerTimingStreamSource TimingSourceFromStableName(const std::string& value)
{
	if (value == "profiler_clock")
	{
		return ProfilerTimingStreamSource::ProfilerClock;
	}
	if (value == "render_stats")
	{
		return ProfilerTimingStreamSource::RenderStats;
	}
	if (value == "backend_gpu_timestamp")
	{
		return ProfilerTimingStreamSource::BackendGpuTimestamp;
	}
	return ProfilerTimingStreamSource::Unavailable;
}

double PercentileFromSortedValues(const std::vector<double>& sortedValues, double percentile)
{
	if (sortedValues.empty())
	{
		return 0.0;
	}
	if (sortedValues.size() == 1u)
	{
		return sortedValues.front();
	}
	const double scaledIndex = std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sortedValues.size() - 1u);
	const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(scaledIndex));
	const std::size_t upperIndex = std::min(lowerIndex + 1u, sortedValues.size() - 1u);
	const double blend = scaledIndex - static_cast<double>(lowerIndex);
	return sortedValues[lowerIndex] + (sortedValues[upperIndex] - sortedValues[lowerIndex]) * blend;
}

// ToLocalTime is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// SettingsToJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// SettingsFromJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// SaxContainerKind names the active JSON container so the metadata reader can
// tell top-level capture fields from similarly named fields inside frames.
enum class SaxContainerKind
{
	Object,
	Array
};

// SaxContainer stores the current object/array name and the object key that is
// waiting for its value. Keeping this tiny stack avoids building a full JSON
// document while scanning large profiler captures for sidebar metadata.
struct SaxContainer
{
	SaxContainerKind kind = SaxContainerKind::Object;
	std::string name;
	std::string pendingKey;
};

// ProfilerArtifactMetadataSax streams enough capture data for the artifact
// list. Full frame/sample validation intentionally remains in LoadCaptureDocument.
class ProfilerArtifactMetadataSax final : public nlohmann::json_sax<nlohmann::json>
{
public:
	using Base = nlohmann::json_sax<nlohmann::json>;
	using binary_t = typename Base::binary_t;
	using number_float_t = typename Base::number_float_t;
	using number_integer_t = typename Base::number_integer_t;
	using number_unsigned_t = typename Base::number_unsigned_t;
	using string_t = typename Base::string_t;

	explicit ProfilerArtifactMetadataSax(ProfilerCaptureArtifact& artifact)
		: outArtifact(artifact)
	{
	}

	bool null() override
	{
		ConsumePendingKey();
		return true;
	}

	bool boolean(bool value) override
	{
		const std::string key = ConsumePendingKey();
		if (ParentIsNamedObject("settings"))
		{
			if (key == "cpuEngineEnabled")
			{
				outArtifact.metadata.settings.cpuEngineEnabled = value;
			}
			else if (key == "taskExecutionEnabled")
			{
				outArtifact.metadata.settings.taskExecutionEnabled = value;
			}
			else if (key == "recordMarkers")
			{
				outArtifact.metadata.settings.recordMarkers = value;
			}
			else if (key == "recordTaskContext")
			{
				outArtifact.metadata.settings.recordTaskContext = value;
			}
		}
		return true;
	}

	bool number_integer(number_integer_t value) override
	{
		if (value < 0)
		{
			ConsumePendingKey();
			return true;
		}
		return number_unsigned(static_cast<number_unsigned_t>(value));
	}

	bool number_unsigned(number_unsigned_t value) override
	{
		const std::string key = ConsumePendingKey();
		if (IsTopLevelObject() && key == "payloadVersion")
		{
			hasPayloadVersion = true;
			outArtifact.payloadVersion = static_cast<std::uint32_t>(value);
			if (outArtifact.payloadVersion != kProfilerCaptureVersion)
			{
				error = "Profiler capture format version is unsupported.";
				return false;
			}
		}
		else if (IsTopLevelObject() && key == "frameCount")
		{
			hasFrameCount = true;
			outArtifact.frameCount = static_cast<std::uint64_t>(value);
		}
		else if (ParentIsNamedObject("settings") && key == "maxFrames")
		{
			outArtifact.metadata.settings.maxFrames = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(value));
		}
		return true;
	}

	bool number_float(number_float_t value, const string_t&) override
	{
		const std::string key = ConsumePendingKey();
		if (ParentIsNamedObject("settings") && key == "maxDurationSeconds")
		{
			outArtifact.metadata.settings.maxDurationSeconds = std::max(0.0, static_cast<double>(value));
		}
		return true;
	}

	bool string(string_t& value) override
	{
		const std::string key = ConsumePendingKey();
		if (IsTopLevelObject() && key == "format")
		{
			hasFormat = true;
			if (value != "TerraEngineProfilerCapture")
			{
				error = "File is not a TerraEngine profiler capture.";
				return false;
			}
		}
		else if (ParentIsNamedObject("metadata"))
		{
			if (key == "terraEngineVersion")
			{
				outArtifact.metadata.terraEngineVersion = value;
			}
			else if (key == "projectName")
			{
				outArtifact.metadata.projectName = value;
			}
			else if (key == "projectPathHint")
			{
				outArtifact.metadata.projectPathHint = value;
			}
			else if (key == "scenePath")
			{
				outArtifact.metadata.scenePath = value;
			}
			else if (key == "startedAt")
			{
				outArtifact.metadata.startedAt = value;
			}
			else if (key == "savedAt")
			{
				outArtifact.metadata.savedAt = value;
			}
		}
		return true;
	}

	bool binary(binary_t&) override
	{
		ConsumePendingKey();
		return true;
	}

	bool start_object(std::size_t) override
	{
		std::string valueName = ConsumePendingKey();
		if (!containers.empty()
			&& containers.back().kind == SaxContainerKind::Array
			&& containers.back().name == "frames"
			&& containers.size() == 2u)
		{
			if (!hasFrameCount)
			{
				++outArtifact.frameCount;
			}
			valueName = "frame";
		}
		containers.push_back({SaxContainerKind::Object, std::move(valueName), {}});
		return true;
	}

	bool key(string_t& value) override
	{
		if (containers.empty() || containers.back().kind != SaxContainerKind::Object)
		{
			error = "Profiler capture JSON contains an object key outside an object.";
			return false;
		}
		containers.back().pendingKey = value;
		return true;
	}

	bool end_object() override
	{
		if (containers.empty() || containers.back().kind != SaxContainerKind::Object)
		{
			error = "Profiler capture JSON ended an object without a matching start.";
			return false;
		}
		const std::string endedName = containers.back().name;
		containers.pop_back();
		if (endedName == "metadata")
		{
			hasMetadata = true;
		}
		return true;
	}

	bool start_array(std::size_t elements) override
	{
		std::string valueName = ConsumePendingKey();
		if (IsTopLevelObject() && valueName == "frames")
		{
			if (CanStopBeforeFrames())
			{
				completedEarly = true;
				return false;
			}
			if (!hasFrameCount && elements != nlohmann::detail::unknown_size())
			{
				hasFrameCount = true;
				outArtifact.frameCount = static_cast<std::uint64_t>(elements);
			}
		}
		containers.push_back({SaxContainerKind::Array, std::move(valueName), {}});
		return true;
	}

	bool end_array() override
	{
		if (containers.empty() || containers.back().kind != SaxContainerKind::Array)
		{
			error = "Profiler capture JSON ended an array without a matching start.";
			return false;
		}
		const std::string endedName = containers.back().name;
		containers.pop_back();
		if (endedName == "frames")
		{
			hasFrameCount = true;
		}
		return true;
	}

	bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& exception) override
	{
		error = exception.what();
		return false;
	}

	bool Finish()
	{
		if (!hasFormat)
		{
			error = "File is not a TerraEngine profiler capture.";
			return false;
		}
		if (!hasPayloadVersion)
		{
			error = "Profiler capture format version is unsupported.";
			return false;
		}
		if (!hasMetadata)
		{
			error = "Profiler capture metadata is missing.";
			return false;
		}
		if (!hasFrameCount)
		{
			error = "Profiler capture frame list is missing.";
			return false;
		}

		outArtifact.metadata.settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;
		outArtifact.supported = true;
		outArtifact.status = completedEarly ? "Ready (metadata)" : "Ready";
		outArtifact.moduleIds = {
			ProfilerModuleId::CpuEngine,
			ProfilerModuleId::TaskExecution
		};
		return true;
	}

	ProfilerCaptureArtifact& outArtifact;
	std::string error;
	bool completedEarly = false;

private:
	bool IsTopLevelObject() const
	{
		return containers.size() == 1u && containers.back().kind == SaxContainerKind::Object;
	}

	bool ParentIsNamedObject(const char* name) const
	{
		return !containers.empty()
			&& containers.back().kind == SaxContainerKind::Object
			&& containers.back().name == name;
	}

	bool CanStopBeforeFrames() const
	{
		return hasFormat && hasPayloadVersion && hasMetadata && hasFrameCount;
	}

	std::string ConsumePendingKey()
	{
		if (containers.empty() || containers.back().kind != SaxContainerKind::Object)
		{
			return {};
		}
		std::string key = std::move(containers.back().pendingKey);
		containers.back().pendingKey.clear();
		return key;
	}

	std::vector<SaxContainer> containers;
	bool hasFormat = false;
	bool hasPayloadVersion = false;
	bool hasMetadata = false;
	bool hasFrameCount = false;
};
}

// ToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

const char* ToString(ProfilerTimingStreamStatus status)
{
	switch (status)
	{
	case ProfilerTimingStreamStatus::Valid:
		return "valid";
	case ProfilerTimingStreamStatus::Unavailable:
		return "unavailable";
	case ProfilerTimingStreamStatus::Stale:
		return "stale";
	case ProfilerTimingStreamStatus::Delayed:
		return "delayed";
	case ProfilerTimingStreamStatus::Invalid:
		return "invalid";
	}
	return "unavailable";
}

const char* ToString(ProfilerTimingStreamScope scope)
{
	switch (scope)
	{
	case ProfilerTimingStreamScope::EngineCpu:
		return "Engine CPU";
	case ProfilerTimingStreamScope::EditorWall:
		return "Editor Wall";
	case ProfilerTimingStreamScope::ViewportCpu:
		return "Viewport CPU";
	case ProfilerTimingStreamScope::ViewportGpu:
		return "Viewport GPU";
	}
	return "Engine CPU";
}

const char* ToString(ProfilerTimingStreamSource source)
{
	switch (source)
	{
	case ProfilerTimingStreamSource::ProfilerClock:
		return "profiler clock";
	case ProfilerTimingStreamSource::RenderStats:
		return "render stats";
	case ProfilerTimingStreamSource::BackendGpuTimestamp:
		return "GPU timestamp";
	case ProfilerTimingStreamSource::Unavailable:
		return "unavailable";
	}
	return "unavailable";
}

// MakeTimestampForFileName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string MakeTimestampForFileName()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
	const std::tm localTime = ToLocalTime(timeValue);
	std::ostringstream stream;
	stream << std::put_time(&localTime, "%Y%m%d-%H%M%S");
	return stream.str();
}

// MakeReadableTimestamp is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string MakeReadableTimestamp()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
	const std::tm localTime = ToLocalTime(timeValue);
	std::ostringstream stream;
	stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
	return stream.str();
}

// AnyModuleEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AnyModuleEnabled(const ProfilerCaptureSettings& settings)
{
	return settings.cpuEngineEnabled || settings.taskExecutionEnabled;
}

// MakeDefaultModuleSummaries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// TrimFrameBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void TrimFrameBuffer(std::vector<ProfilerFrameRecord>& frames, const ProfilerCaptureSettings& settings)
{
	const std::size_t maxFrames = std::max<std::size_t>(1u, settings.maxFrames);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frames.size() > maxFrames)
	{
		frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(frames.size() - maxFrames));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (settings.maxDurationSeconds > 0.0 && !frames.empty())
	{
		const double newestStart = frames.back().frameStartMs;
		const double cutoff = newestStart - settings.maxDurationSeconds * 1000.0;
		const auto firstKept = std::find_if(frames.begin(), frames.end(), [cutoff](const ProfilerFrameRecord& frame) {
			return frame.frameStartMs >= cutoff;
		});
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (firstKept != frames.begin())
		{
			frames.erase(frames.begin(), firstKept);
		}
	}
}

void AddOrReplaceTimingStreamSample(ProfilerFrameRecord& frame, ProfilerTimingStreamSample sample)
{
	if (sample.id.empty())
	{
		sample.id = TimingScopeToStableName(sample.scope);
	}
	if (sample.label.empty())
	{
		sample.label = ToString(sample.scope);
	}

	const auto found = std::find_if(frame.timingStreams.begin(), frame.timingStreams.end(), [&sample](const ProfilerTimingStreamSample& existing) {
		return existing.id == sample.id;
	});
	if (found == frame.timingStreams.end())
	{
		frame.timingStreams.push_back(std::move(sample));
		return;
	}
	*found = std::move(sample);
}

std::vector<ProfilerTimingStreamAggregate> BuildTimingStreamAggregates(const std::vector<ProfilerFrameRecord>& frames, const std::optional<ProfilerFrameRecord>& selectedFrame)
{
	std::vector<ProfilerTimingStreamAggregate> aggregates;
	const auto findAggregate = [&aggregates](const ProfilerTimingStreamSample& sample) -> ProfilerTimingStreamAggregate& {
		const auto found = std::find_if(aggregates.begin(), aggregates.end(), [&sample](const ProfilerTimingStreamAggregate& aggregate) {
			return aggregate.id == sample.id;
		});
		if (found != aggregates.end())
		{
			return *found;
		}

		ProfilerTimingStreamAggregate aggregate;
		aggregate.id = sample.id;
		aggregate.label = sample.label.empty() ? ToString(sample.scope) : sample.label;
		aggregate.scope = sample.scope;
		aggregate.source = sample.source;
		aggregates.push_back(std::move(aggregate));
		return aggregates.back();
	};

	for (const ProfilerFrameRecord& frame : frames)
	{
		for (const ProfilerTimingStreamSample& sample : frame.timingStreams)
		{
			findAggregate(sample);
		}
	}

	for (ProfilerTimingStreamAggregate& aggregate : aggregates)
	{
		std::vector<double> values;
		values.reserve(frames.size());
		for (const ProfilerFrameRecord& frame : frames)
		{
			const auto sample = std::find_if(frame.timingStreams.begin(), frame.timingStreams.end(), [&aggregate](const ProfilerTimingStreamSample& candidate) {
				return candidate.id == aggregate.id;
			});
			if (sample == frame.timingStreams.end())
			{
				++aggregate.missingCount;
				continue;
			}
			if (sample->status == ProfilerTimingStreamStatus::Valid && std::isfinite(sample->milliseconds))
			{
				values.push_back(sample->milliseconds);
			}
			else
			{
				++aggregate.missingCount;
			}
		}

		aggregate.sampleCount = values.size();
		if (!values.empty())
		{
			const double sum = std::accumulate(values.begin(), values.end(), 0.0);
			aggregate.meanMs = sum / static_cast<double>(values.size());
			double varianceSum = 0.0;
			for (double value : values)
			{
				const double delta = value - aggregate.meanMs;
				varianceSum += delta * delta;
			}
			aggregate.stdDevMs = std::sqrt(varianceSum / static_cast<double>(values.size()));
			std::sort(values.begin(), values.end());
			aggregate.minMs = values.front();
			aggregate.maxMs = values.back();
			aggregate.medianMs = PercentileFromSortedValues(values, 0.50);
			aggregate.p90Ms = PercentileFromSortedValues(values, 0.90);
			aggregate.p95Ms = PercentileFromSortedValues(values, 0.95);
			aggregate.p99Ms = PercentileFromSortedValues(values, 0.99);
		}

		if (selectedFrame.has_value())
		{
			const auto selectedSample = std::find_if(selectedFrame->timingStreams.begin(), selectedFrame->timingStreams.end(), [&aggregate](const ProfilerTimingStreamSample& sample) {
				return sample.id == aggregate.id;
			});
			if (selectedSample != selectedFrame->timingStreams.end())
			{
				aggregate.selectedStatus = selectedSample->status;
				aggregate.selectedMilliseconds = selectedSample->milliseconds;
				aggregate.selectedStatusReason = selectedSample->statusReason;
			}
		}
	}

	std::sort(aggregates.begin(), aggregates.end(), [](const ProfilerTimingStreamAggregate& left, const ProfilerTimingStreamAggregate& right) {
		return static_cast<std::uint8_t>(left.scope) < static_cast<std::uint8_t>(right.scope);
	});
	return aggregates;
}

// SaveCaptureDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool SaveCaptureDocument(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError)
{
	try
	{
		nlohmann::ordered_json json;
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
		json["frameCount"] = document.frames.size();
		json["frames"] = nlohmann::json::array();

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ProfilerFrameRecord& frame : document.frames)
		{
			nlohmann::json frameJson;
			frameJson["frameIndex"] = frame.frameIndex;
			frameJson["frameStartMs"] = frame.frameStartMs;
			frameJson["durationMs"] = frame.durationMs;
			frameJson["complete"] = frame.complete;
			if (frame.physics.has_value())
			{
				const ProfilerPhysicsFrameCounters& physics = *frame.physics;
				frameJson["physics"] = {
					{"tick", physics.tick}, {"bodyCount", physics.bodyCount}, {"contactCount", physics.contactCount},
					{"queryCount", physics.queryCount}, {"eventCount", physics.eventCount}, {"rejectedCount", physics.rejectedCount},
					{"staleCount", physics.staleCount}, {"commandQueueDepth", physics.commandQueueDepth},
					{"shapeCacheBytes", physics.shapeCacheBytes}, {"shapeCacheHits", physics.shapeCacheHits},
					{"shapeCacheMisses", physics.shapeCacheMisses}, {"snapshotBytes", physics.snapshotBytes},
					{"snapshotHighWaterBytes", physics.snapshotHighWaterBytes}, {"providerAllocations", physics.providerAllocations},
					{"providerAllocationHighWater", physics.providerAllocationHighWater},
					{"prepareMs", physics.prepareMs}, {"commitMs", physics.commitMs}, {"simulateMs", physics.simulateMs},
					{"queryMs", physics.queryMs}, {"publishMs", physics.publishMs}
				};
			}
			frameJson["timingStreams"] = nlohmann::json::array();
			// Timing streams are stored beside modules because they answer a
			// different question: "which clock measured this frame?" rather than
			// "which profiler module owned these samples?"
			for (const ProfilerTimingStreamSample& stream : frame.timingStreams)
			{
				frameJson["timingStreams"].push_back({
					{"id", stream.id},
					{"label", stream.label},
					{"scope", TimingScopeToStableName(stream.scope)},
					{"source", TimingSourceToStableName(stream.source)},
					{"status", TimingStatusToStableName(stream.status)},
					{"milliseconds", stream.milliseconds},
					{"statusReason", stream.statusReason}
				});
			}
			frameJson["modules"] = nlohmann::json::array();
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const ProfilerModuleSummary& module : frame.moduleSummaries)
			{
				nlohmann::json moduleJson;
				moduleJson["moduleId"] = ModuleIdToStableName(module.moduleId);
				moduleJson["available"] = module.available;
				moduleJson["totalMs"] = module.totalMs;
				moduleJson["metrics"] = nlohmann::json::array();
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const ProfilerMetric& metric : module.metrics)
				{
					moduleJson["metrics"].push_back({{"name", metric.name}, {"value", metric.value}, {"unit", metric.unit}});
				}
				frameJson["modules"].push_back(std::move(moduleJson));
			}
			frameJson["samples"] = nlohmann::json::array();
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// LoadCaptureDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool LoadCaptureDocument(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError)
{
	try
	{
		std::ifstream input(path, std::ios::binary);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!input)
		{
			outError = "Failed to open profiler capture for reading.";
			return false;
		}

		nlohmann::json json;
		input >> json;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (json.value("format", std::string{}) != "TerraEngineProfilerCapture")
		{
			outError = "File is not a TerraEngine profiler capture.";
			return false;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const nlohmann::json& frameJson : json.value("frames", nlohmann::json::array()))
		{
			ProfilerFrameRecord frame;
			frame.frameIndex = frameJson.value("frameIndex", 0ull);
			frame.frameStartMs = frameJson.value("frameStartMs", 0.0);
			frame.durationMs = frameJson.value("durationMs", 0.0);
			frame.complete = frameJson.value("complete", true);
			if (frameJson.contains("physics") && frameJson["physics"].is_object())
			{
				const nlohmann::json& physicsJson = frameJson["physics"];
				ProfilerPhysicsFrameCounters physics;
				physics.tick = physicsJson.value("tick", 0ull);
				physics.bodyCount = physicsJson.value("bodyCount", 0ull);
				physics.contactCount = physicsJson.value("contactCount", 0ull);
				physics.queryCount = physicsJson.value("queryCount", 0ull);
				physics.eventCount = physicsJson.value("eventCount", 0ull);
				physics.rejectedCount = physicsJson.value("rejectedCount", 0ull);
				physics.staleCount = physicsJson.value("staleCount", 0ull);
				physics.commandQueueDepth = physicsJson.value("commandQueueDepth", 0ull);
				physics.shapeCacheBytes = physicsJson.value("shapeCacheBytes", 0ull);
				physics.shapeCacheHits = physicsJson.value("shapeCacheHits", 0ull);
				physics.shapeCacheMisses = physicsJson.value("shapeCacheMisses", 0ull);
				physics.snapshotBytes = physicsJson.value("snapshotBytes", 0ull);
				physics.snapshotHighWaterBytes = physicsJson.value("snapshotHighWaterBytes", 0ull);
				physics.providerAllocations = physicsJson.value("providerAllocations", 0ull);
				physics.providerAllocationHighWater = physicsJson.value("providerAllocationHighWater", 0ull);
				physics.prepareMs = physicsJson.value("prepareMs", 0.0);
				physics.commitMs = physicsJson.value("commitMs", 0.0);
				physics.simulateMs = physicsJson.value("simulateMs", 0.0);
				physics.queryMs = physicsJson.value("queryMs", 0.0);
				physics.publishMs = physicsJson.value("publishMs", 0.0);
				frame.physics = physics;
			}
			for (const nlohmann::json& streamJson : frameJson.value("timingStreams", nlohmann::json::array()))
			{
				ProfilerTimingStreamSample stream;
				stream.id = streamJson.value("id", "");
				stream.label = streamJson.value("label", "");
				const std::string scopeName = streamJson.value("scope", "");
				const std::optional<ProfilerTimingStreamScope> scope = TryTimingScopeFromStableName(scopeName);
				if (!scope.has_value())
				{
					outError = "Profiler capture contains an unknown timing stream scope: " + scopeName;
					return false;
				}
				stream.scope = *scope;
				stream.source = TimingSourceFromStableName(streamJson.value("source", ""));
				stream.status = TimingStatusFromStableName(streamJson.value("status", ""));
				stream.milliseconds = streamJson.value("milliseconds", 0.0);
				stream.statusReason = streamJson.value("statusReason", "");
				AddOrReplaceTimingStreamSample(frame, std::move(stream));
			}
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const nlohmann::json& moduleJson : frameJson.value("modules", nlohmann::json::array()))
			{
				ProfilerModuleSummary module;
				const std::string moduleName = moduleJson.value("moduleId", "");
				const std::optional<ProfilerModuleId> moduleId = TryModuleIdFromStableName(moduleName);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!moduleId.has_value())
				{
					outError = "Profiler capture contains an unknown module id: " + moduleName;
					return false;
				}
				module.moduleId = *moduleId;
				module.available = moduleJson.value("available", false);
				module.totalMs = moduleJson.value("totalMs", 0.0);
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const nlohmann::json& sampleJson : frameJson.value("samples", nlohmann::json::array()))
			{
				ProfilerSample sample;
				sample.sampleId = sampleJson.value("sampleId", 0ull);
				sample.parentSampleId = sampleJson.value("parentSampleId", 0ull);
				const std::string moduleName = sampleJson.value("moduleId", "");
				const std::optional<ProfilerModuleId> moduleId = TryModuleIdFromStableName(moduleName);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// TryReadCaptureArtifact is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool TryReadCaptureArtifactMetadata(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
{
	outArtifact = {};
	outArtifact.absolutePath = path;
	outArtifact.fileName = path.filename().string();

	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		outArtifact.supported = false;
		outArtifact.status = "Failed to open profiler capture for reading.";
		return false;
	}

	ProfilerArtifactMetadataSax sax(outArtifact);
	const bool parsed = nlohmann::json::sax_parse(input, &sax);
	if (!parsed && !sax.completedEarly)
	{
		outArtifact.supported = false;
		outArtifact.status = sax.error.empty() ? "Failed to parse profiler capture metadata." : sax.error;
		return false;
	}
	if (!sax.Finish())
	{
		outArtifact.supported = false;
		outArtifact.status = sax.error;
		return false;
	}
	return true;
}

// TryReadCaptureArtifact is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
{
	ProfilerCaptureDocument document;
	std::string error;
	outArtifact = {};
	outArtifact.absolutePath = path;
	outArtifact.fileName = path.filename().string();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
