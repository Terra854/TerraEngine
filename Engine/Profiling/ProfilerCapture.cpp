// ProfilerCapture.cpp — Contains the Profiler Capture file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/ProfilerCapture.h"
#include "Profiling/ProfilerCaptureV2.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
	// Windows.h otherwise injects min/max macros that corrupt the typed
	// std::min/std::max bounds checks used by untrusted capture readers.
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#endif

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

const char* ToString(ProfilerCaptureDetailLevel level)
{
	switch (level)
	{
	case ProfilerCaptureDetailLevel::Detailed:
		return "detailed";
	case ProfilerCaptureDetailLevel::Sampled:
		return "sampled";
	case ProfilerCaptureDetailLevel::Aggregate:
		return "aggregate";
	}
	return "detailed";
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
	// New captures use the compact v2 framing. A caller can still deliberately
	// write a v1 fixture by setting payloadVersion to one; that compatibility
	// escape hatch never rewrites an existing artifact in place.
	if (document.payloadVersion >= kProfilerCaptureV2PayloadVersion)
	{
		return SaveProfilerCaptureV2(document, path, outError);
	}
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
	// V2 is line framed so a trailing partial chunk can be recovered without
	// asking the JSON parser to accept a malformed whole-file document.
	{
		std::ifstream versionInput(path, std::ios::binary);
		std::string firstLine;
		if (versionInput && std::getline(versionInput, firstLine))
		{
			if (firstLine == "{")
			{
				const std::string remainder((std::istreambuf_iterator<char>(versionInput)), std::istreambuf_iterator<char>());
				if (remainder.find("\"payloadVersion\": 2") != std::string::npos)
				{
					return LoadProfilerCaptureV2(path, outDocument, outError);
				}
			}
			try
			{
				const nlohmann::json header = nlohmann::json::parse(firstLine);
				if (header.value("payloadVersion", 0u) == kProfilerCaptureV2PayloadVersion)
				{
					return LoadProfilerCaptureV2(path, outDocument, outError);
				}
			}
			catch (const nlohmann::json::exception&)
			{
				// The v1 reader below retains its detailed parse error for legacy
				// artifacts rather than hiding it behind a version probe failure.
			}
		}
	}
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
		document.payloadVersion = kProfilerCaptureVersion;
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
	{
		std::ifstream versionInput(path, std::ios::binary);
		std::string firstLine;
		if (versionInput && std::getline(versionInput, firstLine))
		{
			if (firstLine == "{")
			{
				const std::string remainder((std::istreambuf_iterator<char>(versionInput)), std::istreambuf_iterator<char>());
				if (remainder.find("\"payloadVersion\": 2") != std::string::npos)
				{
					return TryReadProfilerCaptureV2Metadata(path, outArtifact);
				}
			}
			try
			{
				const nlohmann::json header = nlohmann::json::parse(firstLine);
				if (header.value("payloadVersion", 0u) == kProfilerCaptureV2PayloadVersion)
				{
					return TryReadProfilerCaptureV2Metadata(path, outArtifact);
				}
			}
			catch (const nlohmann::json::exception&)
			{
			}
		}
	}
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
	outArtifact.payloadVersion = document.payloadVersion;
	outArtifact.committedFrameCount = document.committedFrameCount == 0u
		? static_cast<std::uint64_t>(document.frames.size())
		: document.committedFrameCount;
	outArtifact.complete = document.complete;
	outArtifact.recoveredIncompleteTail = document.recoveredIncompleteTail;
	outArtifact.supported = true;
	outArtifact.status = "Ready";
	outArtifact.moduleIds = {
		ProfilerModuleId::CpuEngine,
		ProfilerModuleId::TaskExecution
	};
	return true;
}

namespace
{
// V2 is newline framed intentionally: each committed chunk is independently
// parseable, so a process loss while writing the final line cannot invalidate
// previously flushed evidence. This is a persistence boundary, not a live-data
// synchronization mechanism.
constexpr std::uint32_t kV2MaximumStringCount = 1u << 20u;
constexpr std::uint32_t kV2MaximumFramesPerChunk = 4096u;

std::uint64_t ToNanoseconds(double milliseconds)
{
	return static_cast<std::uint64_t>(std::max(0.0, milliseconds) * 1000000.0 + 0.5);
}

double ToMilliseconds(std::uint64_t nanoseconds)
{
	return static_cast<double>(nanoseconds) / 1000000.0;
}

std::uint32_t Fnv1a32(const std::string& value)
{
	std::uint32_t hash = 2166136261u;
	for (const unsigned char byte : value)
	{
		hash ^= byte;
		hash *= 16777619u;
	}
	return hash;
}

class V2StringTable final
{
public:
	V2StringTable()
	{
		Intern({});
	}

	std::uint32_t Intern(const std::string& value)
	{
		const auto existing = indices.find(value);
		if (existing != indices.end())
		{
			return existing->second;
		}
		const std::uint32_t index = static_cast<std::uint32_t>(values.size());
		indices.emplace(value, index);
		values.push_back(value);
		return index;
	}

