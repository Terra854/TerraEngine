// Genuine binary journals for decoder protocol checks, never performance proof.
// Usage: --work-root <fresh absolute directory>. No graphics/runtime DLL needed.
#include "Profiling/ProfilerJournalV1.h"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
	using namespace TerraEngine::Profiling;
	void Require(bool valid, const std::string& error)
	{
		if (!valid) throw std::runtime_error(error);
	}
	void WriteJournal(const std::filesystem::path& path, std::string_view variant)
	{
		ProfilerCaptureMetadata metadata;
		metadata.terraEngineVersion = "runtime-memory-safety-cpu-protocol-fixture";
		metadata.projectName = "Decoder protocol only";
		metadata.scenePath = "Scenes/Protocol.tescene";
		metadata.startedAt = "deterministic-cpu-fixture";
		metadata.settings.maxFrames = 4096;
		metadata.settings.sampleIntervalFrames = 1;
		metadata.settings.maximumChunkFrames = 361;
		metadata.settings.maximumChunkBytes = 16u * 1024u * 1024u;
		if (variant == "wrong-capture-settings") metadata.settings.sampleIntervalFrames = 4;
		if (variant == "wrong-capture-detail") metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
		std::string error;
		ProfilerJournalV1Writer writer;
		Require(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok, error);
		std::vector<ProfilerSealedFrame> frames(variant == "valid-partial-stop" ? 361 : 360);
		for (std::size_t ordinal = 0; ordinal < frames.size(); ++ordinal)
		{
			auto& frame = frames[ordinal].frame;
			frame.frameIndex = 1000 + ordinal + (variant == "gapped-frames" && ordinal >= 180 ? 1 : 0);
			frame.frameStartMs = static_cast<double>(ordinal) * 10;
			frame.durationMs = 10;
			frame.complete = !(variant == "incomplete-frame" && ordinal == 180) && ordinal != 360;
			frame.timingStreams.push_back({"engine_cpu", "Engine CPU", ProfilerTimingStreamScope::EngineCpu,
				ProfilerTimingStreamSource::ProfilerClock, ProfilerTimingStreamStatus::Valid, 10, {}});
			constexpr std::array<const char*, 3> names{"RenderSystem: Protected Publication Capture",
				"RenderSystem: Geometry Route Preparation", "RenderSystem: Virtual Geometry Preparation"};
			constexpr std::array<double, 3> starts{0.1, 1.0, 2.0}, durations{0.5, 0.75, 1.25};
			for (std::size_t phase = 0; phase < 3; ++phase)
			{
				ProfilerSample sample;
				sample.sampleId = 10 + phase;
				sample.parentSampleId = 1;
				sample.name = names[phase];
				sample.threadLabel = phase == 0 ? "Main" : "Worker";
				sample.taskLabel = "Protocol phase";
				sample.startMs = starts[phase];
				sample.durationMs = durations[phase];
				sample.depth = 1;
				frame.samples.push_back(std::move(sample));
			}
			if (variant == "valid-with-child")
			{
				ProfilerSample child;
				child.sampleId = 99; child.parentSampleId = 11; child.depth = 2;
				child.name = "Nested worker preparation"; child.startMs = 1.1; child.durationMs = 0.1;
				frame.samples.push_back(std::move(child));
			}
			if (ordinal == 180)
			{
				if (variant == "missing-phase") frame.samples.erase(frame.samples.begin() + 1);
				if (variant == "duplicate-phase") frame.samples.push_back(frame.samples[1]);
				if (variant == "wrong-module") frame.samples[1].moduleId = ProfilerModuleId::TaskExecution;
				if (variant == "incomplete-phase") frame.samples[1].complete = false;
				// The real codec maps NaN duration to zero. This fixture tests that
				// the decoded invalid result is rejected; it cannot preserve raw NaN.
				if (variant == "nonfinite-duration") frame.samples[1].durationMs = std::numeric_limits<double>::quiet_NaN();
				if (variant == "zero-duration") frame.samples[1].durationMs = 0;
				if (variant == "aggregate-occurrence") frame.samples[1].occurrenceCount = 2;
				if (variant == "duplicate-sample-id") frame.samples[1].sampleId = 10;
				if (variant == "misordered-phases") std::swap(frame.samples[0].startMs, frame.samples[2].startMs);
				if (variant == "overlapping-phases") frame.samples[1].startMs = 0.5;
				if (variant == "out-of-frame") frame.samples[2].durationMs = 9;
				if (variant == "rounding-boundary")
				{
					frame.samples[0].durationMs = 0.900001;
					frame.samples[2].durationMs = 8.000001;
				}
				if (variant == "missing-lane") frame.timingStreams.clear();
				if (variant == "wrong-lane-source") frame.timingStreams[0].source = ProfilerTimingStreamSource::RenderStats;
				if (variant == "duplicate-lane") frame.timingStreams.push_back(frame.timingStreams[0]);
			}
			frames[ordinal].ownedByteCount = EstimateProfilerFrameOwnedBytes(frame);
		}
		Require(writer.AppendCommittedSegment(frames, error) == ProfilerJournalV1ResultCode::Ok, error);
		Require(writer.Finalize(variant != "incomplete-terminal", error) == ProfilerJournalV1ResultCode::Ok, error);
	}
	std::string Read(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "Cannot read fixture journal.");
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}
	void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
	{
		Require(!std::filesystem::exists(path), "Fixture path already exists.");
		std::ofstream output(path, std::ios::binary);
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.close();
		Require(output.good(), "Cannot write fixture bytes.");
	}
}

