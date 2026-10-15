// Read-only preparation-scope decoder. Use the installed executable with
// --journal <absolute .teprof> --offset 0 --count 360 after durable capture stop.
// This standalone tool shares the authoritative codec, never runtime DLL APIs.
#include "Profiling/ProfilerJournalV1.h"
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
	using namespace TerraEngine::Profiling;
	constexpr std::array<std::string_view, 3> kNames{
		"RenderSystem: Protected Publication Capture",
		"RenderSystem: Geometry Route Preparation",
		"RenderSystem: Virtual Geometry Preparation"};
	constexpr double kRoundingAllowanceMs = 0.000002;
	void Require(bool condition, const std::string& reason)
	{
		if (!condition) throw std::runtime_error(reason);
	}
	void Complete(const ProfilerJournalV1OpenResult& metadata)
	{
		Require(metadata.complete && !metadata.recoveredIncompleteTail,
			"Require a durable complete journal without recovered tail.");
		const auto& settings = metadata.metadata.settings;
		// JournalV1 persists detail/cadence but not maxFrames or module-enabled
		// flags. The capture receipt proves admitted maxFrames through start.bounds;
		// required CpuEngine samples and engine_cpu prove the actual recorded data.
		Require(settings.detailLevel == ProfilerCaptureDetailLevel::Detailed && settings.sampleIntervalFrames == 1,
			"Require persisted Detailed/1 capture settings.");
	}
	nlohmann::json Decode(const ProfilerFrameRecord& frame)
	{
		Require(frame.complete && std::isfinite(frame.durationMs) && frame.durationMs > 0
			&& std::isfinite(frame.frameStartMs) && frame.frameStartMs >= 0,
			"Incomplete or invalid frame.");
		const ProfilerTimingStreamSample* engineLane = nullptr;
		for (const auto& lane : frame.timingStreams)
		{
			if (lane.id != "engine_cpu") continue;
			Require(engineLane == nullptr, "Duplicate engine CPU timing lane.");
			Require(lane.scope == ProfilerTimingStreamScope::EngineCpu
				&& lane.source == ProfilerTimingStreamSource::ProfilerClock
				&& lane.status == ProfilerTimingStreamStatus::Valid
				&& std::isfinite(lane.milliseconds) && lane.milliseconds == frame.durationMs,
				"Invalid engine CPU clock provenance.");
			engineLane = &lane;
		}
		Require(engineLane != nullptr, "Missing engine CPU clock provenance.");
		std::array<const ProfilerSample*, 3> phases{};
		for (const auto& sample : frame.samples)
		{
			for (std::size_t index = 0; index < kNames.size(); ++index)
			{
				if (sample.name != kNames[index]) continue;
				Require(phases[index] == nullptr, "Duplicate required phase name.");
				Require(sample.moduleId == ProfilerModuleId::CpuEngine && sample.complete
					&& sample.occurrenceCount == 1 && std::isfinite(sample.startMs) && sample.startMs >= 0
					&& std::isfinite(sample.durationMs) && sample.durationMs > 0,
					"Invalid required preparation phase.");
				Require(sample.startMs <= frame.durationMs + kRoundingAllowanceMs
					&& sample.startMs + sample.durationMs <= frame.durationMs + kRoundingAllowanceMs,
					"Required preparation phase exceeds frame bounds.");
				phases[index] = &sample;
			}
		}
		nlohmann::json samples = nlohmann::json::array();
		double preparationMs = 0;
		for (std::size_t index = 0; index < phases.size(); ++index)
		{
			Require(phases[index] != nullptr, "Missing required preparation phase.");
			const auto& sample = *phases[index];
			for (std::size_t earlier = 0; earlier < index; ++earlier)
				Require(sample.sampleId != phases[earlier]->sampleId, "Required phases share a sample ID.");
			if (index > 0)
				Require(sample.startMs + kRoundingAllowanceMs >= phases[index - 1]->startMs + phases[index - 1]->durationMs,
					"Required preparation phases are reordered or overlapping.");
			// Inclusive phase duration already contains nested work. Child samples
			// are intentionally excluded, so worker or nested time is never added twice.
			preparationMs += sample.durationMs;
			samples.push_back({{"sampleId", sample.sampleId}, {"parentSampleId", sample.parentSampleId},
				{"moduleId", "CpuEngine"}, {"name", sample.name}, {"threadLabel", sample.threadLabel},
				{"taskLabel", sample.taskLabel}, {"startMs", sample.startMs}, {"durationMs", sample.durationMs},
				{"depth", sample.depth}, {"occurrenceCount", sample.occurrenceCount}, {"complete", sample.complete},
				{"retainInAggregate", sample.retainInAggregate}});
		}
		Require(std::isfinite(preparationMs) && preparationMs > 0, "Invalid phase sum.");
		return {{"frameIndex", frame.frameIndex}, {"frameStartMs", frame.frameStartMs},
			{"durationMs", frame.durationMs}, {"complete", true}, {"preparationMs", preparationMs},
			{"phaseSamples", std::move(samples)}, {"engineClockLane", {{"id", engineLane->id},
			{"label", engineLane->label}, {"scope", "EngineCpu"}, {"source", "ProfilerClock"},
			{"status", "Valid"}, {"milliseconds", engineLane->milliseconds}, {"statusReason", engineLane->statusReason}}}};
	}
}