	[[nodiscard]] const std::vector<std::string>& Values() const { return values; }

private:
	std::unordered_map<std::string, std::uint32_t> indices;
	std::vector<std::string> values;
};

void CollectDuration(V2StringTable& table, const TelemetryDuration& duration)
{
	table.Intern(duration.unavailableReason);
}

void CollectFrameStrings(V2StringTable& table, const ProfilerFrameRecord& frame)
{
	for (const ProfilerTimingStreamSample& stream : frame.timingStreams)
	{
		table.Intern(stream.id); table.Intern(stream.label); table.Intern(stream.statusReason);
	}
	for (const ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		for (const ProfilerMetric& metric : module.metrics)
		{
			table.Intern(metric.name); table.Intern(metric.unit);
		}
	}
	for (const ProfilerSample& sample : frame.samples)
	{
		table.Intern(sample.name); table.Intern(sample.threadLabel); table.Intern(sample.taskLabel);
	}
	for (const PerformanceStageRecord& stage : frame.performanceStages)
	{
		table.Intern(stage.stageName); table.Intern(stage.workloadName); CollectDuration(table, stage.duration);
	}
	for (const ProtectedBoundaryRecord& boundary : frame.protectedBoundaries)
	{
		table.Intern(boundary.boundaryName);
		CollectDuration(table, boundary.queueDuration); CollectDuration(table, boundary.ownershipWaitDuration);
		CollectDuration(table, boundary.executionDuration); CollectDuration(table, boundary.flushDuration);
		CollectDuration(table, boundary.publicationDuration);
	}
	for (const WorkerTelemetryRecord& worker : frame.workerTelemetry)
	{
		CollectDuration(table, worker.runDuration); CollectDuration(table, worker.waitDuration); CollectDuration(table, worker.idleDuration);
	}
	for (const PresentationWaitRecord& wait : frame.presentationWaits)
	{
		table.Intern(wait.detail); CollectDuration(table, wait.duration);
	}
	if (frame.responsiveness.has_value())
	{
		CollectDuration(table, frame.responsiveness->inputToPresentationDuration);
		CollectDuration(table, frame.responsiveness->frameDuration);
	}
}

nlohmann::json PackDuration(const TelemetryDuration& duration, V2StringTable& table)
{
	return nlohmann::json::array({
		static_cast<std::uint8_t>(duration.availability), duration.nanoseconds, table.Intern(duration.unavailableReason)});
}

bool UnpackDuration(const nlohmann::json& json, const std::vector<std::string>& strings, TelemetryDuration& out, std::string& error)
{
	if (!json.is_array() || json.size() != 3u || !json[0].is_number_unsigned() || !json[1].is_number_unsigned() || !json[2].is_number_unsigned())
	{
		error = "Profiler v2 contains a malformed duration.";
		return false;
	}
	const std::size_t reasonIndex = json[2].get<std::size_t>();
	if (reasonIndex >= strings.size())
	{
		error = "Profiler v2 duration references an out-of-range string.";
		return false;
	}
	out.availability = static_cast<TelemetryAvailability>(json[0].get<std::uint8_t>());
	out.nanoseconds = json[1].get<std::uint64_t>();
	out.unavailableReason = strings[reasonIndex];
	if (!ValidateTelemetryDuration(out).valid)
	{
		error = "Profiler v2 contains an invalid duration.";
		return false;
	}
	return true;
}

const std::string* StringAt(const std::vector<std::string>& strings, const nlohmann::json& index, std::string& error)
{
	if (!index.is_number_unsigned())
	{
		error = "Profiler v2 contains a non-numeric string index.";
		return nullptr;
	}
	const std::size_t value = index.get<std::size_t>();
	if (value >= strings.size())
	{
		error = "Profiler v2 contains an out-of-range string index.";
		return nullptr;
	}
	return &strings[value];
}

bool KeepOptionalV2Data(const ProfilerFrameRecord& frame, const ProfilerCaptureSettings& settings)
{
	if (settings.detailLevel == ProfilerCaptureDetailLevel::Detailed)
	{
		return true;
	}
	if (settings.detailLevel == ProfilerCaptureDetailLevel::Sampled)
	{
		return frame.frameIndex % std::max(1u, settings.sampleIntervalFrames) == 0u;
	}
	return false;
}

nlohmann::json PackFrameV2(const ProfilerFrameRecord& frame, const ProfilerCaptureSettings& settings, V2StringTable& table)
{
	nlohmann::json result = {
		{"i", frame.frameIndex}, {"s", ToNanoseconds(frame.frameStartMs)},
		{"d", ToNanoseconds(frame.durationMs)}, {"c", frame.complete}
	};
	const bool includeOptional = KeepOptionalV2Data(frame, settings);
	if (includeOptional)
	{
		result["t"] = nlohmann::json::array();
		for (const ProfilerTimingStreamSample& stream : frame.timingStreams)
		{
			result["t"].push_back(nlohmann::json::array({table.Intern(stream.id), table.Intern(stream.label),
				static_cast<std::uint8_t>(stream.scope), static_cast<std::uint8_t>(stream.source),
				static_cast<std::uint8_t>(stream.status), ToNanoseconds(stream.milliseconds), table.Intern(stream.statusReason)}));
		}
		result["m"] = nlohmann::json::array();
		for (const ProfilerModuleSummary& module : frame.moduleSummaries)
		{
			nlohmann::json metrics = nlohmann::json::array();
			for (const ProfilerMetric& metric : module.metrics)
			{
				metrics.push_back(nlohmann::json::array({table.Intern(metric.name), metric.value, table.Intern(metric.unit)}));
			}
			result["m"].push_back(nlohmann::json::array({static_cast<std::uint8_t>(module.moduleId), module.available, ToNanoseconds(module.totalMs), metrics}));
		}
		result["a"] = nlohmann::json::array();
		for (const ProfilerSample& sample : frame.samples)
		{
			result["a"].push_back(nlohmann::json::array({sample.sampleId, sample.parentSampleId,
				static_cast<std::uint8_t>(sample.moduleId), table.Intern(sample.name), table.Intern(sample.threadLabel),
				table.Intern(sample.taskLabel), ToNanoseconds(sample.startMs), ToNanoseconds(sample.durationMs),
				sample.depth, sample.occurrenceCount, sample.complete}));
		}
	}
	if (frame.physics.has_value())
	{
		const ProfilerPhysicsFrameCounters& value = *frame.physics;
		result["p"] = nlohmann::json::array({value.tick, value.bodyCount, value.contactCount, value.queryCount, value.eventCount,
			value.rejectedCount, value.staleCount, value.commandQueueDepth, value.shapeCacheBytes, value.shapeCacheHits,
			value.shapeCacheMisses, value.snapshotBytes, value.snapshotHighWaterBytes, value.providerAllocations,
			value.providerAllocationHighWater, ToNanoseconds(value.prepareMs), ToNanoseconds(value.commitMs),
			ToNanoseconds(value.simulateMs), ToNanoseconds(value.queryMs), ToNanoseconds(value.publishMs)});
	}
	// Critical paths and correctness records deliberately bypass detail filtering.
	result["g"] = nlohmann::json::array();
	for (const PerformanceStageRecord& stage : frame.performanceStages)
	{
		result["g"].push_back(nlohmann::json::array({static_cast<std::uint8_t>(stage.domain), table.Intern(stage.stageName),
			PackDuration(stage.duration, table), table.Intern(stage.workloadName), stage.workItemCount, stage.sampled}));
	}
	result["b"] = nlohmann::json::array();
	for (const ProtectedBoundaryRecord& boundary : frame.protectedBoundaries)
	{
		result["b"].push_back(nlohmann::json::array({table.Intern(boundary.boundaryName), PackDuration(boundary.queueDuration, table),
			PackDuration(boundary.ownershipWaitDuration, table), PackDuration(boundary.executionDuration, table),
			PackDuration(boundary.flushDuration, table), PackDuration(boundary.publicationDuration, table)}));
	}
	result["w"] = nlohmann::json::array();
	for (const WorkerTelemetryRecord& worker : frame.workerTelemetry)
	{
		result["w"].push_back(nlohmann::json::array({worker.workerId, worker.queueDepth, worker.dispatchCount,
			worker.usefulWorkItemCount, PackDuration(worker.runDuration, table), PackDuration(worker.waitDuration, table), PackDuration(worker.idleDuration, table)}));
	}
	result["q"] = nlohmann::json::array();
	for (const PresentationWaitRecord& wait : frame.presentationWaits)
	{
		result["q"].push_back(nlohmann::json::array({static_cast<std::uint8_t>(wait.kind), PackDuration(wait.duration, table), table.Intern(wait.detail)}));
	}
	if (frame.responsiveness.has_value())
	{
		const FrameResponsivenessRecord& value = *frame.responsiveness;
		result["r"] = nlohmann::json::array({value.frameIndex, PackDuration(value.inputToPresentationDuration, table), PackDuration(value.frameDuration, table), value.presentationSequence});
	}
	if (frame.physicsWorkVolume.has_value())
	{
		const PhysicsWorkVolumeRecord& value = *frame.physicsWorkVolume;
		result["v"] = nlohmann::json::array({value.tick, value.totalBodyCount, value.activeBodyCount, value.examinedBodyCount,
			value.extractedBodyCount, value.convertedBodyCount, value.changedBodyCount, value.publishedBodyCount});
	}
	return result;
}

bool ReadArray(const nlohmann::json& parent, const char* name, std::size_t minimum, const nlohmann::json*& result, std::string& error)
{
	const auto found = parent.find(name);
	if (found == parent.end() || !found->is_array() || found->size() < minimum)
	{
		error = std::string("Profiler v2 contains a malformed '") + name + "' field.";
		return false;
	}
	result = &*found;
	return true;
}

bool UnpackFrameV2(const nlohmann::json& json, const std::vector<std::string>& strings, ProfilerFrameRecord& out, std::string& error)
{
	if (!json.is_object() || !json.contains("i") || !json.contains("s") || !json.contains("d") || !json.contains("c"))
	{
		error = "Profiler v2 contains a malformed frame.";
		return false;
	}
	try
	{
		out.frameIndex = json.at("i").get<std::uint64_t>();
		out.frameStartMs = ToMilliseconds(json.at("s").get<std::uint64_t>());
		out.durationMs = ToMilliseconds(json.at("d").get<std::uint64_t>());
		out.complete = json.at("c").get<bool>();
		if (const auto it = json.find("t"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 7u) { error = "Profiler v2 has malformed timing data."; return false; }
			const std::string* id = StringAt(strings, value[0], error); const std::string* label = StringAt(strings, value[1], error); const std::string* reason = StringAt(strings, value[6], error);
			if (!id || !label || !reason) return false;
			ProfilerTimingStreamSample stream{*id, *label, static_cast<ProfilerTimingStreamScope>(value[2].get<std::uint8_t>()), static_cast<ProfilerTimingStreamSource>(value[3].get<std::uint8_t>()), static_cast<ProfilerTimingStreamStatus>(value[4].get<std::uint8_t>()), ToMilliseconds(value[5].get<std::uint64_t>()), *reason};
			out.timingStreams.push_back(std::move(stream));
		}
		if (const auto it = json.find("m"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 4u || !value[3].is_array()) { error = "Profiler v2 has malformed module data."; return false; }
			ProfilerModuleSummary module{static_cast<ProfilerModuleId>(value[0].get<std::uint8_t>()), value[1].get<bool>(), ToMilliseconds(value[2].get<std::uint64_t>()), {}};
			for (const auto& metric : value[3]) { if (!metric.is_array() || metric.size() != 3u) { error = "Profiler v2 has malformed metric data."; return false; } const std::string* name = StringAt(strings, metric[0], error); const std::string* unit = StringAt(strings, metric[2], error); if (!name || !unit) return false; module.metrics.push_back({*name, metric[1].get<double>(), *unit}); }
			out.moduleSummaries.push_back(std::move(module));
		}
		if (const auto it = json.find("a"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 11u) { error = "Profiler v2 has malformed sample data."; return false; }
			const std::string* name = StringAt(strings, value[3], error); const std::string* thread = StringAt(strings, value[4], error); const std::string* task = StringAt(strings, value[5], error); if (!name || !thread || !task) return false;
			out.samples.push_back({value[0].get<std::uint64_t>(), value[1].get<std::uint64_t>(), static_cast<ProfilerModuleId>(value[2].get<std::uint8_t>()), *name, *thread, *task, ToMilliseconds(value[6].get<std::uint64_t>()), ToMilliseconds(value[7].get<std::uint64_t>()), value[8].get<std::uint32_t>(), value[9].get<std::uint32_t>(), value[10].get<bool>()});
		}
		if (const auto it = json.find("p"); it != json.end())
		{
			if (!it->is_array() || it->size() != 20u) { error = "Profiler v2 has malformed physics counters."; return false; }
			ProfilerPhysicsFrameCounters physics;
			physics.tick = (*it)[0].get<std::uint64_t>(); physics.bodyCount = (*it)[1].get<std::uint64_t>(); physics.contactCount = (*it)[2].get<std::uint64_t>(); physics.queryCount = (*it)[3].get<std::uint64_t>(); physics.eventCount = (*it)[4].get<std::uint64_t>(); physics.rejectedCount = (*it)[5].get<std::uint64_t>(); physics.staleCount = (*it)[6].get<std::uint64_t>(); physics.commandQueueDepth = (*it)[7].get<std::uint64_t>(); physics.shapeCacheBytes = (*it)[8].get<std::uint64_t>(); physics.shapeCacheHits = (*it)[9].get<std::uint64_t>(); physics.shapeCacheMisses = (*it)[10].get<std::uint64_t>(); physics.snapshotBytes = (*it)[11].get<std::uint64_t>(); physics.snapshotHighWaterBytes = (*it)[12].get<std::uint64_t>(); physics.providerAllocations = (*it)[13].get<std::uint64_t>(); physics.providerAllocationHighWater = (*it)[14].get<std::uint64_t>(); physics.prepareMs = ToMilliseconds((*it)[15].get<std::uint64_t>()); physics.commitMs = ToMilliseconds((*it)[16].get<std::uint64_t>()); physics.simulateMs = ToMilliseconds((*it)[17].get<std::uint64_t>()); physics.queryMs = ToMilliseconds((*it)[18].get<std::uint64_t>()); physics.publishMs = ToMilliseconds((*it)[19].get<std::uint64_t>()); out.physics = physics;
		}
		if (const auto it = json.find("g"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 6u) { error = "Profiler v2 has malformed stage data."; return false; } const std::string* name = StringAt(strings, value[1], error); const std::string* workload = StringAt(strings, value[3], error); if (!name || !workload) return false; PerformanceStageRecord stage; stage.domain = static_cast<PerformanceStageDomain>(value[0].get<std::uint8_t>()); stage.stageName = *name; stage.workloadName = *workload; stage.workItemCount = value[4].get<std::uint64_t>(); stage.sampled = value[5].get<bool>(); if (!UnpackDuration(value[2], strings, stage.duration, error) || !ValidatePerformanceStageRecord(stage).valid) return false; out.performanceStages.push_back(std::move(stage));
		}
		if (const auto it = json.find("b"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 6u) { error = "Profiler v2 has malformed protected-boundary data."; return false; } const std::string* name = StringAt(strings, value[0], error); if (!name) return false; ProtectedBoundaryRecord boundary; boundary.boundaryName = *name; if (!UnpackDuration(value[1], strings, boundary.queueDuration, error) || !UnpackDuration(value[2], strings, boundary.ownershipWaitDuration, error) || !UnpackDuration(value[3], strings, boundary.executionDuration, error) || !UnpackDuration(value[4], strings, boundary.flushDuration, error) || !UnpackDuration(value[5], strings, boundary.publicationDuration, error) || !ValidateProtectedBoundaryRecord(boundary).valid) return false; out.protectedBoundaries.push_back(std::move(boundary));
		}
		if (const auto it = json.find("w"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 7u) { error = "Profiler v2 has malformed worker telemetry."; return false; } WorkerTelemetryRecord worker; worker.workerId = value[0].get<std::uint32_t>(); worker.queueDepth = value[1].get<std::uint64_t>(); worker.dispatchCount = value[2].get<std::uint64_t>(); worker.usefulWorkItemCount = value[3].get<std::uint64_t>(); if (!UnpackDuration(value[4], strings, worker.runDuration, error) || !UnpackDuration(value[5], strings, worker.waitDuration, error) || !UnpackDuration(value[6], strings, worker.idleDuration, error) || !ValidateWorkerTelemetryRecord(worker).valid) return false; out.workerTelemetry.push_back(std::move(worker));
		}
		if (const auto it = json.find("q"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 3u) { error = "Profiler v2 has malformed presentation wait data."; return false; } const std::string* detail = StringAt(strings, value[2], error); if (!detail) return false; PresentationWaitRecord wait; wait.kind = static_cast<PresentationWaitKind>(value[0].get<std::uint8_t>()); wait.detail = *detail; if (!UnpackDuration(value[1], strings, wait.duration, error) || !ValidatePresentationWaitRecord(wait).valid) return false; out.presentationWaits.push_back(std::move(wait));
		}
		if (const auto it = json.find("r"); it != json.end()) { if (!it->is_array() || it->size() != 4u) { error = "Profiler v2 has malformed responsiveness data."; return false; } FrameResponsivenessRecord record; record.frameIndex = (*it)[0].get<std::uint64_t>(); record.presentationSequence = (*it)[3].get<std::uint64_t>(); if (!UnpackDuration((*it)[1], strings, record.inputToPresentationDuration, error) || !UnpackDuration((*it)[2], strings, record.frameDuration, error) || !ValidateFrameResponsivenessRecord(record).valid) return false; out.responsiveness = std::move(record); }
		if (const auto it = json.find("v"); it != json.end()) { if (!it->is_array() || it->size() != 8u) { error = "Profiler v2 has malformed physics-volume data."; return false; } PhysicsWorkVolumeRecord volume{(*it)[0].get<std::uint64_t>(), (*it)[1].get<std::uint64_t>(), (*it)[2].get<std::uint64_t>(), (*it)[3].get<std::uint64_t>(), (*it)[4].get<std::uint64_t>(), (*it)[5].get<std::uint64_t>(), (*it)[6].get<std::uint64_t>(), (*it)[7].get<std::uint64_t>()}; if (!ValidatePhysicsWorkVolumeRecord(volume).valid) { error = "Profiler v2 has invalid physics-volume data."; return false; } out.physicsWorkVolume = volume; }
		return true;
	}
	catch (const nlohmann::json::exception& exception)
	{
		error = exception.what();
		return false;
	}
}

bool ReplaceV2Atomically(const std::filesystem::path& temporary, const std::filesystem::path& destination, std::string& error)
{
#ifdef _WIN32
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		error = "Failed to atomically replace profiler capture output (Win32 error " + std::to_string(GetLastError()) + ").";
		return false;
	}
#else
	std::error_code renameError;
	std::filesystem::rename(temporary, destination, renameError);
	if (renameError)
	{
		error = renameError.message();
		return false;
	}
#endif
	return true;
}
}