int main(int argc, char** argv)
{
	try
	{
		Require(argc == 3 && std::string_view(argv[1]) == "--work-root", "Require --work-root <fresh absolute directory>.");
		const std::string_view text = argv[2];
		const std::filesystem::path root(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
		Require(root.is_absolute() && !std::filesystem::exists(root), "Work root must be fresh and absolute.");
		Require(std::filesystem::create_directories(root), "Cannot create fixture root.");
		nlohmann::json manifest = nlohmann::json::array();
		auto add = [&](const char* name, bool expected) {
			const auto path = root / (std::string(name) + ".teprof");
			manifest.push_back({{"name", name}, {"path", path.generic_string()}, {"expectedInspectorExitZero", expected}});
			return path;
		};
		const auto valid = add("valid", true);
		WriteJournal(valid, "valid");
		for (const char* variant : {"valid-with-child", "rounding-boundary", "valid-partial-stop"}) WriteJournal(add(variant, true), variant);
		for (const char* variant : {"missing-phase", "duplicate-phase", "wrong-module", "incomplete-phase",
			"nonfinite-duration", "zero-duration", "aggregate-occurrence", "duplicate-sample-id", "misordered-phases",
			"overlapping-phases", "out-of-frame", "missing-lane", "wrong-lane-source", "duplicate-lane",
			"gapped-frames", "incomplete-frame", "incomplete-terminal", "wrong-capture-settings", "wrong-capture-detail"}) WriteJournal(add(variant, false), variant);
		auto bytes = Read(valid);
		Require(!bytes.empty(), "Valid journal is empty.");
		WriteBytes(add("truncated-terminal", false), bytes.substr(0, bytes.size() - 1));
		ProfilerJournalV1OpenResult index;
		std::uint64_t total = 0;
		std::string error;
		Require(ReadProfilerJournalV1IndexPage(valid, 0, 1, index, total, error) == ProfilerJournalV1ResultCode::Ok
			&& index.index.size() == 1, error);
		const auto position = index.index.front().segmentOffset + 57;
		Require(position < bytes.size() && bytes.compare(static_cast<std::size_t>(index.index.front().segmentOffset), 4, "SEG1") == 0,
			"Unexpected journal segment framing.");
		bytes[static_cast<std::size_t>(position)] ^= 1;
		WriteBytes(add("corrupted-payload", false), bytes);
		std::cout << manifest.dump() << '\n';
		return std::cout.good() ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Runtime memory safety journal fixture rejected: " << error.what() << '\n';
		return 1;
	}
}