int main(int argc, char** argv)
{
	try
	{
		std::filesystem::path journal;
		bool hasJournal = false, hasOffset = false, hasCount = false;
		for (int index = 1; index < argc; index += 2)
		{
			Require(index + 1 < argc, "Every option requires a value.");
			const std::string_view option = argv[index], value = argv[index + 1];
			if (option == "--journal" && !hasJournal)
			{
				journal = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
				hasJournal = true;
			}
			else if (option == "--offset" && !hasOffset && value == "0") hasOffset = true;
			else if (option == "--count" && !hasCount && value == "360") hasCount = true;
			else throw std::runtime_error("Unknown, duplicate or noncanonical option; window must be 0/360.");
		}
		Require(hasJournal && hasOffset && hasCount && journal.is_absolute(), "Require absolute --journal and --offset 0 --count 360.");
		std::string error;
		ProfilerJournalV1OpenResult metadata;
		std::uint64_t total = 0;
		Require(ReadProfilerJournalV1IndexPage(journal, 0, 360, metadata, total, error) == ProfilerJournalV1ResultCode::Ok, error);
		Complete(metadata);
		Require(total >= 360 && metadata.index.size() == 360, "Journal lacks the fixed 360-frame window.");
		nlohmann::json frames = nlohmann::json::array();
		std::uint64_t previous = 0;
		for (const auto& entry : metadata.index)
		{
			Require(frames.empty() || (previous != std::numeric_limits<std::uint64_t>::max() && entry.frameIndex == previous + 1),
				"Gapped or duplicate selected frame indices.");
			ProfilerFrameRecord frame;
			Require(ReadProfilerJournalV1SelectedFrame(journal, entry.frameIndex, frame, error) == ProfilerJournalV1ResultCode::Ok, error);
			Require(entry.complete && frame.frameIndex == entry.frameIndex && entry.durationMs == frame.durationMs,
				"Incomplete or mismatched selected frame.");
			frames.push_back(Decode(frame));
			previous = frame.frameIndex;
		}
		ProfilerJournalV1OpenResult finalMetadata;
		std::uint64_t finalTotal = 0;
		Require(ReadProfilerJournalV1IndexPage(journal, 0, 0, finalMetadata, finalTotal, error) == ProfilerJournalV1ResultCode::Ok, error);
		Complete(finalMetadata);
		Require(finalTotal == total, "Journal changed during decoding.");
		// Publish only after every selected frame passed. The capture adapter
		// independently hashes original/copied journals before and after this read.
		std::cout << nlohmann::json{{"formatIdentifier", "profilerJournalV1"}, {"formatVersion", 1}, {"complete", true},
			{"metric", "rendererPreparationScopeSumMs"}, {"window", {{"offset", 0}, {"count", 360}, {"warmupFrames", 60}, {"measuredFrames", 300}}},
			{"captureSettings", {{"captureLevel", "detailed"}, {"sampleIntervalFrames", 1}, {"proof", "journalMetadata"}}},
			{"frames", std::move(frames)}}.dump() << '\n';
		return std::cout.good() ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Runtime memory safety timing inspection rejected: " << error.what() << '\n';
		return 1;
	}
}