// The compact codec body is private to the version dispatcher. Public v2
// entry points live in ProfilerCaptureV2.cpp, keeping the versioned module's
// ABI and implementation boundary explicit for callers and build tooling.
bool SaveProfilerCaptureV2Impl(const ProfilerCaptureDocument& inputDocument, const std::filesystem::path& path, std::string& outError)
{
	try
	{
		ProfilerCaptureDocument document = inputDocument;
		document.payloadVersion = kProfilerCaptureV2PayloadVersion;
		V2StringTable strings;
		strings.Intern(document.metadata.terraEngineVersion); strings.Intern(document.metadata.projectName); strings.Intern(document.metadata.projectPathHint);
		strings.Intern(document.metadata.scenePath); strings.Intern(document.metadata.startedAt); strings.Intern(document.metadata.savedAt);
		for (const ProfilerFrameRecord& frame : document.frames) CollectFrameStrings(strings, frame);
		if (strings.Values().size() > kV2MaximumStringCount) { outError = "Profiler v2 string table exceeds its bounded limit."; return false; }

		const std::filesystem::path temporary = path.string() + ".tmp";
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) { outError = "Failed to open profiler v2 temporary output."; return false; }
		const auto stringId = [&strings](const std::string& value) { return strings.Intern(value); };
		nlohmann::json header = {{"format", "TerraEngineProfilerCapture"}, {"payloadVersion", kProfilerCaptureV2PayloadVersion},
			{"metadata", {{"terraEngineVersion", stringId(document.metadata.terraEngineVersion)}, {"projectName", stringId(document.metadata.projectName)}, {"projectPathHint", stringId(document.metadata.projectPathHint)}, {"scenePath", stringId(document.metadata.scenePath)}, {"startedAt", stringId(document.metadata.startedAt)}, {"savedAt", stringId(document.metadata.savedAt)}}},
			{"settings", {{"detailLevel", ToString(document.metadata.settings.detailLevel)}, {"sampleIntervalFrames", std::max(1u, document.metadata.settings.sampleIntervalFrames)}, {"maximumChunkFrames", std::clamp(document.metadata.settings.maximumChunkFrames, 1u, kV2MaximumFramesPerChunk)}, {"maximumChunkBytes", std::max<std::uint64_t>(1024u, document.metadata.settings.maximumChunkBytes)}}},
			{"stringTable", strings.Values()}};
		output << header.dump() << '\n';

		const std::size_t framesPerChunk = std::clamp<std::size_t>(document.metadata.settings.maximumChunkFrames, 1u, kV2MaximumFramesPerChunk);
		const std::size_t maximumChunkBytes = static_cast<std::size_t>(std::max<std::uint64_t>(1024u, document.metadata.settings.maximumChunkBytes));
		std::uint64_t committedFrames = 0u;
		std::uint32_t sequence = 0u;
		std::size_t first = 0u;
		while (first < document.frames.size())
		{
			nlohmann::json frames = nlohmann::json::array();
			std::size_t end = first;
			while (end < document.frames.size() && frames.size() < framesPerChunk)
			{
				nlohmann::json candidate = frames;
				candidate.push_back(PackFrameV2(document.frames[end], document.metadata.settings, strings));
				// A single critical frame may exceed the soft byte target. It stays
				// intact rather than silently losing correctness evidence; all normal
				// chunks are otherwise split before crossing the configured bound.
				if (!frames.empty() && candidate.dump().size() > maximumChunkBytes)
				{
					break;
				}
				frames = std::move(candidate);
				++end;
			}
			nlohmann::json previousPhysicsCounters;
			for (nlohmann::json& frame : frames)
			{
				const auto physics = frame.find("p");
				if (physics == frame.end())
				{
					continue;
				}
				if (previousPhysicsCounters.is_null())
				{
					// A chunk begins with a complete counter baseline so it can be
					// decoded independently after tail recovery.
					frame["cb"] = *physics;
				}
				else
				{
					nlohmann::json deltas = nlohmann::json::array();
					for (std::size_t counter = 0u; counter < physics->size(); ++counter)
					{
						if ((*physics)[counter] != previousPhysicsCounters[counter])
						{
							deltas.push_back(nlohmann::json::array({counter, (*physics)[counter]}));
						}
					}
					frame["cd"] = std::move(deltas);
				}
				previousPhysicsCounters = *physics;
				frame.erase("p");
			}
			const std::string framePayload = frames.dump();
			nlohmann::json chunk = {{"type", "chunk"}, {"sequence", sequence++}, {"committedFrameCount", frames.size()}, {"checksum", Fnv1a32(framePayload)}, {"frames", std::move(frames)}};
			output << chunk.dump() << '\n';
			committedFrames += static_cast<std::uint64_t>(end - first);
			first = end;
		}
		nlohmann::json index = {{"type", "finalIndex"}, {"complete", true}, {"committedFrames", committedFrames}, {"chunkCount", sequence}};
		output << index.dump() << '\n';
		output.flush();
		if (!output) { outError = "Failed to flush profiler v2 capture."; return false; }
		output.close();
		return ReplaceV2Atomically(temporary, path, outError);
	}
	catch (const std::exception& exception)
	{
		outError = exception.what();
		return false;
	}
}

