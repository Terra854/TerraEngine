// CPU-only decoder fixtures, written by the real JournalV1 writer.
// Usage: --work-root <fresh absolute directory>. Link ProfilerCapture.cpp,
// ProfilerJournalV1.cpp and the bundled Zstd/JSON dependencies, as for inspector.
#include "Profiling/ProfilerJournalV1.h"
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace TerraEngine::Profiling;
	void Require(bool value, const std::string& error)
	{
		if (!value) throw std::runtime_error(error);
	}
	void WriteJournal(const std::filesystem::path& path, bool gap, bool zero,
		bool incompleteFrame, bool completeTerminal, std::string_view laneVariant = {}, bool partialStop = false)
	{
		ProfilerCaptureMetadata metadata;
		metadata.terraEngineVersion = "renderer-efficiency-cpu-fixture";
		metadata.projectName = "Profiler journal decoder fixtures";
		metadata.scenePath = "Scenes/Journal.tescene";
		metadata.startedAt = "deterministic-cpu-fixture";
		metadata.settings.maximumChunkFrames = partialStop ? 361 : 360;
		metadata.settings.maximumChunkBytes = 16u * 1024u * 1024u;
		std::string error;
		ProfilerJournalV1Writer writer;
		Require(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok, error);
		std::vector<ProfilerSealedFrame> frames(partialStop ? 361 : 360);
		for (std::size_t ordinal = 0; ordinal < frames.size(); ++ordinal)
		{
			auto& sealed = frames[ordinal];
			sealed.frame.frameIndex = 1000 + ordinal + (gap && ordinal >= 180 ? 1 : 0);
			sealed.frame.frameStartMs = static_cast<double>(ordinal) * 10.0;
			sealed.frame.durationMs = zero && ordinal == 180 ? 0.0 : 10.0;
			sealed.frame.complete = !(incompleteFrame && ordinal == 180) && !(partialStop && ordinal == 360);
			ProfilerTimingStreamSample stream{"engine_cpu", "Engine CPU",
				ProfilerTimingStreamScope::EngineCpu, ProfilerTimingStreamSource::ProfilerClock,
				ProfilerTimingStreamStatus::Valid, sealed.frame.durationMs, {}};
			// Only one middle frame differs, so rejection cannot be attributed to
			// a missing terminal, short capture, or corrupted binary envelope.
			if (ordinal == 180)
			{
				if (laneVariant == "wrong-scope") stream.scope = ProfilerTimingStreamScope::ViewportCpu;
				if (laneVariant == "wrong-source") stream.source = ProfilerTimingStreamSource::RenderStats;
				if (laneVariant == "unavailable-lane") stream.status = ProfilerTimingStreamStatus::Unavailable;
				if (laneVariant == "mismatched-lane") stream.milliseconds = 11.0;
			}
			if (!(ordinal == 180 && laneVariant == "missing-lane")) sealed.frame.timingStreams.push_back(stream);
			if (ordinal == 180 && laneVariant == "duplicate-lane") sealed.frame.timingStreams.push_back(stream);
			sealed.ownedByteCount = EstimateProfilerFrameOwnedBytes(sealed.frame);
		}
		Require(writer.AppendCommittedSegment(frames, error) == ProfilerJournalV1ResultCode::Ok, error);
		Require(writer.Finalize(completeTerminal, error) == ProfilerJournalV1ResultCode::Ok, error);
	}
	std::string Read(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "Cannot read fixture journal.");
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}
	void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
	{
		Require(!std::filesystem::exists(path), "Fixture output already exists.");
		std::ofstream output(path, std::ios::binary);
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.close();
		Require(output.good(), "Cannot persist fixture bytes.");
	}
}

int main(int argc, char** argv)
{
	try
	{
		Require(argc == 3 && std::string_view(argv[1]) == "--work-root", "Require --work-root <fresh absolute directory>.");
		const std::string_view argument = argv[2];
		const std::filesystem::path root(std::u8string(reinterpret_cast<const char8_t*>(argument.data()), argument.size()));
		Require(root.is_absolute() && !std::filesystem::exists(root), "Work root must be fresh and absolute.");
		Require(std::filesystem::create_directories(root), "Cannot create fresh work root.");
		nlohmann::json manifest = nlohmann::json::array();
		auto add = [&](const char* name, bool expected) {
			const auto path = root / (std::string(name) + ".teprof");
			manifest.push_back({{"name", name}, {"path", path.generic_string()}, {"expectedInspectorExitZero", expected}});
			return path;
		};
		const auto valid = add("valid", true);
		WriteJournal(valid, false, false, false, true);
		WriteJournal(add("gap", false), true, false, false, true);
		WriteJournal(add("zero-duration", false), false, true, false, true);
		WriteJournal(add("incomplete-frame", false), false, false, true, true);
		WriteJournal(add("incomplete-terminal", false), false, false, false, false);
		// This durable capture has a complete 360-frame prefix and an honest
		// partial stop frame. The prefix passes; a shifted window must reject.
		WriteJournal(add("partial-stop-outside-window", true), false, false, false, true, {}, true);
		for (const char* variant : {"missing-lane", "wrong-scope", "wrong-source",
			"unavailable-lane", "mismatched-lane", "duplicate-lane"})
		{
			WriteJournal(add(variant, false), false, false, false, true, variant);
		}
		std::string bytes = Read(valid);
		Require(bytes.size() > 1, "Valid journal unexpectedly empty.");
		WriteBytes(add("truncated-terminal", false), bytes.substr(0, bytes.size() - 1));
		// Locate the actual committed segment through the authoritative index.
		// Its V1 header is 57 bytes; flip one compressed payload byte, retaining
		// the writer's original commit CRC so the decoder must reject corruption.
		ProfilerJournalV1OpenResult index;
		std::uint64_t total = 0;
		std::string error;
		Require(ReadProfilerJournalV1IndexPage(valid, 0, 1, index, total, error) == ProfilerJournalV1ResultCode::Ok
			&& index.index.size() == 1 && total == 360, "Cannot locate fixture committed payload: " + error);
		const auto position = index.index.front().segmentOffset + 57;
		Require(position < bytes.size() && bytes.compare(static_cast<std::size_t>(index.index.front().segmentOffset), 4, "SEG1") == 0,
			"Unexpected committed segment layout.");
		bytes[static_cast<std::size_t>(position)] ^= 1;
		WriteBytes(add("corrupted-payload", false), bytes);
		std::cout << manifest.dump() << '\n';
		return std::cout.good() ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Profiler journal fixture rejected: " << error.what() << '\n';
		return 1;
	}
}
