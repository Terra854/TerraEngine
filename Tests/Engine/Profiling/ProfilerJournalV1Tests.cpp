#include "TestHarness.h"

#include "Profiling/ProfilerJournalV1.h"
#include "ProfilerTestSupport.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
namespace
{
Profiling::ProfilerSealedFrame MakeSealedFrame(
	std::uint64_t index,
	std::string label = "Frame",
	double frameStartMs = 0.0,
	double engineCpuMs = 16.0)
{
	Profiling::ProfilerSealedFrame sealed;
	sealed.frame.frameIndex = index;
	sealed.frame.frameStartMs = frameStartMs;
	sealed.frame.durationMs = engineCpuMs;
	sealed.frame.complete = true;
	sealed.frame.timingStreams.push_back({
		"engine_cpu", "Engine CPU", Profiling::ProfilerTimingStreamScope::EngineCpu,
		Profiling::ProfilerTimingStreamSource::ProfilerClock,
		Profiling::ProfilerTimingStreamStatus::Valid, engineCpuMs, {}});
	sealed.frame.samples.push_back({1u, 0u, Profiling::ProfilerModuleId::CpuEngine,
		std::move(label), "main", {}, 0.0, engineCpuMs, 0u, 1u, true, true});
	sealed.ownedByteCount = Profiling::EstimateProfilerFrameOwnedBytes(sealed.frame);
	return sealed;
}

Profiling::ProfilerCaptureMetadata MakeMetadata()
{
	Profiling::ProfilerCaptureMetadata metadata;
	metadata.terraEngineVersion = "unit-test";
	metadata.projectName = "Journal";
	metadata.scenePath = "Scenes/Journal.tescene";
	metadata.startedAt = "2026-08-10";
	metadata.settings.maximumChunkFrames = 4u;
	metadata.settings.maximumChunkBytes = 1024u * 1024u;
	return metadata;
}

std::string ReadBytes(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

void RegisterProfilerJournalV1Tests(TestRegistry& registry)
{
	registry.Add("Engine.ProfilerJournalV1.CommitsPayloadAndFooterInOneDurableWrite", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1SingleFlush");
		auto sink = std::make_shared<ProfilerTestSupport::ControlledJournalSink>();
		ProfilerJournalV1Writer writer(sink);
		std::string error;
		context.Expect(writer.Open(workspace.Path("single-flush.teprof"), MakeMetadata(), error)
			== ProfilerJournalV1ResultCode::Ok,
			"The controlled V1 journal should write its header: " + error);

		const ProfilerSealedFrame frame = MakeSealedFrame(1u);
		context.Expect(writer.AppendCommittedSegment(std::span(&frame, 1u), error)
			== ProfilerJournalV1ResultCode::Ok,
			"A committed V1 segment should reach the sink: " + error);
		// Header is write one.  Combining SEG1 and its COM1 footer makes the
		// durable commit write two, avoiding a second worker flush per segment.
		context.ExpectEqual(2ULL, static_cast<unsigned long long>(sink->GetWriteCount()),
			"A segment payload and its commit footer must share one durable sink write.");
		context.Expect(writer.GetLastStoredSegmentBytes() > 0u,
			"The combined durable write must still contribute stored-byte telemetry.");
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"Finalization should retain the sparse checkpoint and terminal writes: " + error);
	});

	registry.Add("Engine.ProfilerJournalV1.FreshIdentityRoundTripsAndRejectsHistoricalBytes", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Identity");
		const std::filesystem::path path = workspace.Path("fresh.teprof");
		std::string error;
		ProfilerJournalV1Writer writer;
		context.Expect(writer.Open(path, MakeMetadata(), error) == ProfilerJournalV1ResultCode::Ok,
			"A V1 writer should create the fresh header: " + error);
		const ProfilerSealedFrame frame = MakeSealedFrame(7u);
		context.Expect(writer.AppendCommittedSegment(std::span(&frame, 1u), error) == ProfilerJournalV1ResultCode::Ok,
			"A valid V1 frame should commit: " + error);
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"A valid V1 journal should finalize: " + error);