bool LoadProfilerCaptureV2Impl(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) { outError = "Failed to open profiler v2 capture."; return false; }
	// Early v2 fixtures used one logical JSON envelope before the production
	// writer adopted line framing. Keep that reader immutable and additive so
	// fixture upgrades never rewrite historical v2 files in place.
	if (input.peek() == '{')
	{
		const std::string logicalText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		try
		{
			const nlohmann::json logical = nlohmann::json::parse(logicalText);
			if (logical.value("payloadVersion", 0u) == kProfilerCaptureV2PayloadVersion && logical.contains("chunks"))
			{
				if (!logical.contains("stringTable") || !logical["stringTable"].is_array() || logical["stringTable"].empty()) { outError = "Profiler v2 logical fixture has an invalid string table."; return false; }
				ProfilerCaptureDocument document;
				document.payloadVersion = kProfilerCaptureV2PayloadVersion;
				document.sourcePath = path;
				document.metadata.projectName = logical.value("metadata", nlohmann::json::object()).value("captureId", "");
				const std::string level = logical.value("settings", nlohmann::json::object()).value("detailLevel", "detailed");
				document.metadata.settings.detailLevel = level == "sampled" ? ProfilerCaptureDetailLevel::Sampled : level == "aggregate" ? ProfilerCaptureDetailLevel::Aggregate : ProfilerCaptureDetailLevel::Detailed;
				for (const nlohmann::json& chunk : logical["chunks"])
				{
					if (!chunk.is_object() || !chunk.contains("frames") || !chunk["frames"].is_array() || chunk.value("committedFrameCount", 0ull) != chunk["frames"].size()) { outError = "Profiler v2 logical fixture has an invalid committed chunk."; return false; }
					for (const nlohmann::json& frameJson : chunk["frames"])
					{
						ProfilerFrameRecord frame;
						frame.frameIndex = frameJson.value("index", 0ull);
						frame.durationMs = ToMilliseconds(frameJson.value("durationNanoseconds", 0ull));
						frame.complete = frameJson.value("complete", true);
						document.frames.push_back(std::move(frame));
					}
				}
				const nlohmann::json finalIndex = logical.value("finalIndex", nlohmann::json());
				document.complete = finalIndex.is_object() && finalIndex.value("complete", false);
				document.recoveredIncompleteTail = finalIndex.is_null() || (finalIndex.is_object() && finalIndex.value("recoveredIncompleteTail", false));
				document.committedFrameCount = finalIndex.is_object() ? finalIndex.value("committedFrames", static_cast<std::uint64_t>(document.frames.size())) : document.frames.size();
				if (document.committedFrameCount != document.frames.size()) { outError = "Profiler v2 logical fixture final index disagrees with frames."; return false; }
				outDocument = std::move(document);
				return true;
			}
		}
		catch (const nlohmann::json::exception&)
		{
			// The line-framed fallback below reports the canonical recovery error.
		}
		input.clear();
		input.seekg(0);
	}
	std::string line;
	if (!std::getline(input, line)) { outError = "Profiler v2 capture is empty."; return false; }
	try
	{
		const nlohmann::json header = nlohmann::json::parse(line);
		if (header.value("format", std::string{}) != "TerraEngineProfilerCapture" || header.value("payloadVersion", 0u) != kProfilerCaptureV2PayloadVersion) { outError = "File is not a profiler v2 capture."; return false; }
		if (!header.contains("stringTable") || !header["stringTable"].is_array() || header["stringTable"].size() == 0u || header["stringTable"].size() > kV2MaximumStringCount) { outError = "Profiler v2 has an invalid string table."; return false; }
		std::vector<std::string> strings;
		strings.reserve(header["stringTable"].size());
		for (const auto& value : header["stringTable"]) { if (!value.is_string()) { outError = "Profiler v2 string table has a non-string value."; return false; } strings.push_back(value.get<std::string>()); }
		ProfilerCaptureDocument document;
		document.payloadVersion = kProfilerCaptureV2PayloadVersion; document.sourcePath = path; document.complete = false;
		const auto& metadata = header.at("metadata"); std::string stringError;
		const std::string* engine = StringAt(strings, metadata.at("terraEngineVersion"), stringError); const std::string* project = StringAt(strings, metadata.at("projectName"), stringError); const std::string* projectPath = StringAt(strings, metadata.at("projectPathHint"), stringError); const std::string* scene = StringAt(strings, metadata.at("scenePath"), stringError); const std::string* started = StringAt(strings, metadata.at("startedAt"), stringError); const std::string* saved = StringAt(strings, metadata.at("savedAt"), stringError); if (!engine || !project || !projectPath || !scene || !started || !saved) { outError = stringError; return false; }
		document.metadata.terraEngineVersion = *engine; document.metadata.projectName = *project; document.metadata.projectPathHint = *projectPath; document.metadata.scenePath = *scene; document.metadata.startedAt = *started; document.metadata.savedAt = *saved; document.metadata.settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;
		const auto& settings = header.at("settings"); const std::string level = settings.value("detailLevel", "detailed"); document.metadata.settings.detailLevel = level == "sampled" ? ProfilerCaptureDetailLevel::Sampled : level == "aggregate" ? ProfilerCaptureDetailLevel::Aggregate : ProfilerCaptureDetailLevel::Detailed; document.metadata.settings.sampleIntervalFrames = std::max(1u, settings.value("sampleIntervalFrames", 1u));
		std::uint64_t expectedSequence = 0u; bool finalIndexSeen = false;
		while (std::getline(input, line))
		{
			if (line.empty()) continue;
			nlohmann::json item;
			try { item = nlohmann::json::parse(line); }
			catch (const nlohmann::json::exception&) { document.recoveredIncompleteTail = true; break; }
			if (item.value("type", std::string{}) == "finalIndex") { finalIndexSeen = true; document.complete = item.value("complete", false); document.committedFrameCount = item.value("committedFrames", static_cast<std::uint64_t>(document.frames.size())); break; }
			if (item.value("type", std::string{}) != "chunk" || item.value("sequence", std::numeric_limits<std::uint64_t>::max()) != expectedSequence) { outError = "Profiler v2 has a malformed or out-of-order chunk."; return false; }
			const auto frames = item.value("frames", nlohmann::json::array()); const std::uint64_t committed = item.value("committedFrameCount", 0ull); if (!frames.is_array() || frames.size() != committed || frames.size() > kV2MaximumFramesPerChunk) { outError = "Profiler v2 has an invalid committed-frame count."; return false; }
			if (item.value("checksum", 0u) != Fnv1a32(frames.dump())) { outError = "Profiler v2 chunk checksum mismatch."; return false; }
			nlohmann::json previousPhysicsCounters;
			for (const auto& sourceFrameJson : frames)
			{
				nlohmann::json frameJson = sourceFrameJson;
				bool carriesPhysicsCounters = false;
				if (const auto baseline = frameJson.find("cb"); baseline != frameJson.end())
				{
					if (!baseline->is_array() || baseline->size() != 20u) { outError = "Profiler v2 has an invalid counter baseline."; return false; }
					previousPhysicsCounters = *baseline;
					carriesPhysicsCounters = true;
				}
				else if (const auto deltas = frameJson.find("cd"); deltas != frameJson.end())
				{
					if (previousPhysicsCounters.is_null() || !deltas->is_array()) { outError = "Profiler v2 counter delta has no baseline."; return false; }
					for (const nlohmann::json& delta : *deltas) { if (!delta.is_array() || delta.size() != 2u || !delta[0].is_number_unsigned() || delta[0].get<std::size_t>() >= previousPhysicsCounters.size()) { outError = "Profiler v2 has an invalid counter delta."; return false; } previousPhysicsCounters[delta[0].get<std::size_t>()] = delta[1]; }
					carriesPhysicsCounters = true;
				}
				if (carriesPhysicsCounters) frameJson["p"] = previousPhysicsCounters;
				ProfilerFrameRecord frame; if (!UnpackFrameV2(frameJson, strings, frame, outError)) return false; document.frames.push_back(std::move(frame));
			}
			++expectedSequence;
		}
		if (!finalIndexSeen) { document.recoveredIncompleteTail = true; document.complete = false; }
		if (document.committedFrameCount == 0u) document.committedFrameCount = document.frames.size();
		if (document.committedFrameCount != document.frames.size()) { outError = "Profiler v2 final index disagrees with committed frames."; return false; }
		outDocument = std::move(document);
		return true;
	}
	catch (const std::exception& exception) { outError = exception.what(); return false; }
}

bool TryReadProfilerCaptureV2MetadataImpl(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
{
	ProfilerCaptureDocument document;
	std::string error;
	if (!LoadProfilerCaptureV2(path, document, error)) { outArtifact = {}; outArtifact.absolutePath = path; outArtifact.fileName = path.filename().string(); outArtifact.supported = false; outArtifact.status = error; return false; }
	outArtifact = {}; outArtifact.absolutePath = path; outArtifact.fileName = path.filename().string(); outArtifact.metadata = document.metadata; outArtifact.payloadVersion = document.payloadVersion; outArtifact.frameCount = document.frames.size(); outArtifact.committedFrameCount = document.committedFrameCount; outArtifact.complete = document.complete; outArtifact.recoveredIncompleteTail = document.recoveredIncompleteTail; outArtifact.supported = true; outArtifact.status = document.recoveredIncompleteTail ? "Recovered incomplete tail" : "Ready (metadata)"; outArtifact.moduleIds = {ProfilerModuleId::CpuEngine, ProfilerModuleId::TaskExecution}; return true;
}
}
