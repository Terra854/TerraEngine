// ProfilerJournalV1.h -- Sole append-only profiler capture format.
//
// A journal is opened before recording begins, receives already-sealed frames
// in bounded segments, and appends a terminal record only after Finish.  The
// format identifier is intentionally not the historical JSON payload number:
// old profiler files therefore fail before any compatibility parser is reached.

#pragma once

#include "Profiling/ProfilerCapture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Profiling
{
inline constexpr std::string_view kProfilerJournalV1FormatIdentifier =
	"TerraEngineProfilerJournalV1";
inline constexpr std::uint32_t kProfilerJournalV1SchemaVersion = 1u;

// Result codes are stable, typed failure categories for service and automation
// callers.  Human-readable detail remains in the companion error string.
enum class ProfilerJournalV1ResultCode : std::uint8_t
{
	Ok,
	NotOpen,
	IoFailure,
	InvalidInput,
	UnsupportedFormat,
	CorruptCommittedData,
	IncompleteTail,
	AlreadyFinalized
};

// One compact entry is enough to list a finished capture without decoding its
// detailed sample hierarchy.  The offset points at the containing committed
// segment and is private to this format lineage.
struct ProfilerJournalV1IndexEntry
{
	std::uint64_t frameIndex = 0u;
	double durationMs = 0.0;
	bool complete = true;
	std::uint64_t segmentOffset = 0u;
	// Digest of the stored compressed segment record. Selected reads validate
	// this durable value before they decompress the one requested payload.
	std::uint64_t storedPayloadDigest = 0u;
};

// Each module receives a fixed scalar lane in Terminal. This lets a catalog
// show bounded subsystem totals without reopening every detailed frame.
struct ProfilerJournalV1SubsystemSummary
{
	std::uint64_t availableFrameCount = 0u;
	std::uint64_t totalNanoseconds = 0u;
};

inline constexpr std::size_t kProfilerJournalV1SubsystemCount =
	static_cast<std::size_t>(ProfilerModuleId::TaskExecution) + 1u;

// ProfilerJournalV1Summary is persisted in terminal records and incrementally
// maintained while writing.  It keeps ordinary status/UI reads bounded even
// when a capture has been recording for hours.
struct ProfilerJournalV1Summary
{
	std::uint64_t frameCount = 0u;
	std::uint64_t completeFrameCount = 0u;
	std::uint64_t totalDurationNanoseconds = 0u;
	std::uint64_t droppedDetailRecordCount = 0u;
	std::uint64_t droppedDetailByteCount = 0u;
	std::uint64_t committedSegmentCount = 0u;
	// Engine CPU aggregates are persisted incrementally so catalog/benchmark
	// reads do not need to decode every detailed frame to compute a mean.
	std::uint64_t engineCpuSampleCount = 0u;
	std::uint64_t engineCpuTotalNanoseconds = 0u;
	std::array<ProfilerJournalV1SubsystemSummary, kProfilerJournalV1SubsystemCount>
		subsystemSummaries{};
	std::uint64_t persistenceLagHighWaterFrames = 0u;
	// Window values are scalar terminal facts.  Writer-only sample rings are
	// capped at 240 Hz * 300 seconds, so long recordings cannot grow memory.
	std::uint64_t firstEngineCpuWindowSampleCount = 0u;
	std::uint64_t firstEngineCpuWindowTotalNanoseconds = 0u;
	std::uint64_t firstEngineCpuWindowMedianNanoseconds = 0u;
	std::uint64_t firstEngineCpuWindowP95Nanoseconds = 0u;
	std::uint64_t finalEngineCpuWindowSampleCount = 0u;
	std::uint64_t finalEngineCpuWindowTotalNanoseconds = 0u;
	std::uint64_t finalEngineCpuWindowMedianNanoseconds = 0u;
	std::uint64_t finalEngineCpuWindowP95Nanoseconds = 0u;
	std::uint64_t profilerCollectionNanoseconds = 0u;
	std::uint64_t profilerSealingNanoseconds = 0u;
	std::uint64_t profilerQueueingNanoseconds = 0u;
	std::uint64_t profilerPersistenceNanoseconds = 0u;
	// Terminal cannot contain the duration of its own final WriteAndFlush: that
	// value exists only after its immutable bytes are durable. This field is the
	// deterministic pre-terminal sealing/checkpoint duration plus any earlier
	// lifecycle finalization work supplied by the producer. Live health remains
	// authoritative for the complete sink-inclusive finalization duration.
	std::uint64_t profilerFinalizationNanoseconds = 0u;
	std::uint64_t persistedBytes = 0u;
	std::uint64_t persistenceOperationCount = 0u;
	std::uint64_t persistenceThroughputBytesPerSecond = 0u;
	// Batching efficiency separates producer-owned logical values from encoded
	// and durable bytes so reports never mistake resident pressure for disk I/O.
	std::uint64_t logicalInputBytes = 0u;
	std::uint64_t encodedPayloadBytes = 0u;
	std::uint64_t storedSegmentBytes = 0u;
};

// ProfilerJournalV1OpenResult exposes the persisted identity and recovery
// facts without requiring callers to materialize every detailed frame.
struct ProfilerJournalV1OpenResult
{
	ProfilerCaptureMetadata metadata;
	ProfilerJournalV1Summary summary;
	std::vector<ProfilerJournalV1IndexEntry> index;
	bool complete = false;
	bool recoveredIncompleteTail = false;
};

// ProfilerJournalV1Sink is the narrow persistence boundary used by the
// serialized drain job. Production uses the bundled filesystem sink; tests can
// inject deterministic slow or unavailable storage without sleeping or
// replacing the engine ThreadPool.
class ProfilerJournalV1Sink
{
public:
	virtual ~ProfilerJournalV1Sink() = default;
	[[nodiscard]] virtual bool Open(
		const std::filesystem::path& path,
		std::string& outError) = 0;
	[[nodiscard]] virtual bool WriteAndFlush(
		std::string_view bytes,
		std::string& outError) = 0;
	[[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

[[nodiscard]] std::shared_ptr<ProfilerJournalV1Sink> CreateFilesystemProfilerJournalV1Sink();

class ProfilerJournalV1Writer
{
public:
	ProfilerJournalV1Writer();
	explicit ProfilerJournalV1Writer(std::shared_ptr<ProfilerJournalV1Sink> sink);
	~ProfilerJournalV1Writer();
	ProfilerJournalV1Writer(ProfilerJournalV1Writer&&) noexcept;
	ProfilerJournalV1Writer& operator=(ProfilerJournalV1Writer&&) noexcept;
	ProfilerJournalV1Writer(const ProfilerJournalV1Writer&) = delete;
	ProfilerJournalV1Writer& operator=(const ProfilerJournalV1Writer&) = delete;

	// Open creates the fresh V1 header immediately.  A recording cannot start
	// when this validation fails, avoiding a later Finish-time path surprise.
	[[nodiscard]] ProfilerJournalV1ResultCode Open(
		const std::filesystem::path& path,
		const ProfilerCaptureMetadata& metadata,
		std::string& outError);
	// Append commits one bounded segment.  The call owns no live profiler state;
	// persistence workers can safely invoke it after producer-side sealing.
	[[nodiscard]] ProfilerJournalV1ResultCode AppendCommittedSegment(
		std::span<const ProfilerSealedFrame> frames,
		std::string& outError);
	// Finish appends the one authoritative terminal record.  Passing false is
	// used only for controlled cancellation/error paths; unexpected interruption
	// simply leaves a recoverable journal without a terminal record.
	[[nodiscard]] ProfilerJournalV1ResultCode Finalize(
		bool complete,
		std::string& outError);
	[[nodiscard]] bool IsOpen() const noexcept;
	[[nodiscard]] bool IsFinalized() const noexcept;
	[[nodiscard]] const ProfilerJournalV1Summary& GetSummary() const noexcept;
	// Stored bytes include the segment header, compressed payload, and durable
	// commit footer; queue residency intentionally uses a different logical size.
	[[nodiscard]] std::uint64_t GetLastStoredSegmentBytes() const noexcept;
	// The serialized drain owns these counters. Copying their scalar snapshot
	// before Terminal keeps profiler-self attribution available after reopening.
	void UpdateProfilerSelfSummary(const ProfilerCaptureHealth& health) noexcept;

private:
	[[nodiscard]] ProfilerJournalV1ResultCode FlushPendingCheckpoint(
		std::string& outError);
	struct Impl;
	std::unique_ptr<Impl> impl;
};

// OpenProfilerJournalV1 validates the exact header before allocating decoded
// frame data.  Only committed segments are returned after a truncated tail;
// corruption inside a committed record fails closed and returns no document.
[[nodiscard]] ProfilerJournalV1ResultCode OpenProfilerJournalV1(
	const std::filesystem::path& path,
	ProfilerCaptureDocument& outDocument,
	ProfilerJournalV1OpenResult& outResult,
	std::string& outError);

// This metadata/index helper is used by catalog consumers.  It follows the
// same integrity rules as the full reader but leaves detailed frame payloads
// on disk.
[[nodiscard]] ProfilerJournalV1ResultCode ReadProfilerJournalV1Index(
	const std::filesystem::path& path,
	ProfilerJournalV1OpenResult& outResult,
	std::string& outError);

// Reads one bounded catalog page without materializing the complete frame
// index. Pass kProfilerJournalV1NewestIndexPage to select the final page, or
// pageSize zero to validate/read metadata only.
inline constexpr std::uint64_t kProfilerJournalV1NewestIndexPage =
	std::numeric_limits<std::uint64_t>::max();
[[nodiscard]] ProfilerJournalV1ResultCode ReadProfilerJournalV1IndexPage(
	const std::filesystem::path& path,
	std::uint64_t offset,
	std::uint32_t pageSize,
	ProfilerJournalV1OpenResult& outResult,
	std::uint64_t& outTotalFrameCount,
	std::string& outError);

// Reads at most one detailed frame into caller storage. The completed-journal
// path seeks from END1 through compact IDX1 pages to the selected SEG1/CMT1
// pair, so it never replays unrelated segment envelopes or decodes their
// payloads. Interrupted journals retain bounded-tail recovery instead.
[[nodiscard]] ProfilerJournalV1ResultCode ReadProfilerJournalV1SelectedFrame(
	const std::filesystem::path& path,
	std::uint64_t frameIndex,
	ProfilerFrameRecord& outFrame,
	std::string& outError);
}