		const std::string bytes = ReadBytes(path);
		context.Expect(bytes.starts_with("TEPROFJOURNALV1"),
			"The fresh V1 header must use its new unambiguous magic.");
		ProfilerCaptureDocument loaded;
		ProfilerJournalV1OpenResult result;
		const ProfilerJournalV1ResultCode openResult = OpenProfilerJournalV1(
			path, loaded, result, error);
		context.Expect(openResult == ProfilerJournalV1ResultCode::Ok,
			"The V1 reader should accept its own committed journal: " + error);
		context.ExpectEqual(1ULL, static_cast<unsigned long long>(loaded.frames.size()),
			"The V1 journal must retain its committed frame.");
		context.Expect(!loaded.frames.empty() && !loaded.frames.front().samples.empty()
			&& loaded.frames.front().samples.front().name == "Frame",
			"The compressed V1 segment must reconstruct its bounded local string identifiers exactly.");
		context.Expect(result.complete && !result.recoveredIncompleteTail,
			"A terminal record must make the journal complete.");

		const std::filesystem::path historical = workspace.Path("historical.teprof");
		const std::string oldFormat = "TerraEngineProfiler" "Capture";
		const std::string oldVersionField = "payload" "Version";
		workspace.WriteBytes("historical.teprof",
			"{\"format\":\"" + oldFormat + "\",\"" + oldVersionField + "\":2}");
		context.Expect(OpenProfilerJournalV1(historical, loaded, result, error)
			== ProfilerJournalV1ResultCode::UnsupportedFormat,
			"Historical profiler JSON must be rejected before compatibility parsing.");
		workspace.WriteBytes("historical.teprof",
			"{\"format\":\"" + oldFormat + "\",\"" + oldVersionField + "\":1}");
		context.Expect(OpenProfilerJournalV1(historical, loaded, result, error)
			== ProfilerJournalV1ResultCode::UnsupportedFormat,
			"Historical payload-v1 JSON must be rejected by the same magic check.");
	});

	registry.Add("Engine.ProfilerJournalV1.CompressesRepetitiveSegmentsAndRejectsOversizedHeaders", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Compression");
		const std::filesystem::path path = workspace.Path("compressed.teprof");
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.maximumChunkFrames = 32u;
		metadata.settings.maximumChunkBytes = 8u * 1024u * 1024u;
		std::vector<ProfilerSealedFrame> frames;
		frames.reserve(metadata.settings.maximumChunkFrames);
		std::uint64_t rawFrameBytes = 0u;
		std::string error;
		for (std::uint64_t index = 0u; index < metadata.settings.maximumChunkFrames; ++index)
		{
			ProfilerSealedFrame frame = MakeSealedFrame(index,
				"Repeated profiler scope label shared by every frame in this segment");
			std::string encoded;
			context.Expect(EncodeProfilerJournalFrame(frame.frame, metadata.settings, encoded, error),
				"The repetitive compression fixture should encode: " + error);
			rawFrameBytes += encoded.size();
			frames.push_back(std::move(frame));
		}

		ProfilerJournalV1Writer writer;
		context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The compressed journal should open: " + error);
		const std::span<const ProfilerSealedFrame> firstSegment(frames.data(), 16u);
		const std::span<const ProfilerSealedFrame> secondSegment(frames.data() + 16u, 16u);
		context.Expect(writer.AppendCommittedSegment(firstSegment, error) == ProfilerJournalV1ResultCode::Ok,
			"The first repetitive segment should commit: " + error);
		context.Expect(writer.AppendCommittedSegment(secondSegment, error) == ProfilerJournalV1ResultCode::Ok,
			"The writer should reuse its compression workspace across a second segment: " + error);
		context.Expect(writer.GetLastStoredSegmentBytes() < rawFrameBytes / 2u,
			"Segment-level compression should materially reduce repetitive profiler evidence.");
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The compressed journal should finalize: " + error);
		ProfilerCaptureDocument decoded;
		ProfilerJournalV1OpenResult decodedIndex;
		context.Expect(OpenProfilerJournalV1(path, decoded, decodedIndex, error)
			== ProfilerJournalV1ResultCode::Ok && decoded.frames.size() == frames.size(),
			"Both segments written through the reused compression workspace must round-trip exactly: " + error);

		// The uncompressed-size field follows the segment sequence, frame range,
		// frame count, and codec byte. Mutating it above the configured chunk bound
		// must fail before the reader allocates or asks zstd to decode the payload.
		std::string bytes = ReadBytes(path);
		const std::size_t segment = bytes.find("SEG1");
		const std::size_t uncompressedSizeOffset = segment == std::string::npos
			? std::string::npos : segment + 4u + 8u + 8u + 8u + 4u + 1u;
		context.Expect(uncompressedSizeOffset != std::string::npos
			&& uncompressedSizeOffset + 8u <= bytes.size(),
			"The fixture must contain a bounded compressed-segment header.");
		if (uncompressedSizeOffset != std::string::npos
			&& uncompressedSizeOffset + 8u <= bytes.size())
		{
			const std::uint64_t oversized = metadata.settings.maximumChunkBytes + 1u;
			for (std::uint32_t byte = 0u; byte < 8u; ++byte)
			{
				bytes[uncompressedSizeOffset + byte] = static_cast<char>(
					(oversized >> (byte * 8u)) & 0xffu);
			}
			workspace.WriteBytes("compressed.teprof", bytes);
		}
		ProfilerJournalV1OpenResult result;
		context.Expect(ReadProfilerJournalV1Index(path, result, error)
			== ProfilerJournalV1ResultCode::CorruptCommittedData,
			"A segment declaring an uncompressed size above its configured bound must fail closed.");
	});

	registry.Add("Engine.ProfilerJournalV1.IndexReadsCompactCheckpointsWithoutDecodingDetail", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1CompactIndex");
		const std::filesystem::path path = workspace.Path("compact-index.teprof");
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.maximumChunkFrames = 4u;
		std::vector<ProfilerSealedFrame> frames;
		for (std::uint64_t index = 40u; index < 44u; ++index)
		{
			frames.push_back(MakeSealedFrame(index, "Detail that index-only reads must skip",
				static_cast<double>(index), 10.0 + static_cast<double>(index - 40u)));
		}
		std::string error;
		ProfilerJournalV1Writer writer;
		context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The compact-index journal should open: " + error);
		context.Expect(writer.AppendCommittedSegment(frames, error) == ProfilerJournalV1ResultCode::Ok,
			"The compact-index segment should commit: " + error);
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The compact-index journal should finalize: " + error);

		// Damage only the compressed detail bytes. The compact checkpoint and
		// Terminal remain intact, so metadata/index listing must still be bounded
		// and usable while a later detail decode fails closed on the corruption.
		std::string bytes = ReadBytes(path);
		const std::size_t segment = bytes.find("SEG1");
		const std::size_t payloadOffset = segment == std::string::npos
			? std::string::npos : segment + 4u + 8u + 8u + 8u + 4u + 1u + 8u + 8u + 8u;
		context.Expect(payloadOffset != std::string::npos && payloadOffset < bytes.size(),
			"The compact-index fixture must expose compressed detail bytes.");
		if (payloadOffset != std::string::npos && payloadOffset < bytes.size())
		{
			bytes[payloadOffset] ^= 0x2a;
			workspace.WriteBytes("compact-index.teprof", bytes);
		}

		ProfilerJournalV1OpenResult index;
		context.Expect(ReadProfilerJournalV1Index(path, index, error) == ProfilerJournalV1ResultCode::Ok,
			"Index-only listing must use compact checkpoints rather than decode detail: " + error);
		context.ExpectEqual(4ULL, static_cast<unsigned long long>(index.index.size()),
			"The compact checkpoint must reconstruct every frame-list row.");
		context.Expect(index.index.size() == 4u && index.index.front().frameIndex == 40u
			&& index.index.back().frameIndex == 43u
			&& index.index.front().durationMs == 10.0 && index.index.back().durationMs == 13.0,
			"Compact index identity and durations must agree with the committed frames.");

		ProfilerCaptureDocument rejected;
		context.Expect(OpenProfilerJournalV1(path, rejected, index, error)
			== ProfilerJournalV1ResultCode::CorruptCommittedData,
			"Opening detailed evidence must still reject the corrupted compressed segment.");
	});

	registry.Add("Engine.ProfilerJournalV1.ThirtyThousandFramesCommitWithBoundedWriterState", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1LongRun");
		ProfilerJournalV1Writer writer;
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.maximumChunkFrames = 120u;
		std::string error;
		context.Expect(writer.Open(workspace.Path("long.teprof"), metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The long-run journal should open: " + error);
		std::vector<ProfilerSealedFrame> segment;
		segment.reserve(metadata.settings.maximumChunkFrames);
		for (std::uint64_t index = 0u; index < 30000u; ++index)
		{
			segment.push_back(MakeSealedFrame(index));
			if (segment.size() == metadata.settings.maximumChunkFrames)
			{
				context.Expect(writer.AppendCommittedSegment(segment, error) == ProfilerJournalV1ResultCode::Ok,
					"Every bounded long-run segment must commit: " + error);
				segment.clear();
			}
		}
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The long-run journal should finalize: " + error);
		context.ExpectEqual(30000ULL, static_cast<unsigned long long>(writer.GetSummary().frameCount),
			"Incremental writer summary must agree with every committed frame.");
		context.ExpectEqual(250ULL, static_cast<unsigned long long>(writer.GetSummary().committedSegmentCount),
			"The writer must retain only bounded segments rather than one growing capture document.");
	});

	registry.Add("Engine.ProfilerJournalV1.EnforcesFrameBoundsAndStrictOrdering", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Bounds");
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.maximumSealedFrameBytes = 8u;
		ProfilerJournalV1Writer writer;
		std::string error;
		context.Expect(writer.Open(workspace.Path("bounded.teprof"), metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The bounded journal should open: " + error);
		const ProfilerSealedFrame oversized = MakeSealedFrame(1u, "A deliberately long sample label");
		context.Expect(writer.AppendCommittedSegment(std::span(&oversized, 1u), error)
			== ProfilerJournalV1ResultCode::InvalidInput,
			"A frame above the configured sealed-frame bound must reject before writing.");

		metadata.settings.maximumSealedFrameBytes = 1024u * 1024u;
		ProfilerJournalV1Writer orderedWriter;
		context.Expect(orderedWriter.Open(workspace.Path("ordered.teprof"), metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The ordered journal should open: " + error);
		const ProfilerSealedFrame later = MakeSealedFrame(2u);
		const ProfilerSealedFrame earlier = MakeSealedFrame(1u);
		context.Expect(orderedWriter.AppendCommittedSegment(std::span(&later, 1u), error) == ProfilerJournalV1ResultCode::Ok,
			"The first ordered frame should commit: " + error);
		context.Expect(orderedWriter.AppendCommittedSegment(std::span(&earlier, 1u), error)
			== ProfilerJournalV1ResultCode::InvalidInput,
			"Duplicate or out-of-order frame indices must reject atomically.");
	});

	registry.Add("Engine.ProfilerJournalV1.RejectsIncoherentResidentBudgets", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Budget");
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.residentByteBudget = 1024u;
		metadata.settings.maximumPendingBytes = 2048u;
		metadata.settings.maximumChunkBytes = 512u;
		metadata.settings.maximumSealedFrameBytes = 256u;
		ProfilerJournalV1Writer writer;
		std::string error;
		context.Expect(writer.Open(workspace.Path("budget.teprof"), metadata, error)
			== ProfilerJournalV1ResultCode::InvalidInput,
			"A pending queue larger than the resident budget must fail before recording starts.");
	});

	registry.Add("Engine.ProfilerJournalV1.PersistsExactFirstAndFinalFiveMinuteWindows", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Windows");
		const std::filesystem::path path = workspace.Path("windows.teprof");
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.maximumChunkFrames = 4u;
		ProfilerJournalV1Writer writer;
		std::string error;
		context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The window journal should open: " + error);

		// Sample a synthetic 30-minute session every three seconds. The first
		// window is 10 ms, the middle is 15 ms, and the final window is 20 ms.
		// This proves time-window retention without making this unit test sleep.
		for (std::uint64_t first = 0u; first <= 600u; first += 4u)
		{
			std::vector<ProfilerSealedFrame> batch;
			for (std::uint64_t ordinal = first;
				ordinal < std::min<std::uint64_t>(first + 4u, 601u);
				++ordinal)
			{
				const double startMs = static_cast<double>(ordinal) * 3000.0;
				const double durationMs = startMs <= 300000.0 ? 10.0
					: (startMs >= 1500000.0 ? 20.0 : 15.0);
				batch.push_back(MakeSealedFrame(ordinal, "Window", startMs, durationMs));
			}
			context.Expect(writer.AppendCommittedSegment(batch, error) == ProfilerJournalV1ResultCode::Ok,
				"Each synthetic window segment should commit: " + error);
		}
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The synthetic window journal should finalize: " + error);

		ProfilerJournalV1OpenResult result;
		const ProfilerJournalV1ResultCode readResult = ReadProfilerJournalV1Index(path, result, error);
		context.Expect(readResult == ProfilerJournalV1ResultCode::Ok,
			"The persisted window summary should reopen without detailed decoding: " + error);
		context.ExpectEqual(101ULL,
			static_cast<unsigned long long>(result.summary.firstEngineCpuWindowSampleCount),
			"The first window should contain exactly five minutes of three-second samples.");
		context.ExpectEqual(10000000ULL,
			static_cast<unsigned long long>(result.summary.firstEngineCpuWindowMedianNanoseconds),
			"The first-window median should preserve 10 ms exactly.");
		context.ExpectEqual(10000000ULL,
			static_cast<unsigned long long>(result.summary.firstEngineCpuWindowP95Nanoseconds),
			"The first-window P95 should preserve 10 ms exactly.");
		context.ExpectEqual(101ULL,
			static_cast<unsigned long long>(result.summary.finalEngineCpuWindowSampleCount),
			"The final window should contain exactly the last five minutes.");
		context.ExpectEqual(20000000ULL,
			static_cast<unsigned long long>(result.summary.finalEngineCpuWindowMedianNanoseconds),
			"The final-window median should preserve 20 ms exactly.");
		context.ExpectEqual(20000000ULL,
			static_cast<unsigned long long>(result.summary.finalEngineCpuWindowP95Nanoseconds),
			"The final-window P95 should preserve 20 ms exactly.");
	});

	registry.Add("Engine.ProfilerJournalV1.PersistsBoundedProfilerSelfSummary", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1SelfSummary");
		const std::filesystem::path path = workspace.Path("self-summary.teprof");
		ProfilerJournalV1Writer writer;
		std::string error;
		context.Expect(writer.Open(path, MakeMetadata(), error) == ProfilerJournalV1ResultCode::Ok,
			"The profiler-self summary journal should open: " + error);
		const ProfilerSealedFrame frame = MakeSealedFrame(7u, "Self summary");
		ProfilerSealedFrame frameWithSubsystems = frame;
		frameWithSubsystems.frame.moduleSummaries = {
			{ProfilerModuleId::CpuEngine, true, 1.25, {}},
			{ProfilerModuleId::TaskExecution, true, 0.5, {}}};
		frameWithSubsystems.ownedByteCount = EstimateProfilerFrameOwnedBytes(
			frameWithSubsystems.frame);
		context.Expect(writer.AppendCommittedSegment(std::span(&frameWithSubsystems, 1u), error)
			== ProfilerJournalV1ResultCode::Ok,
			"The profiler-self summary frame should commit: " + error);

		ProfilerCaptureHealth health;
		health.persistenceLagHighWaterFrames = 9u;
		health.collectionNanoseconds = 101u;
		health.sealingNanoseconds = 102u;
		health.queueingNanoseconds = 103u;
		health.persistenceNanoseconds = 104u;
		health.finalizationNanoseconds = 108u;
		health.persistedBytes = 105u;
		health.persistenceOperationCount = 6u;
		health.persistenceThroughputBytesPerSecond = 107u;
		writer.UpdateProfilerSelfSummary(health);
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The profiler-self summary journal should finalize: " + error);

		ProfilerJournalV1OpenResult result;
		const ProfilerJournalV1ResultCode readResult = ReadProfilerJournalV1Index(path, result, error);
		context.Expect(readResult == ProfilerJournalV1ResultCode::Ok,
			"The profiler-self summary should reopen from Terminal: " + error);
		context.Expect(result.summary.persistenceLagHighWaterFrames == 9u
			&& result.summary.profilerCollectionNanoseconds == 101u
			&& result.summary.profilerSealingNanoseconds == 102u
			&& result.summary.profilerQueueingNanoseconds == 103u
			&& result.summary.profilerPersistenceNanoseconds == 104u
			&& result.summary.profilerFinalizationNanoseconds >= 108u
			&& result.summary.persistedBytes == 105u
			&& result.summary.persistenceOperationCount == 6u
			&& result.summary.persistenceThroughputBytesPerSecond == 107u,
			"Terminal must preserve bounded profiler-self and pre-terminal sealing totals without scanning frame history.");
		const ProfilerJournalV1SubsystemSummary& cpu = result.summary.subsystemSummaries[
			static_cast<std::size_t>(ProfilerModuleId::CpuEngine)];
		const ProfilerJournalV1SubsystemSummary& tasks = result.summary.subsystemSummaries[
			static_cast<std::size_t>(ProfilerModuleId::TaskExecution)];
		context.Expect(result.summary.frameCount == 1u
			&& cpu.availableFrameCount == 1u && cpu.totalNanoseconds == 1250000u
			&& tasks.availableFrameCount == 1u && tasks.totalNanoseconds == 500000u,
			"Terminal subsystem lanes must conserve the exact committed-frame scalar totals.");
	});

	registry.Add("Engine.ProfilerJournalV1.CheckpointsAndSelectedFrameUseCommittedRanges", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Checkpoint");
		ProfilerCaptureMetadata metadata = MakeMetadata();
		metadata.settings.maximumChunkFrames = 1u;
		std::string error;
		ProfilerJournalV1Writer writer;
		const std::filesystem::path path = workspace.Path("checkpoint.teprof");
		context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
			"The checkpoint journal should open: " + error);
		for (std::uint64_t index = 0u; index < 33u; ++index)
		{
			const ProfilerSealedFrame frame = MakeSealedFrame(index, "Selected range");
			context.Expect(writer.AppendCommittedSegment(std::span(&frame, 1u), error)
				== ProfilerJournalV1ResultCode::Ok,
				"Each checkpoint fixture segment should commit: " + error);
		}
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The checkpoint journal should finalize: " + error);
		const std::string bytes = ReadBytes(path);
		std::size_t checkpointCount = 0u;
		for (std::size_t offset = bytes.find("IDX1"); offset != std::string::npos;
			offset = bytes.find("IDX1", offset + 4u))
		{
			++checkpointCount;
		}
		context.ExpectEqual(std::size_t(2u), checkpointCount,
			"Thirty-three one-frame segments should flush one stride page and one final partial page, not one checkpoint per segment.");
		ProfilerJournalV1OpenResult page;
		std::uint64_t totalFrames = 0u;
		context.Expect(ReadProfilerJournalV1IndexPage(path, 10u, 5u, page, totalFrames, error)
			== ProfilerJournalV1ResultCode::Ok,
			"A bounded middle index page should stream from checkpoint rows: " + error);
		context.Expect(totalFrames == 33u && page.index.size() == 5u
			&& page.index.front().frameIndex == 10u && page.index.back().frameIndex == 14u,
			"The bounded page must retain only the requested five rows and authoritative total.");
		context.Expect(ReadProfilerJournalV1IndexPage(path,
			kProfilerJournalV1NewestIndexPage, 4u, page, totalFrames, error)
			== ProfilerJournalV1ResultCode::Ok,
			"The newest-page sentinel should stream the final bounded rows: " + error);
		context.Expect(page.index.size() == 4u && page.index.front().frameIndex == 29u
			&& page.index.back().frameIndex == 32u,
			"Newest-page streaming must not retain the preceding frame history.");
		context.Expect(ReadProfilerJournalV1IndexPage(path, 0u, 0u, page, totalFrames, error)
			== ProfilerJournalV1ResultCode::Ok && page.index.empty() && totalFrames == 33u,
			"Metadata-only validation must retain no frame-list rows.");
		ProfilerFrameRecord selected;
		context.Expect(ReadProfilerJournalV1SelectedFrame(path, 32u, selected, error)
			== ProfilerJournalV1ResultCode::Ok,
			"A selected read should use the committed segment range: " + error);
		context.ExpectEqual(32ULL, static_cast<unsigned long long>(selected.frameIndex),
			"Selected-frame lookup must return the exact committed frame.");

		std::string corrupted = bytes;
		const std::size_t firstSegment = corrupted.find("SEG1");
		if (firstSegment != std::string::npos)
		{
			// Damage an unrelated SEG1 envelope, not merely its compressed bytes.
			// A sequential selected reader would stop before frame 32; the direct
			// END1 -> IDX1 -> selected SEG1 path must not touch this older record.
			corrupted[firstSegment] ^= 0x31;
			workspace.WriteBytes("checkpoint.teprof", corrupted);
		}
		context.Expect(ReadProfilerJournalV1SelectedFrame(path, 32u, selected, error)
			== ProfilerJournalV1ResultCode::Ok,
			"A selected read should seek past an unreadable unrelated SEG1 envelope: " + error);
		ProfilerCaptureDocument corruptDocument;
		ProfilerJournalV1OpenResult corruptResult;
		context.Expect(OpenProfilerJournalV1(path, corruptDocument, corruptResult, error)
			== ProfilerJournalV1ResultCode::CorruptCommittedData,
			"A full validation must still reject corruption in any committed segment.");

		// The bounded lookup intentionally skips unrelated payload detail, but it
		// must fail closed for the one compressed segment it chooses to expose.
		// Reset the earlier unrelated corruption, then damage frame 32's segment.
		corrupted = bytes;
		const std::size_t selectedSegment = corrupted.rfind("SEG1");
		const std::size_t selectedPayload = selectedSegment == std::string::npos
			? std::string::npos
			: selectedSegment + 4u + 8u + 8u + 8u + 4u + 1u + 8u + 8u + 8u;
		if (selectedPayload != std::string::npos && selectedPayload < corrupted.size())
		{
			corrupted[selectedPayload] ^= 0x47;
			workspace.WriteBytes("checkpoint.teprof", corrupted);
		}
		context.Expect(ReadProfilerJournalV1SelectedFrame(path, 32u, selected, error)
			== ProfilerJournalV1ResultCode::CorruptCommittedData,
			"A selected lookup must verify its directly-read stored payload before returning detail.");
	});

	registry.Add("Engine.ProfilerJournalV1.RecoversOnlyUncommittedTailAndRejectsCorruption", [](TestContext& context) {
		using namespace TerraEngine::Profiling;
		ProfilerTestSupport::TemporaryWorkspace workspace("ProfilerJournalV1Recovery");
		const std::filesystem::path uncheckpointedPath = workspace.Path("uncheckpointed.teprof");
		std::string error;
		{
			ProfilerJournalV1Writer interruptedWriter;
			context.Expect(interruptedWriter.Open(uncheckpointedPath, MakeMetadata(), error)
				== ProfilerJournalV1ResultCode::Ok,
				"The uncheckpointed-tail fixture should open: " + error);
			const ProfilerSealedFrame interruptedFrame = MakeSealedFrame(1u);
			context.Expect(interruptedWriter.AppendCommittedSegment(
				std::span(&interruptedFrame, 1u), error) == ProfilerJournalV1ResultCode::Ok,
				"The interrupted fixture should durably commit before its sparse checkpoint: " + error);
		}
		ProfilerJournalV1OpenResult interruptedIndex;
		context.Expect(ReadProfilerJournalV1Index(uncheckpointedPath, interruptedIndex, error)
			== ProfilerJournalV1ResultCode::IncompleteTail,
			"Index reopen should recover the bounded committed tail after the last sparse page: " + error);
		context.ExpectEqual(std::size_t(1u), interruptedIndex.index.size(),
			"Sparse-page recovery must reconstruct every committed tail frame.");

		const std::filesystem::path path = workspace.Path("recovery.teprof");
		ProfilerJournalV1Writer writer;
		context.Expect(writer.Open(path, MakeMetadata(), error) == ProfilerJournalV1ResultCode::Ok,
			"The recovery journal should open: " + error);
		const ProfilerSealedFrame frame = MakeSealedFrame(3u);
		context.Expect(writer.AppendCommittedSegment(std::span(&frame, 1u), error) == ProfilerJournalV1ResultCode::Ok,
			"The recovery journal should commit one frame: " + error);
		context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
			"The recovery journal should write its terminal: " + error);

		std::string bytes = ReadBytes(path);
		const std::size_t terminal = bytes.rfind("END1");
		context.Expect(terminal != std::string::npos, "The V1 fixture must contain a terminal record.");
		if (terminal != std::string::npos)
		{
			context.Expect(workspace.TruncateFile("recovery.teprof", terminal),
				"The deterministic truncation helper should retain the committed prefix exactly.");
		}
		ProfilerCaptureDocument recovered;
		ProfilerJournalV1OpenResult recoveryResult;
		context.Expect(OpenProfilerJournalV1(path, recovered, recoveryResult, error)
			== ProfilerJournalV1ResultCode::IncompleteTail,
			"A missing terminal must recover preceding committed segments: " + error);
		context.ExpectEqual(1ULL, static_cast<unsigned long long>(recovered.frames.size()),
			"Tail recovery must retain the committed prefix exactly.");

		bytes = ReadBytes(path);
		const std::size_t segment = bytes.find("SEG1");
		const std::size_t segmentPayloadOffset = segment == std::string::npos
			? std::string::npos : segment + 4u + 8u + 8u + 8u + 4u + 1u + 8u + 8u + 8u;
		context.Expect(segmentPayloadOffset != std::string::npos
			&& segmentPayloadOffset < bytes.size(),
			"The fixture must expose a committed V1 segment payload for corruption testing.");
		if (segmentPayloadOffset != std::string::npos && segmentPayloadOffset < bytes.size())
		{
			bytes[segmentPayloadOffset] ^= 0x5a;
			workspace.WriteBytes("recovery.teprof", bytes);
		}
		ProfilerCaptureDocument rejected;
		context.Expect(OpenProfilerJournalV1(path, rejected, recoveryResult, error)
			== ProfilerJournalV1ResultCode::CorruptCommittedData,
			"Corruption inside a committed segment must fail closed.");
	});
}
}
