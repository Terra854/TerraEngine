// Read-only baseline timing decoder. Link the authoritative profiler journal
// and frame codec sources with the repository's JSON and Zstd dependencies.
// Usage: --journal <absolute path> --offset <frame ordinal> --count <frames>.
#include "Profiling/ProfilerJournalV1.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
	using namespace TerraEngine::Profiling;
	void Require(bool condition, const std::string& message)
	{
		if (!condition) throw std::runtime_error(message);
	}
	std::uint64_t Unsigned(std::string_view text)
	{
		// Reject signs, whitespace and alternate spellings rather than guessing.
		Require(!text.empty() && (text.size() == 1 || text.front() != '0'), "Invalid unsigned argument.");
		std::uint64_t value = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid unsigned argument.");
		return value;
	}
	void Complete(const ProfilerJournalV1OpenResult& result)
	{
		Require(result.complete && !result.recoveredIncompleteTail
			&& result.summary.frameCount > 0,
			"Journal requires a complete terminal and no recovered tail.");
		// Stopping capture may seal a partial last frame. Only the requested
		// timing window must be complete; the terminal still proves durable close.
	}
}

int main(int argc, char** argv)
{
	try
	{
		std::filesystem::path journal;
		std::uint64_t offset = 0, count = 0;
		bool hasJournal = false, hasOffset = false, hasCount = false;
		for (int i = 1; i < argc; i += 2)
		{
			Require(i + 1 < argc, "Every option needs a value.");
			const std::string_view option = argv[i];
			if (option == "--journal" && !hasJournal)
			{
				const std::string_view pathText = argv[i + 1];
				journal = std::filesystem::path(std::u8string(
					reinterpret_cast<const char8_t*>(pathText.data()), pathText.size()));
				hasJournal = true;
			}
			else if (option == "--offset" && !hasOffset) { offset = Unsigned(argv[i + 1]); hasOffset = true; }
			else if (option == "--count" && !hasCount) { count = Unsigned(argv[i + 1]); hasCount = true; }
			else throw std::runtime_error("Unknown or duplicate option.");
		}
		Require(hasJournal && hasOffset && hasCount && journal.is_absolute() && count > 0,
			"Require absolute --journal, --offset and positive --count.");
		std::string error;
		ProfilerJournalV1OpenResult metadata;
		std::uint64_t total = 0;
		Require(ReadProfilerJournalV1IndexPage(journal, 0, 0, metadata, total, error) == ProfilerJournalV1ResultCode::Ok, error);
		Complete(metadata);
		Require(offset < total && count <= total - offset, "Requested ordinal range is outside the journal.");
		nlohmann::json frames = nlohmann::json::array();
		std::uint64_t previous = 0;
		bool havePrevious = false;
		// Decode one selected frame at a time; never retain detailed sample trees.
		// JSON remains private until every requested committed frame is verified.
		for (std::uint64_t consumed = 0; consumed < count;)
		{
			const auto pageCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(512, count - consumed));
			ProfilerJournalV1OpenResult page;
			std::uint64_t currentTotal = 0;
			Require(ReadProfilerJournalV1IndexPage(journal, offset + consumed, pageCount, page, currentTotal, error) == ProfilerJournalV1ResultCode::Ok, error);
			Complete(page);
			Require(currentTotal == total && page.index.size() == pageCount, "Journal range changed or is incomplete.");
			for (const auto& entry : page.index)
			{
				Require(!havePrevious || (previous != std::numeric_limits<std::uint64_t>::max() && entry.frameIndex == previous + 1), "Duplicate or gapped frame indices.");
				ProfilerFrameRecord frame;
				Require(ReadProfilerJournalV1SelectedFrame(journal, entry.frameIndex, frame, error) == ProfilerJournalV1ResultCode::Ok, error);
				Require(entry.complete && frame.complete && frame.frameIndex == entry.frameIndex
					&& std::isfinite(frame.durationMs) && frame.durationMs > 0
					&& frame.durationMs == entry.durationMs, "Incomplete or invalid decoded frame timing.");
				// The scalar duration is not a clock identity. Require the producer's
				// independently encoded engine lane before grading it as engine CPU.
				std::size_t engineLanes = 0;
				for (const auto& stream : frame.timingStreams)
				{
					if (stream.id != "engine_cpu") continue;
					++engineLanes;
					Require(stream.scope == ProfilerTimingStreamScope::EngineCpu
						&& stream.source == ProfilerTimingStreamSource::ProfilerClock
						&& stream.status == ProfilerTimingStreamStatus::Valid
						&& std::isfinite(stream.milliseconds) && stream.milliseconds > 0
						&& stream.milliseconds == frame.durationMs, "Invalid engine CPU timing lane.");
				}
				Require(engineLanes == 1, "Require exactly one engine CPU timing lane.");
				frames.push_back({{"frameIndex", frame.frameIndex}, {"durationMs", frame.durationMs}, {"complete", true}});
				previous = frame.frameIndex; havePrevious = true;
			}
			consumed += pageCount;
		}
		ProfilerJournalV1OpenResult finalMetadata;
		std::uint64_t finalTotal = 0;
		Require(ReadProfilerJournalV1IndexPage(journal, 0, 0, finalMetadata, finalTotal, error) == ProfilerJournalV1ResultCode::Ok, error);
		Complete(finalMetadata);
		Require(finalTotal == total && frames.size() == count, "Journal changed during inspection.");
		std::cout << nlohmann::json{{"formatIdentifier", "profilerJournalV1"}, {"formatVersion", 1},
			{"complete", true}, {"frames", std::move(frames)}}.dump() << '\n';
		return std::cout.good() ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Profiler journal inspection rejected: " << error.what() << '\n';
		return 1;
	}
}
