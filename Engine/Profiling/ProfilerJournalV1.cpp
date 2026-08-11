// ProfilerJournalV1.cpp -- Append-only, recoverable profiler capture journal.

#include "Profiling/ProfilerJournalV1.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <utility>
#include <zstd.h>

namespace TerraEngine::Profiling
{
namespace
{
constexpr std::array<char, 16u> kJournalMagic = {
	'T', 'E', 'P', 'R', 'O', 'F', 'J', 'O', 'U', 'R', 'N', 'A', 'L', 'V', '1', '\0'};
constexpr std::array<char, 4u> kSegmentMagic = {'S', 'E', 'G', '1'};
constexpr std::array<char, 4u> kCommitMagic = {'C', 'M', 'T', '1'};
constexpr std::array<char, 4u> kIndexCheckpointMagic = {'I', 'D', 'X', '1'};
constexpr std::array<char, 4u> kTerminalMagic = {'E', 'N', 'D', '1'};
constexpr std::uint64_t kMaximumJournalRecordBytes = 64u * 1024u * 1024u;

enum class SegmentCodec : std::uint8_t
{
	Zstd = 1u
};

bool CompressSegment(
	ZSTD_CCtx& context,
	std::string_view source,
	std::string& out,
	std::string& error)
{
	const std::size_t bound = ZSTD_compressBound(source.size());
	if (bound > kMaximumJournalRecordBytes)
	{
		error = "Profiler journal V1 compressed segment bound is too large.";
		return false;
	}
	out.resize(bound);
	// Live profiling values latency and steady CPU headroom over archival density.
	// Level one retains deterministic Zstd V1 decoding/integrity semantics while
	// avoiding the higher parse/search cost previously paid by every segment.
	const std::size_t written = ZSTD_compressCCtx(
		&context, out.data(), out.size(), source.data(), source.size(), 1);
	if (ZSTD_isError(written) != 0u)
	{
		error = ZSTD_getErrorName(written);
		return false;
	}
	out.resize(written);
	return true;
}

bool DecompressSegment(std::string_view stored, std::uint64_t uncompressedBytes, std::string& out, std::string& error)
{
	if (uncompressedBytes == 0u || uncompressedBytes > kMaximumJournalRecordBytes)
	{
		error = "Profiler journal V1 uncompressed segment length is outside bounds.";
		return false;
	}
	out.resize(static_cast<std::size_t>(uncompressedBytes));
	const std::size_t decoded = ZSTD_decompress(out.data(), out.size(), stored.data(), stored.size());
	if (ZSTD_isError(decoded) != 0u || decoded != out.size())
	{
		error = "Profiler journal V1 compressed segment failed integrity decode.";
		return false;
	}
	return true;
}
constexpr std::uint64_t kCheckpointSegmentStride = 32u;
constexpr std::size_t kMaximumFiveMinuteSamples = 240u * 300u;
constexpr double kFiveMinuteWindowMilliseconds = 300000.0;
// END1 has a fixed scalar layout. Keeping its byte count explicit lets a
// selected-frame request find the terminal anchor with one seek instead of
// replaying an unbounded stream of segment headers.
constexpr std::size_t kTerminalUnsignedFieldCount = 10u
	+ (kProfilerJournalV1SubsystemCount * 2u) + 4u + 4u + 5u + 3u + 3u + 3u;
constexpr std::size_t kTerminalRecordBytes = kTerminalMagic.size()
	+ (kTerminalUnsignedFieldCount * sizeof(std::uint64_t)) + 1u
	+ sizeof(std::uint64_t);

enum class ReadState : std::uint8_t
{
	Ok,
	End,
	Partial
};

// The journal encodes integers one byte at a time.  This keeps artifacts
// portable across host endianness without depending on packed C++ structs.
void AppendU32(std::string& output, std::uint32_t value)
{
	for (std::uint32_t shift = 0u; shift < 32u; shift += 8u)
	{
		output.push_back(static_cast<char>((value >> shift) & 0xffu));
	}
}

void AppendU64(std::string& output, std::uint64_t value)
{
	for (std::uint32_t shift = 0u; shift < 64u; shift += 8u)
	{
		output.push_back(static_cast<char>((value >> shift) & 0xffu));
	}
}

void AppendDouble(std::string& output, double value)
{
	std::uint64_t bits = 0u;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	AppendU64(output, bits);
}

std::uint64_t ReadU64FromBytes(const char* data)
{
	std::uint64_t value = 0u;
	for (std::uint32_t shift = 0u; shift < 64u; shift += 8u)
	{
		value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[shift / 8u])) << shift;
	}
	return value;
}

std::uint32_t ReadU32FromBytes(const char* data)
{
	std::uint32_t value = 0u;
	for (std::uint32_t shift = 0u; shift < 32u; shift += 8u)
	{
		value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[shift / 8u])) << shift;
	}
	return value;
}

double ReadDoubleFromBytes(const char* data)
{
	const std::uint64_t bits = ReadU64FromBytes(data);
	double value = 0.0;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

bool ConsumeU32(std::string_view& input, std::uint32_t& value)
{
	if (input.size() < 4u)
	{
		return false;
	}
	value = ReadU32FromBytes(input.data());
	input.remove_prefix(4u);
	return true;
}

bool ConsumeU64(std::string_view& input, std::uint64_t& value)
{
	if (input.size() < 8u)
	{
		return false;
	}
	value = ReadU64FromBytes(input.data());
	input.remove_prefix(8u);
	return true;
}

bool ConsumeDouble(std::string_view& input, double& value)
{
	if (input.size() < 8u)
	{
		return false;
	}
	value = ReadDoubleFromBytes(input.data());
	input.remove_prefix(8u);
	return std::isfinite(value) && value >= 0.0;
}

ReadState ReadExact(std::istream& input, char* destination, std::size_t count)
{
	input.read(destination, static_cast<std::streamsize>(count));
	if (input.gcount() == static_cast<std::streamsize>(count))
	{
		return ReadState::Ok;
	}
	return input.eof() && input.gcount() == 0 ? ReadState::End : ReadState::Partial;
}

class FilesystemProfilerJournalV1Sink final : public ProfilerJournalV1Sink
{
public:
	bool Open(const std::filesystem::path& path, std::string& outError) override
	{
		outError.clear();
		output.open(path, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			outError = "Failed to create profiler journal V1 artifact.";
			return false;
		}
		return true;
	}

	bool WriteAndFlush(std::string_view bytes, std::string& outError) override
	{
		if (!output.is_open())
		{
			outError = "Profiler journal V1 sink is not open.";
			return false;
		}
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if (!output)
		{
			outError = "Profiler journal V1 sink write failed.";
			return false;
		}
		return true;
	}

	bool IsOpen() const noexcept override
	{
		return output.is_open();
	}

private:
	std::ofstream output;
};

ReadState ReadU32(std::istream& input, std::uint32_t& value)
{
	std::array<char, 4u> bytes{};
	const ReadState state = ReadExact(input, bytes.data(), bytes.size());
	if (state == ReadState::Ok)
	{
		value = ReadU32FromBytes(bytes.data());
	}
	return state;
}

ReadState ReadU64(std::istream& input, std::uint64_t& value)
{
	std::array<char, 8u> bytes{};
	const ReadState state = ReadExact(input, bytes.data(), bytes.size());
	if (state == ReadState::Ok)
	{
		value = ReadU64FromBytes(bytes.data());
	}
	return state;
}

// CRC-64/ECMA detects accidental corruption. A predecessor digest is stored
// in every segment, making reordered or removed committed records fail closed.
// Generate the table from the format polynomial rather than embedding a magic
// constant blob: this keeps the on-disk checksum definition reviewable while
// avoiding eight branchy polynomial steps for every persisted byte.
constexpr std::array<std::uint64_t, 256u> MakeCrc64EcmaTable()
{
	std::array<std::uint64_t, 256u> table{};
	for (std::uint32_t ordinal = 0u; ordinal < table.size(); ++ordinal)
	{
		std::uint64_t crc = static_cast<std::uint64_t>(ordinal) << 56u;
		for (std::uint32_t bit = 0u; bit < 8u; ++bit)
		{
			crc = (crc & (1ull << 63u)) != 0u
				? (crc << 1u) ^ 0x42F0E1EBA9EA3693ull
				: crc << 1u;
		}
		table[ordinal] = crc;
	}
	return table;
}

constexpr std::array<std::uint64_t, 256u> kCrc64EcmaTable = MakeCrc64EcmaTable();

constexpr std::uint64_t Crc64Ecma(std::string_view bytes)
{
	std::uint64_t crc = 0u;
	for (const unsigned char byte : bytes)
	{
		const std::uint8_t tableIndex = static_cast<std::uint8_t>((crc >> 56u) ^ byte);
		crc = (crc << 8u) ^ kCrc64EcmaTable[tableIndex];
	}
	return crc;
}

// "123456789" is the published CRC-64/ECMA-182 check vector for an initial
// and final xor value of zero, which are the values used by this V1 format.
static_assert(Crc64Ecma("123456789") == 0x6C40DF5F0B497347ull);

std::uint64_t Crc64(std::string_view bytes)
{
	return Crc64Ecma(bytes);
}

std::uint64_t ComputeQuantileNanoseconds(
	const std::vector<std::uint64_t>& samples,
	double quantile)
{
	if (samples.empty())
	{
		return 0u;
	}
	std::vector<std::uint64_t> sorted = samples;
	std::sort(sorted.begin(), sorted.end());
	const double position = std::clamp(quantile, 0.0, 1.0)
		* static_cast<double>(sorted.size() - 1u);
	const std::size_t lower = static_cast<std::size_t>(std::floor(position));
	const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
	if (lower == upper)
	{
		return sorted[lower];
	}
	const double fraction = position - static_cast<double>(lower);
	return static_cast<std::uint64_t>(std::llround(
		static_cast<double>(sorted[lower]) * (1.0 - fraction)
		+ static_cast<double>(sorted[upper]) * fraction));
}

void CompleteEngineCpuWindowSummary(
	ProfilerJournalV1Summary& summary,
	const std::vector<std::uint64_t>& firstWindow,
	const std::deque<std::pair<double, std::uint64_t>>& finalWindow)
{
	std::vector<std::uint64_t> finalSamples;
	finalSamples.reserve(finalWindow.size());
	for (const auto& [frameStartMs, durationNanoseconds] : finalWindow)
	{
		(void)frameStartMs;
		finalSamples.push_back(durationNanoseconds);
	}
	summary.firstEngineCpuWindowSampleCount = firstWindow.size();
	summary.firstEngineCpuWindowTotalNanoseconds = std::accumulate(
		firstWindow.begin(), firstWindow.end(), std::uint64_t{0u});
	summary.firstEngineCpuWindowMedianNanoseconds = ComputeQuantileNanoseconds(firstWindow, 0.5);
	summary.firstEngineCpuWindowP95Nanoseconds = ComputeQuantileNanoseconds(firstWindow, 0.95);
	summary.finalEngineCpuWindowSampleCount = finalSamples.size();
	summary.finalEngineCpuWindowTotalNanoseconds = std::accumulate(
		finalSamples.begin(), finalSamples.end(), std::uint64_t{0u});
	summary.finalEngineCpuWindowMedianNanoseconds = ComputeQuantileNanoseconds(finalSamples, 0.5);
	summary.finalEngineCpuWindowP95Nanoseconds = ComputeQuantileNanoseconds(finalSamples, 0.95);
}

std::string SettingsToJsonText(const ProfilerCaptureMetadata& metadata)
{
	const ProfilerCaptureSettings& settings = metadata.settings;
	nlohmann::json value = {
		{"terraEngineVersion", metadata.terraEngineVersion},
		{"projectName", metadata.projectName},
		{"projectPathHint", metadata.projectPathHint},
		{"scenePath", metadata.scenePath},
		{"startedAt", metadata.startedAt},
		{"savedAt", metadata.savedAt},
		{"detailLevel", static_cast<std::uint8_t>(settings.detailLevel)},
		{"sampleIntervalFrames", settings.sampleIntervalFrames},
		{"maximumChunkFrames", settings.maximumChunkFrames},
		{"maximumChunkBytes", settings.maximumChunkBytes},
		{"residentByteBudget", settings.residentByteBudget},
		{"maximumPendingBytes", settings.maximumPendingBytes},
		{"maximumSealedFrameBytes", settings.maximumSealedFrameBytes}};
	return value.dump();
}

bool MetadataFromJsonText(std::string_view text, ProfilerCaptureMetadata& metadata, std::string& error)
{
	try
	{
		const nlohmann::json value = nlohmann::json::parse(text);
		if (!value.is_object())
		{
			error = "Profiler journal V1 header metadata is not an object.";
			return false;
		}
		metadata.terraEngineVersion = value.value("terraEngineVersion", "unknown");
		metadata.projectName = value.value("projectName", "");
		metadata.projectPathHint = value.value("projectPathHint", "");
		metadata.scenePath = value.value("scenePath", "");
		metadata.startedAt = value.value("startedAt", "");
		metadata.savedAt = value.value("savedAt", "");
		metadata.settings.detailLevel = static_cast<ProfilerCaptureDetailLevel>(
			value.value("detailLevel", static_cast<std::uint8_t>(ProfilerCaptureDetailLevel::Detailed)));
		metadata.settings.sampleIntervalFrames = std::max(1u, value.value("sampleIntervalFrames", 1u));
		metadata.settings.maximumChunkFrames = std::max(1u, value.value("maximumChunkFrames", 1u));
		metadata.settings.maximumChunkBytes = std::max<std::uint64_t>(1024u,
			value.value("maximumChunkBytes", 8u * 1024u * 1024u));
		metadata.settings.residentByteBudget = value.value("residentByteBudget", 256u * 1024u * 1024u);
		metadata.settings.maximumPendingBytes = value.value("maximumPendingBytes", 256u * 1024u * 1024u);
		metadata.settings.maximumSealedFrameBytes = value.value("maximumSealedFrameBytes", 2u * 1024u * 1024u);
		if (metadata.settings.residentByteBudget == 0u
			|| metadata.settings.maximumPendingBytes == 0u
			|| metadata.settings.maximumSealedFrameBytes == 0u)
		{
			error = "Profiler journal V1 header has an unbounded capture setting.";
			return false;
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		error = exception.what();
		return false;
	}
}

bool ReadHeader(std::istream& input, ProfilerCaptureMetadata& metadata, std::string& error)
{
	std::array<char, kJournalMagic.size()> magic{};
	if (ReadExact(input, magic.data(), magic.size()) != ReadState::Ok || magic != kJournalMagic)
	{
		error = "Unsupported profiler capture: expected TerraEngineProfilerJournalV1.";
		return false;
	}
	std::uint32_t schema = 0u;
	std::uint32_t metadataBytes = 0u;
	std::uint64_t checksum = 0u;
	if (ReadU32(input, schema) != ReadState::Ok || ReadU32(input, metadataBytes) != ReadState::Ok
		|| ReadU64(input, checksum) != ReadState::Ok || schema != kProfilerJournalV1SchemaVersion
		|| metadataBytes == 0u || metadataBytes > kMaximumJournalRecordBytes)
	{
		error = "Profiler journal V1 header is malformed.";
		return false;
	}
	std::string metadataText(metadataBytes, '\0');
	if (ReadExact(input, metadataText.data(), metadataText.size()) != ReadState::Ok
		|| Crc64(metadataText) != checksum)
	{
		error = "Profiler journal V1 header integrity check failed.";
		return false;
	}
	return MetadataFromJsonText(metadataText, metadata, error);
}

void AppendLoss(std::string& payload, const ProfilerCaptureLossRange& loss)
{
	AppendU64(payload, loss.firstFrameIndex);
	AppendU64(payload, loss.lastFrameIndex);
	payload.push_back(static_cast<char>(loss.lossClass));
	payload.push_back(static_cast<char>(loss.reason));
	AppendU64(payload, loss.discardedRecordCount);
	AppendU64(payload, loss.discardedByteCount);
}

bool ConsumeLoss(std::string_view& payload, ProfilerCaptureLossRange& loss)
{
	if (!ConsumeU64(payload, loss.firstFrameIndex) || !ConsumeU64(payload, loss.lastFrameIndex)
		|| payload.size() < 2u)
	{
		return false;
	}
	loss.lossClass = static_cast<ProfilerCaptureLossClass>(static_cast<unsigned char>(payload[0]));
	loss.reason = static_cast<ProfilerCaptureLossReason>(static_cast<unsigned char>(payload[1]));
	payload.remove_prefix(2u);
	return ConsumeU64(payload, loss.discardedRecordCount)
		&& ConsumeU64(payload, loss.discardedByteCount);
}

bool BuildSegmentPayload(
	std::span<const ProfilerSealedFrame> frames,
	const ProfilerCaptureSettings& settings,
	std::string& outPayload,
	ProfilerJournalV1Summary& summaryDelta,
	std::string& error)
{
	outPayload.clear();
	summaryDelta = {};
	std::uint64_t previousFrame = 0u;
	bool havePrevious = false;
	// One segment can hold many frames. Reusing this bounded scratch buffer keeps
	// the persistence worker from allocating once per detailed frame, while each
	// encoded value is still copied into the immutable segment payload below.
	std::string encodedFrame;
	for (const ProfilerSealedFrame& sealed : frames)
	{
		if (!ValidateProfilerSealedFrame(sealed, error)
			|| sealed.ownedByteCount > settings.maximumSealedFrameBytes
			|| (havePrevious && sealed.frame.frameIndex <= previousFrame))
		{
			if (error.empty())
			{
				error = "Profiler journal V1 segment has an oversized or out-of-order frame.";
			}
			return false;
		}
		if (!EncodeProfilerJournalFrame(sealed.frame, settings, encodedFrame, error)
			|| encodedFrame.size() > kMaximumJournalRecordBytes)
		{
			return false;
		}
		AppendU64(outPayload, sealed.frame.frameIndex);
		AppendU64(outPayload, sealed.ownedByteCount);
		AppendU32(outPayload, static_cast<std::uint32_t>(sealed.losses.size()));
		for (const ProfilerCaptureLossRange& loss : sealed.losses)
		{
			AppendLoss(outPayload, loss);
			summaryDelta.droppedDetailRecordCount += loss.discardedRecordCount;
			summaryDelta.droppedDetailByteCount += loss.discardedByteCount;
		}
		AppendU64(outPayload, static_cast<std::uint64_t>(encodedFrame.size()));
		outPayload.append(encodedFrame);
		++summaryDelta.frameCount;
		summaryDelta.completeFrameCount += sealed.frame.complete ? 1u : 0u;
		summaryDelta.totalDurationNanoseconds += static_cast<std::uint64_t>(
			std::llround(sealed.frame.durationMs * 1000000.0));
		for (const ProfilerTimingStreamSample& stream : sealed.frame.timingStreams)
		{
			if (stream.scope == ProfilerTimingStreamScope::EngineCpu
				&& stream.status == ProfilerTimingStreamStatus::Valid)
			{
				++summaryDelta.engineCpuSampleCount;
				summaryDelta.engineCpuTotalNanoseconds += static_cast<std::uint64_t>(
					std::llround(stream.milliseconds * 1000000.0));
				break;
			}
		}
		// Module rows are already compact scalar evidence in each sealed frame.
		// Fold them here, while the writer owns one bounded segment, so Terminal
		// can answer subsystem summary requests without a history-sized rescan.
		for (const ProfilerModuleSummary& module : sealed.frame.moduleSummaries)
		{
			const std::size_t moduleOrdinal = static_cast<std::size_t>(module.moduleId);
			if (module.available && moduleOrdinal < summaryDelta.subsystemSummaries.size())
			{
				ProfilerJournalV1SubsystemSummary& subsystem =
					summaryDelta.subsystemSummaries[moduleOrdinal];
				++subsystem.availableFrameCount;
				subsystem.totalNanoseconds += static_cast<std::uint64_t>(
					std::llround(module.totalMs * 1000000.0));
			}
		}
		previousFrame = sealed.frame.frameIndex;
		havePrevious = true;
	}
	return true;
}

bool DecodeSegmentPayload(
	std::string_view payload,
	std::uint32_t expectedFrameCount,
	std::uint64_t segmentOffset,
	bool decodeFrames,
	ProfilerCaptureDocument* document,
	const std::optional<std::uint64_t>& selectedFrameIndex,
	ProfilerJournalV1OpenResult& result,
	ProfilerJournalV1Summary& summaryDelta,
	std::string& error)
{
	summaryDelta = {};
	std::uint64_t previousFrame = 0u;
	bool havePrevious = false;
	for (std::uint32_t frameOrdinal = 0u; frameOrdinal < expectedFrameCount; ++frameOrdinal)
	{
		std::uint64_t frameIndex = 0u;
		std::uint64_t ownedBytes = 0u;
		std::uint32_t lossCount = 0u;
		if (!ConsumeU64(payload, frameIndex) || !ConsumeU64(payload, ownedBytes)
			|| !ConsumeU32(payload, lossCount) || lossCount > 4096u
			|| (havePrevious && frameIndex <= previousFrame))
		{
			error = "Profiler journal V1 segment has malformed frame ordering.";
			return false;
		}
		std::vector<ProfilerCaptureLossRange> losses;
		losses.reserve(lossCount);
		for (std::uint32_t lossOrdinal = 0u; lossOrdinal < lossCount; ++lossOrdinal)
		{
			ProfilerCaptureLossRange loss;
			if (!ConsumeLoss(payload, loss) || loss.firstFrameIndex != frameIndex
				|| loss.lastFrameIndex != frameIndex
				|| loss.lossClass == ProfilerCaptureLossClass::None
				|| loss.reason == ProfilerCaptureLossReason::None)
			{
				error = "Profiler journal V1 segment has malformed loss evidence.";
				return false;
			}
			losses.push_back(loss);
			summaryDelta.droppedDetailRecordCount += loss.discardedRecordCount;
			summaryDelta.droppedDetailByteCount += loss.discardedByteCount;
		}
		std::uint64_t encodedBytes = 0u;
		if (!ConsumeU64(payload, encodedBytes) || encodedBytes > payload.size()
			|| encodedBytes > kMaximumJournalRecordBytes)
		{
			error = "Profiler journal V1 segment has an invalid frame length.";
			return false;
		}
		const std::string_view encodedFrame = payload.substr(0u, static_cast<std::size_t>(encodedBytes));
		payload.remove_prefix(static_cast<std::size_t>(encodedBytes));
		// Index-only reads still decode one bounded frame at a time to obtain its
		// authoritative duration/completion state.  The temporary is released
		// before the next record, so catalog memory never grows with history.
		ProfilerFrameRecord frame;
		if (!DecodeProfilerJournalFrame(encodedFrame, frame, error))
		{
			return false;
		}
		if (frame.frameIndex != frameIndex || ownedBytes == 0u)
		{
			error = "Profiler journal V1 frame identity or byte envelope is invalid.";
			return false;
		}
		// Writer-side validation proves the exact owned-byte count before commit.
		// A decoded value may materialize fixed optional wrappers that were encoded
		// compactly, so comparing its C++ representation to the sealed allocation
		// total would reject valid journals without improving byte-stream safety.
		const double durationMs = frame.durationMs;
		const bool complete = frame.complete;
		for (const ProfilerTimingStreamSample& stream : frame.timingStreams)
		{
			if (stream.scope == ProfilerTimingStreamScope::EngineCpu
				&& stream.status == ProfilerTimingStreamStatus::Valid)
			{
				++summaryDelta.engineCpuSampleCount;
				summaryDelta.engineCpuTotalNanoseconds += static_cast<std::uint64_t>(
					std::llround(stream.milliseconds * 1000000.0));
				break;
			}
		}
		for (const ProfilerModuleSummary& module : frame.moduleSummaries)
		{
			const std::size_t moduleOrdinal = static_cast<std::size_t>(module.moduleId);
			if (module.available && moduleOrdinal < summaryDelta.subsystemSummaries.size())
			{
				ProfilerJournalV1SubsystemSummary& subsystem =
					summaryDelta.subsystemSummaries[moduleOrdinal];
				++subsystem.availableFrameCount;
				subsystem.totalNanoseconds += static_cast<std::uint64_t>(
					std::llround(module.totalMs * 1000000.0));
			}
		}
		if (decodeFrames && document != nullptr
			&& (!selectedFrameIndex.has_value() || frameIndex == *selectedFrameIndex))
		{
			document->frames.push_back(std::move(frame));
		}
		result.index.push_back({frameIndex, durationMs, complete, segmentOffset});
		++summaryDelta.frameCount;
		summaryDelta.completeFrameCount += complete ? 1u : 0u;
		summaryDelta.totalDurationNanoseconds += static_cast<std::uint64_t>(
			std::llround(durationMs * 1000000.0));
		previousFrame = frameIndex;
		havePrevious = true;
	}
	if (!payload.empty())
	{
		error = "Profiler journal V1 segment has trailing frame bytes.";
		return false;
	}
	return true;
}

void MergeSummary(ProfilerJournalV1Summary& destination, const ProfilerJournalV1Summary& delta)
{
	destination.frameCount += delta.frameCount;
	destination.completeFrameCount += delta.completeFrameCount;
	destination.totalDurationNanoseconds += delta.totalDurationNanoseconds;
	destination.droppedDetailRecordCount += delta.droppedDetailRecordCount;
	destination.droppedDetailByteCount += delta.droppedDetailByteCount;
	destination.committedSegmentCount += delta.committedSegmentCount;
	destination.engineCpuSampleCount += delta.engineCpuSampleCount;
	destination.engineCpuTotalNanoseconds += delta.engineCpuTotalNanoseconds;
	for (std::size_t moduleOrdinal = 0u;
		moduleOrdinal < destination.subsystemSummaries.size(); ++moduleOrdinal)
	{
		destination.subsystemSummaries[moduleOrdinal].availableFrameCount +=
			delta.subsystemSummaries[moduleOrdinal].availableFrameCount;
		destination.subsystemSummaries[moduleOrdinal].totalNanoseconds +=
			delta.subsystemSummaries[moduleOrdinal].totalNanoseconds;
	}
	destination.persistenceLagHighWaterFrames = std::max(
		destination.persistenceLagHighWaterFrames, delta.persistenceLagHighWaterFrames);
}

struct PendingSegmentDirectory
{
	std::uint64_t offset = 0u;
	std::uint64_t firstFrame = 0u;
	std::uint64_t lastFrame = 0u;
	std::uint32_t frameCount = 0u;
	std::uint64_t digest = 0u;
	std::size_t decodedIndexOffset = 0u;
	bool decoded = false;
};

struct JournalReadState
{
	ProfilerCaptureMetadata metadata;
	ProfilerJournalV1OpenResult result;
	std::uint64_t expectedSequence = 0u;
	std::uint64_t previousDigest = 0u;
	std::uint64_t previousFrameIndex = 0u;
	std::uint64_t committedFrameCount = 0u;
	std::vector<PendingSegmentDirectory> pendingSegments;
	std::uint64_t lastCheckpointOffset = 0u;
	std::uint64_t checkpointPageCount = 0u;
	bool haveFrame = false;
};

struct IndexPageSelection
{
	std::uint64_t offset = 0u;
	std::uint32_t pageSize = 0u;
	std::uint64_t seenEntries = 0u;
};

void RetainIndexPageEntries(
	std::span<const ProfilerJournalV1IndexEntry> entries,
	IndexPageSelection& selection,
	std::vector<ProfilerJournalV1IndexEntry>& destination)
{
	if (selection.pageSize == 0u)
	{
		selection.seenEntries += entries.size();
		return;
	}
	if (selection.offset == kProfilerJournalV1NewestIndexPage)
	{
		destination.insert(destination.end(), entries.begin(), entries.end());
		if (destination.size() > selection.pageSize)
		{
			destination.erase(destination.begin(),
				destination.end() - static_cast<std::ptrdiff_t>(selection.pageSize));
		}
		selection.seenEntries += entries.size();
		return;
	}
	const std::uint64_t requestEnd = selection.offset + selection.pageSize;
	for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal)
	{
		const std::uint64_t globalOrdinal = selection.seenEntries + ordinal;
		if (globalOrdinal >= selection.offset && globalOrdinal < requestEnd)
		{
			destination.push_back(entries[ordinal]);
		}
	}
	selection.seenEntries += entries.size();
}

struct TerminalDirectoryAnchor
{
	std::uint64_t nextSequence = 0u;
	std::uint64_t frameCount = 0u;
	std::uint64_t lastCheckpointOffset = 0u;
	std::uint64_t checkpointPageCount = 0u;
	std::uint64_t previousDigest = 0u;
};

struct CheckpointLookup
{
	std::uint64_t nextSequence = 0u;
	std::uint64_t frameCount = 0u;
	std::uint64_t firstFrame = 0u;
	std::uint64_t lastFrame = 0u;
	std::uint64_t predecessor = 0u;
	std::uint64_t previousCheckpointOffset = 0u;
	std::uint32_t segmentCount = 0u;
	std::uint32_t entryCount = 0u;
	std::optional<ProfilerJournalV1IndexEntry> selectedEntry;
};

// A completed journal has exactly one fixed-size END1 record at EOF. This
// helper reads only that record, verifies its checksum, and exposes the sparse
// directory anchor needed by a selected-frame lookup.
ProfilerJournalV1ResultCode ReadTerminalDirectoryAnchor(
	std::istream& input,
	TerminalDirectoryAnchor& outAnchor,
	std::string& outError)
{
	input.clear();
	input.seekg(0, std::ios::end);
	const std::streamoff endOffset = input.tellg();
	if (endOffset < static_cast<std::streamoff>(kTerminalRecordBytes))
	{
		outError = "Profiler journal V1 has no complete terminal record.";
		return ProfilerJournalV1ResultCode::IncompleteTail;
	}
	input.seekg(endOffset - static_cast<std::streamoff>(kTerminalRecordBytes), std::ios::beg);
	std::string terminal(kTerminalRecordBytes, '\0');
	if (ReadExact(input, terminal.data(), terminal.size()) != ReadState::Ok)
	{
		outError = "Profiler journal V1 terminal record is truncated.";
		return ProfilerJournalV1ResultCode::IncompleteTail;
	}
	if (!std::equal(kTerminalMagic.begin(), kTerminalMagic.end(), terminal.begin()))
	{
		outError = "Profiler journal V1 has no complete terminal record.";
		return ProfilerJournalV1ResultCode::IncompleteTail;
	}
	const std::uint64_t storedChecksum = ReadU64FromBytes(
		terminal.data() + terminal.size() - sizeof(std::uint64_t));
	const std::string_view terminalBytes(terminal.data(),
		terminal.size() - sizeof(std::uint64_t));
	if (Crc64(terminalBytes) != storedChecksum)
	{
		outError = "Profiler journal V1 terminal checksum is invalid.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	std::string_view fields = terminalBytes.substr(kTerminalMagic.size());
	auto consume = [&fields](std::uint64_t& value) { return ConsumeU64(fields, value); };
	std::uint64_t ignored = 0u;
	if (!consume(outAnchor.nextSequence) || !consume(outAnchor.frameCount))
	{
		outError = "Profiler journal V1 terminal fields are truncated.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	for (std::size_t ordinal = 0u; ordinal < 8u; ++ordinal)
	{
		if (!consume(ignored))
		{
			outError = "Profiler journal V1 terminal scalar fields are truncated.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
	}
	// Module totals, timing windows, profiler-self totals, persistence totals,
	// and batching totals have fixed slots before the directory anchor.
	constexpr std::size_t kFieldsBeforeDirectoryAnchor =
		(kProfilerJournalV1SubsystemCount * 2u) + 4u + 4u + 5u + 3u + 3u;
	for (std::size_t ordinal = 0u; ordinal < kFieldsBeforeDirectoryAnchor; ++ordinal)
	{
		if (!consume(ignored))
		{
			outError = "Profiler journal V1 terminal summary fields are truncated.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
	}
	if (!consume(outAnchor.lastCheckpointOffset) || !consume(outAnchor.checkpointPageCount)
		|| !consume(outAnchor.previousDigest) || fields.size() != 1u
		|| (fields.front() != '\0' && fields.front() != '\1'))
	{
		outError = "Profiler journal V1 terminal directory anchor is invalid.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	return ProfilerJournalV1ResultCode::Ok;
}

// Read one IDX1 page in place. The checksum covers every compact row, so this
// path walks the directory without allocating a history-sized frame index.
ProfilerJournalV1ResultCode ReadCheckpointLookup(
	std::istream& input,
	const ProfilerCaptureMetadata& metadata,
	std::uint64_t checkpointOffset,
	std::uint64_t selectedFrameIndex,
	CheckpointLookup& outLookup,
	std::string& outError)
{
	outLookup = {};
	input.clear();
	input.seekg(static_cast<std::streamoff>(checkpointOffset), std::ios::beg);
	std::array<char, 4u> marker{};
	if (ReadExact(input, marker.data(), marker.size()) != ReadState::Ok
		|| marker != kIndexCheckpointMagic)
	{
		outError = "Profiler journal V1 checkpoint offset does not point to IDX1.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	if (ReadU64(input, outLookup.nextSequence) != ReadState::Ok
		|| ReadU64(input, outLookup.frameCount) != ReadState::Ok
		|| ReadU64(input, outLookup.lastFrame) != ReadState::Ok
		|| ReadU64(input, outLookup.predecessor) != ReadState::Ok
		|| ReadU64(input, outLookup.previousCheckpointOffset) != ReadState::Ok
		|| ReadU32(input, outLookup.segmentCount) != ReadState::Ok
		|| ReadU32(input, outLookup.entryCount) != ReadState::Ok
		|| outLookup.segmentCount == 0u || outLookup.segmentCount > kCheckpointSegmentStride
		|| outLookup.entryCount == 0u
		|| outLookup.entryCount > static_cast<std::uint64_t>(
			metadata.settings.maximumChunkFrames) * kCheckpointSegmentStride)
	{
		outError = "Profiler journal V1 checkpoint header is invalid.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}

	std::string checksumBytes(kIndexCheckpointMagic.data(), kIndexCheckpointMagic.size());
	AppendU64(checksumBytes, outLookup.nextSequence); AppendU64(checksumBytes, outLookup.frameCount);
	AppendU64(checksumBytes, outLookup.lastFrame); AppendU64(checksumBytes, outLookup.predecessor);
	AppendU64(checksumBytes, outLookup.previousCheckpointOffset);
	AppendU32(checksumBytes, outLookup.segmentCount); AppendU32(checksumBytes, outLookup.entryCount);
	std::uint64_t previousFrame = 0u;
	for (std::uint32_t ordinal = 0u; ordinal < outLookup.entryCount; ++ordinal)
	{
		std::uint64_t candidateFrameIndex = 0u;
		std::uint64_t durationNanoseconds = 0u;
		std::uint64_t segmentOffset = 0u;
		std::uint64_t storedDigest = 0u;
		std::array<char, 1u> completeByte{};
		if (ReadU64(input, candidateFrameIndex) != ReadState::Ok
			|| ReadU64(input, durationNanoseconds) != ReadState::Ok
			|| ReadExact(input, completeByte.data(), completeByte.size()) != ReadState::Ok
			|| ReadU64(input, segmentOffset) != ReadState::Ok
			|| ReadU64(input, storedDigest) != ReadState::Ok
			|| (ordinal != 0u && candidateFrameIndex <= previousFrame)
			|| (completeByte[0] != '\0' && completeByte[0] != '\1'))
		{
			outError = "Profiler journal V1 checkpoint entry is invalid.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		AppendU64(checksumBytes, candidateFrameIndex); AppendU64(checksumBytes, durationNanoseconds);
		checksumBytes.push_back(completeByte[0]); AppendU64(checksumBytes, segmentOffset);
		AppendU64(checksumBytes, storedDigest);
		if (ordinal == 0u)
		{
			outLookup.firstFrame = candidateFrameIndex;
		}
		if (candidateFrameIndex == selectedFrameIndex)
		{
			outLookup.selectedEntry = ProfilerJournalV1IndexEntry{candidateFrameIndex,
				static_cast<double>(durationNanoseconds) / 1000000.0,
				completeByte[0] != 0, segmentOffset, storedDigest};
		}
		previousFrame = candidateFrameIndex;
	}
	std::uint64_t checksum = 0u;
	if (ReadU64(input, checksum) != ReadState::Ok || checksum != Crc64(checksumBytes)
		|| previousFrame != outLookup.lastFrame)
	{
		outError = "Profiler journal V1 checkpoint checksum or frame range is invalid.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	if (selectedFrameIndex >= outLookup.firstFrame && selectedFrameIndex <= outLookup.lastFrame
		&& !outLookup.selectedEntry.has_value())
	{
		outError = "Profiler journal V1 checkpoint omits a frame inside its declared range.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	return ProfilerJournalV1ResultCode::Ok;
}

// The selected SEG1 is the only detailed payload this path reads. Its durable
// CMT1 digest must agree with the compact IDX1 row before decompression, so a
// corrupt or redirected directory entry cannot expose unrelated frame data.
ProfilerJournalV1ResultCode ReadSelectedCommittedSegment(
	std::istream& input,
	const ProfilerCaptureMetadata& metadata,
	const ProfilerJournalV1IndexEntry& selectedEntry,
	ProfilerFrameRecord& outFrame,
	std::string& outError)
{
	input.clear();
	input.seekg(static_cast<std::streamoff>(selectedEntry.segmentOffset), std::ios::beg);
	std::array<char, 4u> segmentMagic{};
	std::uint64_t sequence = 0u;
	std::uint64_t firstFrame = 0u;
	std::uint64_t lastFrame = 0u;
	std::uint32_t frameCount = 0u;
	std::array<char, 1u> codecByte{};
	std::uint64_t uncompressedBytes = 0u;
	std::uint64_t storedBytes = 0u;
	std::uint64_t predecessor = 0u;
	if (ReadExact(input, segmentMagic.data(), segmentMagic.size()) != ReadState::Ok
		|| ReadU64(input, sequence) != ReadState::Ok || ReadU64(input, firstFrame) != ReadState::Ok
		|| ReadU64(input, lastFrame) != ReadState::Ok || ReadU32(input, frameCount) != ReadState::Ok
		|| ReadExact(input, codecByte.data(), codecByte.size()) != ReadState::Ok
		|| ReadU64(input, uncompressedBytes) != ReadState::Ok || ReadU64(input, storedBytes) != ReadState::Ok
		|| ReadU64(input, predecessor) != ReadState::Ok)
	{
		outError = "Profiler journal V1 selected segment header is truncated.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	const std::uint64_t maximumStoredBytes = std::min<std::uint64_t>(
		kMaximumJournalRecordBytes, ZSTD_compressBound(metadata.settings.maximumChunkBytes));
	if (segmentMagic != kSegmentMagic || frameCount == 0u || firstFrame > lastFrame
		|| selectedEntry.frameIndex < firstFrame || selectedEntry.frameIndex > lastFrame
		|| codecByte[0] != static_cast<char>(SegmentCodec::Zstd)
		|| uncompressedBytes == 0u || uncompressedBytes > metadata.settings.maximumChunkBytes
		|| storedBytes == 0u || storedBytes > maximumStoredBytes)
	{
		outError = "Profiler journal V1 selected segment header is invalid.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	std::string storedPayload(static_cast<std::size_t>(storedBytes), '\0');
	std::array<char, 4u> commitMagic{};
	std::uint64_t committedSequence = 0u;
	std::uint64_t digest = 0u;
	std::uint64_t committedFrames = 0u;
	if (ReadExact(input, storedPayload.data(), storedPayload.size()) != ReadState::Ok
		|| ReadExact(input, commitMagic.data(), commitMagic.size()) != ReadState::Ok
		|| ReadU64(input, committedSequence) != ReadState::Ok || ReadU64(input, digest) != ReadState::Ok
		|| ReadU64(input, committedFrames) != ReadState::Ok)
	{
		outError = "Profiler journal V1 selected segment payload is truncated.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	std::string digestBytes(kSegmentMagic.data(), kSegmentMagic.size());
	AppendU64(digestBytes, sequence); AppendU64(digestBytes, firstFrame); AppendU64(digestBytes, lastFrame);
	AppendU32(digestBytes, frameCount); digestBytes.push_back(codecByte[0]);
	AppendU64(digestBytes, uncompressedBytes); AppendU64(digestBytes, storedBytes);
	AppendU64(digestBytes, predecessor); digestBytes.append(storedPayload);
	if (commitMagic != kCommitMagic || committedSequence != sequence || committedFrames != frameCount
		|| digest != selectedEntry.storedPayloadDigest || Crc64(digestBytes) != digest)
	{
		outError = "Profiler journal V1 selected segment digest disagrees with IDX1.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	std::string payload;
	if (!DecompressSegment(storedPayload, uncompressedBytes, payload, outError))
	{
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	ProfilerCaptureDocument selectedDocument;
	ProfilerJournalV1OpenResult selectedResult;
	ProfilerJournalV1Summary delta;
	const std::optional<std::uint64_t> selectedFrameIndex = selectedEntry.frameIndex;
	if (!DecodeSegmentPayload(payload, frameCount, selectedEntry.segmentOffset, true,
		&selectedDocument, selectedFrameIndex, selectedResult, delta, outError)
		|| selectedDocument.frames.size() != 1u
		|| selectedDocument.frames.front().frameIndex != selectedEntry.frameIndex)
	{
		if (outError.empty())
		{
			outError = "Profiler journal V1 selected frame is missing from its segment.";
		}
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	const auto decoded = std::find_if(selectedResult.index.begin(), selectedResult.index.end(),
		[&selectedEntry](const ProfilerJournalV1IndexEntry& entry) {
			return entry.frameIndex == selectedEntry.frameIndex;
		});
	if (decoded == selectedResult.index.end() || decoded->complete != selectedEntry.complete
		|| std::abs(decoded->durationMs - selectedEntry.durationMs) > 0.000001)
	{
		outError = "Profiler journal V1 selected frame disagrees with IDX1.";
		return ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	outFrame = std::move(selectedDocument.frames.front());
	return ProfilerJournalV1ResultCode::Ok;
}

ProfilerJournalV1ResultCode ReadProfilerJournalV1SelectedFrameDirect(
	const std::filesystem::path& path,
	std::uint64_t frameIndex,
	ProfilerFrameRecord& outFrame,
	std::string& outError)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		outError = "Failed to open profiler journal V1.";
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	ProfilerCaptureMetadata metadata;
	if (!ReadHeader(input, metadata, outError))
	{
		return outError.starts_with("Unsupported")
			? ProfilerJournalV1ResultCode::UnsupportedFormat
			: ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	TerminalDirectoryAnchor anchor;
	const ProfilerJournalV1ResultCode anchorResult =
		ReadTerminalDirectoryAnchor(input, anchor, outError);
	if (anchorResult != ProfilerJournalV1ResultCode::Ok)
	{
		return anchorResult;
	}
	if (anchor.frameCount == 0u || anchor.lastCheckpointOffset == 0u
		|| anchor.checkpointPageCount == 0u)
	{
		outError = "The requested profiler journal frame was not committed.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}

	std::uint64_t currentCheckpointOffset = anchor.lastCheckpointOffset;
	std::uint64_t visitedPageCount = 0u;
	std::uint64_t newerSequence = anchor.nextSequence;
	std::uint64_t newerFrameCount = anchor.frameCount;
	std::uint64_t newerFirstFrame = std::numeric_limits<std::uint64_t>::max();
	std::uint32_t newerSegmentCount = 0u;
	std::uint32_t newerEntryCount = 0u;
	std::optional<ProfilerJournalV1IndexEntry> selectedEntry;
	while (currentCheckpointOffset != 0u)
	{
		if (visitedPageCount >= anchor.checkpointPageCount)
		{
			outError = "Profiler journal V1 checkpoint chain is cyclic or longer than Terminal.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		CheckpointLookup page;
		const ProfilerJournalV1ResultCode pageResult = ReadCheckpointLookup(input, metadata,
			currentCheckpointOffset, frameIndex, page, outError);
		if (pageResult != ProfilerJournalV1ResultCode::Ok)
		{
			return pageResult;
		}
		if (visitedPageCount == 0u)
		{
			if (page.nextSequence != anchor.nextSequence || page.frameCount != anchor.frameCount
				|| page.predecessor != anchor.previousDigest)
			{
				outError = "Profiler journal V1 Terminal does not anchor its newest IDX1 page.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
		}
		else if (page.nextSequence + newerSegmentCount != newerSequence
			|| page.frameCount + newerEntryCount != newerFrameCount
			|| page.lastFrame >= newerFirstFrame)
		{
			outError = "Profiler journal V1 checkpoint pages are not a contiguous ordered chain.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		if (page.previousCheckpointOffset >= currentCheckpointOffset)
		{
			outError = "Profiler journal V1 checkpoint page points forward or to itself.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		if (page.selectedEntry.has_value())
		{
			selectedEntry = page.selectedEntry;
		}
		newerSequence = page.nextSequence;
		newerFrameCount = page.frameCount;
		newerFirstFrame = page.firstFrame;
		newerSegmentCount = page.segmentCount;
		newerEntryCount = page.entryCount;
		currentCheckpointOffset = page.previousCheckpointOffset;
		++visitedPageCount;
	}
	if (visitedPageCount != anchor.checkpointPageCount || !selectedEntry.has_value())
	{
		outError = "The requested profiler journal frame was not committed.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	return ReadSelectedCommittedSegment(input, metadata, *selectedEntry, outFrame, outError);
}

ProfilerJournalV1ResultCode ReadJournal(
	const std::filesystem::path& path,
	ProfilerCaptureDocument* document,
	const std::optional<std::uint64_t>& selectedFrameIndex,
	IndexPageSelection* indexPageSelection,
	ProfilerJournalV1OpenResult& outResult,
	std::string& outError)
{
	outError.clear();
	outResult = {};
	if (document != nullptr)
	{
		*document = {};
	}
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		outError = "Failed to open profiler journal V1.";
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	JournalReadState state;
	const bool targetedRead = selectedFrameIndex.has_value();
	const bool indexOnlyRead = document == nullptr && !targetedRead;
	if (!ReadHeader(input, state.metadata, outError))
	{
		return outError.starts_with("Unsupported")
			? ProfilerJournalV1ResultCode::UnsupportedFormat
			: ProfilerJournalV1ResultCode::CorruptCommittedData;
	}
	state.result.metadata = state.metadata;
	if (document != nullptr)
	{
		document->metadata = state.metadata;
		document->metadata.settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;
		document->sourcePath = path;
		document->formatIdentifier = std::string(kProfilerJournalV1FormatIdentifier);
		document->formatVersion = kProfilerJournalV1SchemaVersion;
	}
	for (;;)
	{
		const std::uint64_t recordOffset = static_cast<std::uint64_t>(input.tellg());
		std::array<char, 4u> marker{};
		const ReadState markerState = ReadExact(input, marker.data(), marker.size());
		if (markerState == ReadState::End)
		{
			state.result.recoveredIncompleteTail = true;
			break;
		}
		if (markerState != ReadState::Ok)
		{
			state.result.recoveredIncompleteTail = true;
			break;
		}
		if (marker == kIndexCheckpointMagic)
		{
			std::uint64_t nextSequence = 0u;
			std::uint64_t frameCount = 0u;
			std::uint64_t lastFrame = 0u;
			std::uint64_t predecessor = 0u;
			std::uint64_t previousCheckpointOffset = 0u;
			std::uint32_t pageSegmentCount = 0u;
			std::uint32_t pageEntryCount = 0u;
			std::uint64_t checksum = 0u;
			if (ReadU64(input, nextSequence) != ReadState::Ok || ReadU64(input, frameCount) != ReadState::Ok
				|| ReadU64(input, lastFrame) != ReadState::Ok || ReadU64(input, predecessor) != ReadState::Ok
				|| ReadU64(input, previousCheckpointOffset) != ReadState::Ok
				|| ReadU32(input, pageSegmentCount) != ReadState::Ok
				|| ReadU32(input, pageEntryCount) != ReadState::Ok
				|| pageSegmentCount == 0u || pageSegmentCount > kCheckpointSegmentStride
				|| pageEntryCount == 0u
				|| pageEntryCount > static_cast<std::uint64_t>(
					state.metadata.settings.maximumChunkFrames) * kCheckpointSegmentStride)
			{
				state.result.recoveredIncompleteTail = true;
				break;
			}
			std::string checkpoint(kIndexCheckpointMagic.data(), kIndexCheckpointMagic.size());
			AppendU64(checkpoint, nextSequence); AppendU64(checkpoint, frameCount);
			AppendU64(checkpoint, lastFrame); AppendU64(checkpoint, predecessor);
			AppendU64(checkpoint, previousCheckpointOffset);
			AppendU32(checkpoint, pageSegmentCount);
			AppendU32(checkpoint, pageEntryCount);
			std::vector<ProfilerJournalV1IndexEntry> checkpointEntries;
			checkpointEntries.reserve(pageEntryCount);
			std::uint64_t checkpointPreviousFrame = 0u;
			for (std::uint32_t entryOrdinal = 0u; entryOrdinal < pageEntryCount; ++entryOrdinal)
			{
				std::uint64_t frameIndex = 0u;
				std::uint64_t durationNanoseconds = 0u;
				std::uint64_t segmentOffset = 0u;
				std::uint64_t storedDigest = 0u;
				std::array<char, 1u> completeByte{};
				if (ReadU64(input, frameIndex) != ReadState::Ok
					|| ReadU64(input, durationNanoseconds) != ReadState::Ok
					|| ReadExact(input, completeByte.data(), completeByte.size()) != ReadState::Ok
					|| ReadU64(input, segmentOffset) != ReadState::Ok || ReadU64(input, storedDigest) != ReadState::Ok
					|| completeByte[0] < '\0' || completeByte[0] > '\1'
					|| (entryOrdinal != 0u && frameIndex <= checkpointPreviousFrame))
				{
					state.result.recoveredIncompleteTail = true;
					break;
				}
				AppendU64(checkpoint, frameIndex);
				AppendU64(checkpoint, durationNanoseconds);
				checkpoint.push_back(completeByte[0]);
				AppendU64(checkpoint, segmentOffset); AppendU64(checkpoint, storedDigest);
				checkpointEntries.push_back({frameIndex,
					static_cast<double>(durationNanoseconds) / 1000000.0,
					completeByte[0] != 0,
					segmentOffset, storedDigest});
				checkpointPreviousFrame = frameIndex;
			}
			if (checkpointEntries.size() != pageEntryCount
				|| ReadU64(input, checksum) != ReadState::Ok)
			{
				state.result.recoveredIncompleteTail = true;
				break;
			}
			std::uint64_t pendingEntryCount = 0u;
			for (const PendingSegmentDirectory& pending : state.pendingSegments)
			{
				pendingEntryCount += pending.frameCount;
			}
			if (state.pendingSegments.empty() || nextSequence != state.expectedSequence
				|| frameCount != state.committedFrameCount || !state.haveFrame
				|| lastFrame != state.previousFrameIndex || predecessor != state.previousDigest
				|| previousCheckpointOffset != state.lastCheckpointOffset
				|| pageSegmentCount != state.pendingSegments.size()
				|| pageEntryCount != pendingEntryCount
				|| checkpointEntries.front().frameIndex != state.pendingSegments.front().firstFrame
				|| checkpointEntries.back().frameIndex != state.pendingSegments.back().lastFrame
				|| Crc64(checkpoint) != checksum)
			{
				outError = "Profiler journal V1 index checkpoint disagrees with committed evidence.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			std::size_t entryOffset = 0u;
			for (const PendingSegmentDirectory& pending : state.pendingSegments)
			{
				if (entryOffset + pending.frameCount > checkpointEntries.size())
				{
					outError = "Profiler journal V1 checkpoint segment range is truncated.";
					return ProfilerJournalV1ResultCode::CorruptCommittedData;
				}
				for (std::size_t ordinal = 0u; ordinal < pending.frameCount; ++ordinal)
				{
					const ProfilerJournalV1IndexEntry& compact = checkpointEntries[entryOffset + ordinal];
					if (compact.segmentOffset != pending.offset
						|| compact.storedPayloadDigest != pending.digest
						|| (ordinal == 0u && compact.frameIndex != pending.firstFrame)
						|| (ordinal + 1u == pending.frameCount && compact.frameIndex != pending.lastFrame))
					{
						outError = "Profiler journal V1 checkpoint points at the wrong committed segment.";
						return ProfilerJournalV1ResultCode::CorruptCommittedData;
					}
					if (pending.decoded)
					{
						if (pending.decodedIndexOffset + ordinal >= state.result.index.size())
						{
							outError = "Profiler journal V1 checkpoint is missing decoded frame evidence.";
							return ProfilerJournalV1ResultCode::CorruptCommittedData;
						}
						const ProfilerJournalV1IndexEntry& decoded =
							state.result.index[pending.decodedIndexOffset + ordinal];
						if (decoded.frameIndex != compact.frameIndex || decoded.complete != compact.complete
							|| std::abs(decoded.durationMs - compact.durationMs) > 0.000001)
						{
							outError = "Profiler journal V1 compact index disagrees with decoded frame evidence.";
							return ProfilerJournalV1ResultCode::CorruptCommittedData;
						}
					}
				}
				entryOffset += pending.frameCount;
			}
			if (indexOnlyRead)
			{
				for (const ProfilerJournalV1IndexEntry& entry : checkpointEntries)
				{
					state.result.summary.completeFrameCount += entry.complete ? 1u : 0u;
					state.result.summary.totalDurationNanoseconds += static_cast<std::uint64_t>(
						std::llround(entry.durationMs * 1000000.0));
				}
				if (indexPageSelection)
				{
					RetainIndexPageEntries(checkpointEntries, *indexPageSelection,
						state.result.index);
				}
				else
				{
					state.result.index.insert(state.result.index.end(),
						checkpointEntries.begin(), checkpointEntries.end());
				}
			}
			state.lastCheckpointOffset = recordOffset;
			++state.checkpointPageCount;
			state.pendingSegments.clear();
			continue;
		}
		if (marker == kTerminalMagic)
		{
			std::uint64_t sequence = 0u;
			std::uint64_t frameCount = 0u;
			std::uint64_t completeFrameCount = 0u;
			std::uint64_t totalDuration = 0u;
			std::uint64_t droppedRecords = 0u;
			std::uint64_t droppedBytes = 0u;
			std::uint64_t segmentCount = 0u;
			std::uint64_t engineCpuCount = 0u;
			std::uint64_t engineCpuTotal = 0u;
			std::array<ProfilerJournalV1SubsystemSummary,
				kProfilerJournalV1SubsystemCount> subsystemSummaries{};
			std::uint64_t lagHighWater = 0u;
			std::uint64_t firstWindowCount = 0u;
			std::uint64_t firstWindowTotal = 0u;
			std::uint64_t firstWindowMedian = 0u;
			std::uint64_t firstWindowP95 = 0u;
			std::uint64_t finalWindowCount = 0u;
			std::uint64_t finalWindowTotal = 0u;
			std::uint64_t finalWindowMedian = 0u;
			std::uint64_t finalWindowP95 = 0u;
			std::uint64_t profilerCollection = 0u;
			std::uint64_t profilerSealing = 0u;
			std::uint64_t profilerQueueing = 0u;
			std::uint64_t profilerPersistence = 0u;
			std::uint64_t profilerFinalization = 0u;
			std::uint64_t persistedBytes = 0u;
			std::uint64_t persistenceOperations = 0u;
			std::uint64_t persistenceThroughput = 0u;
			std::uint64_t logicalInputBytes = 0u;
			std::uint64_t encodedPayloadBytes = 0u;
			std::uint64_t storedSegmentBytes = 0u;
			std::uint64_t lastCheckpointOffset = 0u;
			std::uint64_t checkpointPageCount = 0u;
			std::uint64_t predecessor = 0u;
			std::uint64_t digest = 0u;
			std::array<char, 1u> completeByte{};
			if (ReadU64(input, sequence) != ReadState::Ok || ReadU64(input, frameCount) != ReadState::Ok
				|| ReadU64(input, completeFrameCount) != ReadState::Ok || ReadU64(input, totalDuration) != ReadState::Ok
				|| ReadU64(input, droppedRecords) != ReadState::Ok || ReadU64(input, droppedBytes) != ReadState::Ok
				|| ReadU64(input, segmentCount) != ReadState::Ok || ReadU64(input, engineCpuCount) != ReadState::Ok
				|| ReadU64(input, engineCpuTotal) != ReadState::Ok || ReadU64(input, lagHighWater) != ReadState::Ok
				|| ReadU64(input, subsystemSummaries[0u].availableFrameCount) != ReadState::Ok
				|| ReadU64(input, subsystemSummaries[0u].totalNanoseconds) != ReadState::Ok
				|| ReadU64(input, subsystemSummaries[1u].availableFrameCount) != ReadState::Ok
				|| ReadU64(input, subsystemSummaries[1u].totalNanoseconds) != ReadState::Ok
				|| ReadU64(input, firstWindowCount) != ReadState::Ok
				|| ReadU64(input, firstWindowTotal) != ReadState::Ok
				|| ReadU64(input, firstWindowMedian) != ReadState::Ok
				|| ReadU64(input, firstWindowP95) != ReadState::Ok
				|| ReadU64(input, finalWindowCount) != ReadState::Ok
				|| ReadU64(input, finalWindowTotal) != ReadState::Ok
				|| ReadU64(input, finalWindowMedian) != ReadState::Ok
				|| ReadU64(input, finalWindowP95) != ReadState::Ok
				|| ReadU64(input, profilerCollection) != ReadState::Ok
				|| ReadU64(input, profilerSealing) != ReadState::Ok
				|| ReadU64(input, profilerQueueing) != ReadState::Ok
				|| ReadU64(input, profilerPersistence) != ReadState::Ok
				|| ReadU64(input, profilerFinalization) != ReadState::Ok
				|| ReadU64(input, persistedBytes) != ReadState::Ok
				|| ReadU64(input, persistenceOperations) != ReadState::Ok
				|| ReadU64(input, persistenceThroughput) != ReadState::Ok
				|| ReadU64(input, logicalInputBytes) != ReadState::Ok
				|| ReadU64(input, encodedPayloadBytes) != ReadState::Ok
				|| ReadU64(input, storedSegmentBytes) != ReadState::Ok
				|| ReadU64(input, lastCheckpointOffset) != ReadState::Ok
				|| ReadU64(input, checkpointPageCount) != ReadState::Ok
				|| ReadU64(input, predecessor) != ReadState::Ok
				|| ReadExact(input, completeByte.data(), completeByte.size()) != ReadState::Ok
				|| ReadU64(input, digest) != ReadState::Ok)
			{
				state.result.recoveredIncompleteTail = true;
				break;
			}
			std::string terminalBytes(kTerminalMagic.data(), kTerminalMagic.size());
			AppendU64(terminalBytes, sequence); AppendU64(terminalBytes, frameCount);
			AppendU64(terminalBytes, completeFrameCount); AppendU64(terminalBytes, totalDuration);
			AppendU64(terminalBytes, droppedRecords); AppendU64(terminalBytes, droppedBytes);
			AppendU64(terminalBytes, segmentCount); AppendU64(terminalBytes, engineCpuCount);
			AppendU64(terminalBytes, engineCpuTotal); AppendU64(terminalBytes, lagHighWater);
			for (const ProfilerJournalV1SubsystemSummary& subsystem : subsystemSummaries)
			{
				AppendU64(terminalBytes, subsystem.availableFrameCount);
				AppendU64(terminalBytes, subsystem.totalNanoseconds);
			}
			AppendU64(terminalBytes, firstWindowCount); AppendU64(terminalBytes, firstWindowTotal);
			AppendU64(terminalBytes, firstWindowMedian); AppendU64(terminalBytes, firstWindowP95);
			AppendU64(terminalBytes, finalWindowCount); AppendU64(terminalBytes, finalWindowTotal);
			AppendU64(terminalBytes, finalWindowMedian); AppendU64(terminalBytes, finalWindowP95);
			AppendU64(terminalBytes, profilerCollection); AppendU64(terminalBytes, profilerSealing);
			AppendU64(terminalBytes, profilerQueueing); AppendU64(terminalBytes, profilerPersistence);
			AppendU64(terminalBytes, profilerFinalization);
			AppendU64(terminalBytes, persistedBytes); AppendU64(terminalBytes, persistenceOperations);
			AppendU64(terminalBytes, persistenceThroughput);
			AppendU64(terminalBytes, logicalInputBytes); AppendU64(terminalBytes, encodedPayloadBytes);
			AppendU64(terminalBytes, storedSegmentBytes);
			AppendU64(terminalBytes, lastCheckpointOffset);
			AppendU64(terminalBytes, checkpointPageCount);
			AppendU64(terminalBytes, predecessor);
			terminalBytes.push_back(completeByte[0]);
			if (!state.pendingSegments.empty() || sequence != state.expectedSequence
				|| predecessor != state.previousDigest
				|| lastCheckpointOffset != state.lastCheckpointOffset
				|| checkpointPageCount != state.checkpointPageCount
				|| Crc64(terminalBytes) != digest)
			{
				outError = "Profiler journal V1 terminal integrity chain is invalid.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			if (firstWindowCount > std::min<std::uint64_t>(engineCpuCount, kMaximumFiveMinuteSamples)
				|| finalWindowCount > std::min<std::uint64_t>(engineCpuCount, kMaximumFiveMinuteSamples)
				|| (firstWindowCount == 0u
					&& (firstWindowTotal != 0u || firstWindowMedian != 0u || firstWindowP95 != 0u))
				|| (finalWindowCount == 0u
					&& (finalWindowTotal != 0u || finalWindowMedian != 0u || finalWindowP95 != 0u)))
			{
				outError = "Profiler journal V1 terminal timing windows are invalid.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			bool subsystemSummaryMismatch = false;
			if (!indexOnlyRead)
			{
				for (std::size_t moduleOrdinal = 0u;
					moduleOrdinal < subsystemSummaries.size(); ++moduleOrdinal)
				{
					const ProfilerJournalV1SubsystemSummary& terminalSubsystem =
						subsystemSummaries[moduleOrdinal];
					const ProfilerJournalV1SubsystemSummary& decodedSubsystem =
						state.result.summary.subsystemSummaries[moduleOrdinal];
					subsystemSummaryMismatch = subsystemSummaryMismatch
						|| terminalSubsystem.availableFrameCount != decodedSubsystem.availableFrameCount
						|| terminalSubsystem.totalNanoseconds != decodedSubsystem.totalNanoseconds;
				}
			}
			if (!targetedRead && (frameCount != state.committedFrameCount
					|| segmentCount != state.expectedSequence
					|| completeFrameCount != state.result.summary.completeFrameCount
					|| totalDuration != state.result.summary.totalDurationNanoseconds
					|| (!indexOnlyRead && (droppedRecords != state.result.summary.droppedDetailRecordCount
						|| droppedBytes != state.result.summary.droppedDetailByteCount
						|| segmentCount != state.result.summary.committedSegmentCount
						|| engineCpuCount != state.result.summary.engineCpuSampleCount
						|| engineCpuTotal != state.result.summary.engineCpuTotalNanoseconds
						|| subsystemSummaryMismatch))))
			{
				outError = "Profiler journal V1 terminal counts disagree with committed evidence.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			state.result.complete = completeByte[0] != 0;
			state.result.summary.frameCount = frameCount;
			state.result.summary.completeFrameCount = completeFrameCount;
			state.result.summary.totalDurationNanoseconds = totalDuration;
			state.result.summary.droppedDetailRecordCount = droppedRecords;
			state.result.summary.droppedDetailByteCount = droppedBytes;
			state.result.summary.committedSegmentCount = segmentCount;
			state.result.summary.engineCpuSampleCount = engineCpuCount;
			state.result.summary.engineCpuTotalNanoseconds = engineCpuTotal;
			state.result.summary.subsystemSummaries = subsystemSummaries;
			state.result.summary.persistenceLagHighWaterFrames = lagHighWater;
			state.result.summary.firstEngineCpuWindowSampleCount = firstWindowCount;
			state.result.summary.firstEngineCpuWindowTotalNanoseconds = firstWindowTotal;
			state.result.summary.firstEngineCpuWindowMedianNanoseconds = firstWindowMedian;
			state.result.summary.firstEngineCpuWindowP95Nanoseconds = firstWindowP95;
			state.result.summary.finalEngineCpuWindowSampleCount = finalWindowCount;
			state.result.summary.finalEngineCpuWindowTotalNanoseconds = finalWindowTotal;
			state.result.summary.finalEngineCpuWindowMedianNanoseconds = finalWindowMedian;
			state.result.summary.finalEngineCpuWindowP95Nanoseconds = finalWindowP95;
			state.result.summary.profilerCollectionNanoseconds = profilerCollection;
			state.result.summary.profilerSealingNanoseconds = profilerSealing;
			state.result.summary.profilerQueueingNanoseconds = profilerQueueing;
			state.result.summary.profilerPersistenceNanoseconds = profilerPersistence;
			state.result.summary.profilerFinalizationNanoseconds = profilerFinalization;
			state.result.summary.persistedBytes = persistedBytes;
			state.result.summary.persistenceOperationCount = persistenceOperations;
			state.result.summary.persistenceThroughputBytesPerSecond = persistenceThroughput;
			state.result.summary.logicalInputBytes = logicalInputBytes;
			state.result.summary.encodedPayloadBytes = encodedPayloadBytes;
			state.result.summary.storedSegmentBytes = storedSegmentBytes;
			state.result.recoveredIncompleteTail = false;
			break;
		}
		if (marker != kSegmentMagic)
		{
			outError = "Profiler journal V1 encountered an unknown committed record.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		std::uint64_t sequence = 0u;
		std::uint64_t firstFrame = 0u;
		std::uint64_t lastFrame = 0u;
		std::uint32_t frameCount = 0u;
		std::array<char, 1u> codecByte{};
		std::uint64_t uncompressedBytes = 0u;
		std::uint64_t storedBytes = 0u;
		std::uint64_t predecessor = 0u;
		if (ReadU64(input, sequence) != ReadState::Ok || ReadU64(input, firstFrame) != ReadState::Ok
			|| ReadU64(input, lastFrame) != ReadState::Ok
			|| ReadU32(input, frameCount) != ReadState::Ok || ReadExact(input, codecByte.data(), codecByte.size()) != ReadState::Ok
			|| ReadU64(input, uncompressedBytes) != ReadState::Ok || ReadU64(input, storedBytes) != ReadState::Ok
			|| ReadU64(input, predecessor) != ReadState::Ok)
		{
			state.result.recoveredIncompleteTail = true;
			break;
		}
		const std::uint64_t maximumStoredBytes = std::min<std::uint64_t>(
			kMaximumJournalRecordBytes, ZSTD_compressBound(state.metadata.settings.maximumChunkBytes));
		if (sequence != state.expectedSequence || frameCount == 0u || firstFrame > lastFrame
			|| codecByte[0] != static_cast<char>(SegmentCodec::Zstd)
			|| uncompressedBytes == 0u || uncompressedBytes > state.metadata.settings.maximumChunkBytes
			|| storedBytes == 0u || storedBytes > maximumStoredBytes
			|| predecessor != state.previousDigest)
		{
			outError = "Profiler journal V1 segment header is invalid or out of order.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		const bool selectedSegment = !targetedRead
			|| (*selectedFrameIndex >= firstFrame && *selectedFrameIndex <= lastFrame);
		const std::size_t decodedIndexOffset = state.result.index.size();
		// Directory pages cover up to thirty-two segments. Nonmatching payloads
		// are skipped now; their committed offsets/digests stay in a bounded pending
		// page until IDX1 validates the complete directory batch.
		if (!selectedSegment || indexOnlyRead)
		{
			input.seekg(static_cast<std::streamoff>(storedBytes), std::ios::cur);
			if (!input)
			{
				state.result.recoveredIncompleteTail = true;
				break;
			}
			std::array<char, 4u> skippedCommitMagic{};
			std::uint64_t skippedSequence = 0u;
			std::uint64_t skippedDigest = 0u;
			std::uint64_t skippedFrames = 0u;
			if (ReadExact(input, skippedCommitMagic.data(), skippedCommitMagic.size()) != ReadState::Ok
				|| ReadU64(input, skippedSequence) != ReadState::Ok || ReadU64(input, skippedDigest) != ReadState::Ok
				|| ReadU64(input, skippedFrames) != ReadState::Ok
				|| skippedCommitMagic != kCommitMagic || skippedSequence != sequence || skippedFrames != frameCount)
			{
				state.result.recoveredIncompleteTail = true;
				break;
			}
			state.previousDigest = skippedDigest;
			state.previousFrameIndex = lastFrame;
			state.haveFrame = true;
			state.committedFrameCount += frameCount;
			state.pendingSegments.push_back({recordOffset, firstFrame, lastFrame,
				frameCount, skippedDigest, decodedIndexOffset, false});
			if (indexOnlyRead)
			{
				state.result.summary.frameCount += frameCount;
				++state.result.summary.committedSegmentCount;
			}
			++state.expectedSequence;
			continue;
		}
		std::string storedPayload(static_cast<std::size_t>(storedBytes), '\0');
		if (ReadExact(input, storedPayload.data(), storedPayload.size()) != ReadState::Ok)
		{
			state.result.recoveredIncompleteTail = true;
			break;
		}
		std::array<char, 4u> commitMagic{};
		std::uint64_t committedSequence = 0u;
		std::uint64_t digest = 0u;
		std::uint64_t committedFrames = 0u;
		if (ReadExact(input, commitMagic.data(), commitMagic.size()) != ReadState::Ok
			|| ReadU64(input, committedSequence) != ReadState::Ok || ReadU64(input, digest) != ReadState::Ok
			|| ReadU64(input, committedFrames) != ReadState::Ok)
		{
			state.result.recoveredIncompleteTail = true;
			break;
		}
		std::string digestBytes(kSegmentMagic.data(), kSegmentMagic.size());
		AppendU64(digestBytes, sequence); AppendU64(digestBytes, firstFrame); AppendU64(digestBytes, lastFrame); AppendU32(digestBytes, frameCount);
		digestBytes.push_back(codecByte[0]); AppendU64(digestBytes, uncompressedBytes);
		AppendU64(digestBytes, storedBytes); AppendU64(digestBytes, predecessor); digestBytes.append(storedPayload);
		if (commitMagic != kCommitMagic || committedSequence != sequence || committedFrames != frameCount
			|| Crc64(digestBytes) != digest)
		{
			outError = "Profiler journal V1 committed segment integrity check failed.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		std::string payload;
		if (!DecompressSegment(storedPayload, uncompressedBytes, payload, outError))
		{
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		// Segment headers persist their inclusive frame range.  A selected-frame
		// read can therefore verify the committed bytes and chain while skipping
		// compact-frame decoding for every nonmatching segment.
		ProfilerJournalV1Summary delta;
		if (!DecodeSegmentPayload(payload, frameCount, recordOffset, document != nullptr, document,
			selectedFrameIndex, state.result, delta, outError))
		{
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		if (!state.result.index.empty() && state.result.index[state.result.index.size() - frameCount].frameIndex != firstFrame
			|| (state.haveFrame && firstFrame <= state.previousFrameIndex))
		{
			outError = "Profiler journal V1 segment frame sequence is invalid.";
			return ProfilerJournalV1ResultCode::CorruptCommittedData;
		}
		delta.committedSegmentCount = 1u;
		MergeSummary(state.result.summary, delta);
		state.previousDigest = digest;
		state.previousFrameIndex = state.result.index.back().frameIndex;
		state.haveFrame = true;
		state.committedFrameCount += frameCount;
		state.pendingSegments.push_back({recordOffset, firstFrame, lastFrame,
			frameCount, digest, decodedIndexOffset, true});
		++state.expectedSequence;
	}
	// A power loss can leave up to thirty-one committed segments after the last
	// sparse directory page. Reopen only that bounded tail, verify each stored
	// payload against its durable commit digest, and reconstruct its compact rows.
	// Completed captures never take this path because Finalize flushes the page.
	if (state.result.recoveredIncompleteTail && !state.pendingSegments.empty())
	{
		for (const PendingSegmentDirectory& pending : state.pendingSegments)
		{
			if (pending.decoded)
			{
				continue;
			}
			input.clear();
			input.seekg(static_cast<std::streamoff>(pending.offset), std::ios::beg);
			std::array<char, 4u> segmentMagic{};
			std::uint64_t sequence = 0u;
			std::uint64_t firstFrame = 0u;
			std::uint64_t lastFrame = 0u;
			std::uint32_t frameCount = 0u;
			std::array<char, 1u> codecByte{};
			std::uint64_t uncompressedBytes = 0u;
			std::uint64_t storedBytes = 0u;
			std::uint64_t predecessor = 0u;
			if (ReadExact(input, segmentMagic.data(), segmentMagic.size()) != ReadState::Ok
				|| segmentMagic != kSegmentMagic
				|| ReadU64(input, sequence) != ReadState::Ok
				|| ReadU64(input, firstFrame) != ReadState::Ok
				|| ReadU64(input, lastFrame) != ReadState::Ok
				|| ReadU32(input, frameCount) != ReadState::Ok
				|| ReadExact(input, codecByte.data(), codecByte.size()) != ReadState::Ok
				|| ReadU64(input, uncompressedBytes) != ReadState::Ok
				|| ReadU64(input, storedBytes) != ReadState::Ok
				|| ReadU64(input, predecessor) != ReadState::Ok
				|| firstFrame != pending.firstFrame || lastFrame != pending.lastFrame
				|| frameCount != pending.frameCount
				|| codecByte[0] != static_cast<char>(SegmentCodec::Zstd)
				|| storedBytes == 0u || storedBytes > kMaximumJournalRecordBytes)
			{
				outError = "Profiler journal V1 interrupted directory tail is malformed.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			std::string storedPayload(static_cast<std::size_t>(storedBytes), '\0');
			std::array<char, 4u> commitMagic{};
			std::uint64_t committedSequence = 0u;
			std::uint64_t digest = 0u;
			std::uint64_t committedFrames = 0u;
			if (ReadExact(input, storedPayload.data(), storedPayload.size()) != ReadState::Ok
				|| ReadExact(input, commitMagic.data(), commitMagic.size()) != ReadState::Ok
				|| ReadU64(input, committedSequence) != ReadState::Ok
				|| ReadU64(input, digest) != ReadState::Ok
				|| ReadU64(input, committedFrames) != ReadState::Ok)
			{
				outError = "Profiler journal V1 interrupted directory tail is truncated.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			std::string digestBytes(kSegmentMagic.data(), kSegmentMagic.size());
			AppendU64(digestBytes, sequence); AppendU64(digestBytes, firstFrame);
			AppendU64(digestBytes, lastFrame); AppendU32(digestBytes, frameCount);
			digestBytes.push_back(codecByte[0]); AppendU64(digestBytes, uncompressedBytes);
			AppendU64(digestBytes, storedBytes); AppendU64(digestBytes, predecessor);
			digestBytes.append(storedPayload);
			if (commitMagic != kCommitMagic || committedSequence != sequence
				|| committedFrames != frameCount || digest != pending.digest
				|| Crc64(digestBytes) != digest)
			{
				outError = "Profiler journal V1 interrupted committed tail failed integrity validation.";
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			std::string payload;
			ProfilerJournalV1Summary delta;
			const std::size_t tailIndexOffset = state.result.index.size();
			if (!DecompressSegment(storedPayload, uncompressedBytes, payload, outError)
				|| !DecodeSegmentPayload(payload, frameCount, pending.offset,
					document != nullptr, document, selectedFrameIndex,
					state.result, delta, outError))
			{
				return ProfilerJournalV1ResultCode::CorruptCommittedData;
			}
			for (std::size_t entryIndex = tailIndexOffset;
				entryIndex < state.result.index.size(); ++entryIndex)
			{
				state.result.index[entryIndex].segmentOffset = pending.offset;
				state.result.index[entryIndex].storedPayloadDigest = digest;
			}
			if (indexPageSelection)
			{
				std::vector<ProfilerJournalV1IndexEntry> decodedTail(
					std::make_move_iterator(state.result.index.begin()
						+ static_cast<std::ptrdiff_t>(tailIndexOffset)),
					std::make_move_iterator(state.result.index.end()));
				state.result.index.resize(tailIndexOffset);
				RetainIndexPageEntries(decodedTail, *indexPageSelection,
					state.result.index);
			}
			// The first pass already counted the committed segment/frame envelope.
			// Tail decoding contributes only facts unavailable without frame payloads.
			delta.frameCount = 0u;
			delta.committedSegmentCount = 0u;
			MergeSummary(state.result.summary, delta);
		}
	}
	if (document != nullptr)
	{
		document->committedFrameCount = state.result.summary.frameCount;
		document->complete = state.result.complete;
		document->recoveredIncompleteTail = state.result.recoveredIncompleteTail;
	}
	outResult = std::move(state.result);
	return outResult.recoveredIncompleteTail ? ProfilerJournalV1ResultCode::IncompleteTail : ProfilerJournalV1ResultCode::Ok;
}
}

std::shared_ptr<ProfilerJournalV1Sink> CreateFilesystemProfilerJournalV1Sink()
{
	return std::make_shared<FilesystemProfilerJournalV1Sink>();
}

struct ProfilerJournalV1Writer::Impl
{
	struct CompressionContextDeleter
	{
		void operator()(ZSTD_CCtx* context) const noexcept
		{
			ZSTD_freeCCtx(context);
		}
	};

	std::shared_ptr<ProfilerJournalV1Sink> sink;
	// The writer has one serialized owner. Retaining its Zstd context and the two
	// largest temporary strings avoids a periodic allocator/context-setup burst
	// whenever a detailed journal segment is committed.
	std::unique_ptr<ZSTD_CCtx, CompressionContextDeleter> compressionContext{ZSTD_createCCtx()};
	std::string payloadScratch;
	std::string storedPayloadScratch;
	ProfilerCaptureMetadata metadata;
	ProfilerJournalV1Summary summary;
	std::uint64_t nextSequence = 0u;
	std::uint64_t previousDigest = 0u;
	std::uint64_t previousFrameIndex = 0u;
	bool haveFrame = false;
	bool finalized = false;
	std::uint64_t lastStoredSegmentBytes = 0u;
	std::uint64_t durableByteOffset = 0u;
	std::vector<ProfilerJournalV1IndexEntry> pendingCheckpointEntries;
	std::uint32_t pendingCheckpointSegmentCount = 0u;
	std::uint64_t lastCheckpointOffset = 0u;
	std::uint64_t checkpointPageCount = 0u;
	std::optional<double> firstEngineCpuFrameStartMs;
	std::vector<std::uint64_t> firstEngineCpuWindow;
	std::deque<std::pair<double, std::uint64_t>> finalEngineCpuWindow;
};

ProfilerJournalV1Writer::ProfilerJournalV1Writer()
	: ProfilerJournalV1Writer(CreateFilesystemProfilerJournalV1Sink())
{
}

ProfilerJournalV1Writer::ProfilerJournalV1Writer(
	std::shared_ptr<ProfilerJournalV1Sink> sink)
	: impl(std::make_unique<Impl>())
{
	impl->sink = sink ? std::move(sink) : CreateFilesystemProfilerJournalV1Sink();
}

ProfilerJournalV1Writer::~ProfilerJournalV1Writer() = default;
ProfilerJournalV1Writer::ProfilerJournalV1Writer(ProfilerJournalV1Writer&&) noexcept = default;
ProfilerJournalV1Writer& ProfilerJournalV1Writer::operator=(ProfilerJournalV1Writer&&) noexcept = default;

ProfilerJournalV1ResultCode ProfilerJournalV1Writer::Open(
	const std::filesystem::path& path,
	const ProfilerCaptureMetadata& metadata,
	std::string& outError)
{
	outError.clear();
	if (impl->sink->IsOpen())
	{
		outError = "Profiler journal V1 writer is already open.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	if (metadata.settings.residentByteBudget == 0u || metadata.settings.maximumPendingBytes == 0u
		|| metadata.settings.maximumSealedFrameBytes == 0u
		|| metadata.settings.maximumPendingBytes > metadata.settings.residentByteBudget
		|| metadata.settings.maximumChunkBytes > metadata.settings.maximumPendingBytes)
	{
		outError = "Profiler journal V1 requires a coherent chunk <= pending <= resident memory budget.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	if (!impl->sink->Open(path, outError))
	{
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	const std::string metadataText = SettingsToJsonText(metadata);
	if (metadataText.empty() || metadataText.size() > kMaximumJournalRecordBytes)
	{
		outError = "Profiler journal V1 metadata is outside the bounded header limit.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	std::string header(kJournalMagic.data(), kJournalMagic.size());
	AppendU32(header, kProfilerJournalV1SchemaVersion);
	AppendU32(header, static_cast<std::uint32_t>(metadataText.size()));
	AppendU64(header, Crc64(metadataText));
	header.append(metadataText);
	if (!impl->sink->WriteAndFlush(header, outError))
	{
		if (outError.empty())
		{
			outError = "Failed to commit profiler journal V1 header.";
		}
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	impl->durableByteOffset = header.size();
	impl->metadata = metadata;
	impl->summary = {};
	impl->nextSequence = 0u;
	impl->previousDigest = 0u;
	impl->previousFrameIndex = 0u;
	impl->haveFrame = false;
	impl->finalized = false;
	impl->pendingCheckpointEntries.clear();
	impl->pendingCheckpointSegmentCount = 0u;
	impl->lastCheckpointOffset = 0u;
	impl->checkpointPageCount = 0u;
	return ProfilerJournalV1ResultCode::Ok;
}

ProfilerJournalV1ResultCode ProfilerJournalV1Writer::FlushPendingCheckpoint(
	std::string& outError)
{
	if (impl->pendingCheckpointEntries.empty())
	{
		return ProfilerJournalV1ResultCode::Ok;
	}
	const std::uint64_t checkpointOffset = impl->durableByteOffset;
	std::string checkpoint(kIndexCheckpointMagic.data(), kIndexCheckpointMagic.size());
	AppendU64(checkpoint, impl->nextSequence);
	AppendU64(checkpoint, impl->summary.frameCount);
	AppendU64(checkpoint, impl->previousFrameIndex);
	AppendU64(checkpoint, impl->previousDigest);
	AppendU64(checkpoint, impl->lastCheckpointOffset);
	AppendU32(checkpoint, impl->pendingCheckpointSegmentCount);
	AppendU32(checkpoint, static_cast<std::uint32_t>(impl->pendingCheckpointEntries.size()));
	for (const ProfilerJournalV1IndexEntry& entry : impl->pendingCheckpointEntries)
	{
		AppendU64(checkpoint, entry.frameIndex);
		AppendU64(checkpoint, static_cast<std::uint64_t>(
			std::llround(entry.durationMs * 1000000.0)));
		checkpoint.push_back(entry.complete ? '\1' : '\0');
		AppendU64(checkpoint, entry.segmentOffset);
		AppendU64(checkpoint, entry.storedPayloadDigest);
	}
	AppendU64(checkpoint, Crc64(checkpoint));
	if (!impl->sink->WriteAndFlush(checkpoint, outError))
	{
		if (outError.empty())
		{
			outError = "Failed to persist profiler journal V1 index checkpoint page.";
		}
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	impl->durableByteOffset += static_cast<std::uint64_t>(checkpoint.size());
	impl->lastCheckpointOffset = checkpointOffset;
	++impl->checkpointPageCount;
	impl->pendingCheckpointEntries.clear();
	impl->pendingCheckpointSegmentCount = 0u;
	return ProfilerJournalV1ResultCode::Ok;
}

ProfilerJournalV1ResultCode ProfilerJournalV1Writer::AppendCommittedSegment(
	std::span<const ProfilerSealedFrame> frames,
	std::string& outError)
{
	outError.clear();
	if (!impl->sink->IsOpen())
	{
		return ProfilerJournalV1ResultCode::NotOpen;
	}
	if (impl->finalized)
	{
		return ProfilerJournalV1ResultCode::AlreadyFinalized;
	}
	if (frames.empty() || frames.size() > impl->metadata.settings.maximumChunkFrames)
	{
		outError = "Profiler journal V1 segment exceeds its configured frame bound.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	if (impl->haveFrame && frames.front().frame.frameIndex <= impl->previousFrameIndex)
	{
		outError = "Profiler journal V1 cannot append an out-of-order frame.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	ProfilerJournalV1Summary delta;
	if (!impl->compressionContext
		|| !BuildSegmentPayload(frames, impl->metadata.settings, impl->payloadScratch, delta, outError)
		|| impl->payloadScratch.size() > impl->metadata.settings.maximumChunkBytes)
	{
		if (outError.empty())
		{
			outError = "Profiler journal V1 segment exceeds its configured byte bound.";
		}
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	const std::uint64_t firstFrame = frames.front().frame.frameIndex;
	const std::uint64_t lastFrame = frames.back().frame.frameIndex;
	if (!CompressSegment(*impl->compressionContext, impl->payloadScratch,
		impl->storedPayloadScratch, outError)
		|| impl->storedPayloadScratch.empty()
		|| impl->storedPayloadScratch.size() > kMaximumJournalRecordBytes)
	{
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	std::string digestBytes(kSegmentMagic.data(), kSegmentMagic.size());
	AppendU64(digestBytes, impl->nextSequence); AppendU64(digestBytes, firstFrame); AppendU64(digestBytes, lastFrame);
	AppendU32(digestBytes, static_cast<std::uint32_t>(frames.size()));
	digestBytes.push_back(static_cast<char>(SegmentCodec::Zstd));
	AppendU64(digestBytes, static_cast<std::uint64_t>(impl->payloadScratch.size()));
	AppendU64(digestBytes, static_cast<std::uint64_t>(impl->storedPayloadScratch.size()));
	AppendU64(digestBytes, impl->previousDigest); digestBytes.append(impl->storedPayloadScratch);
	const std::uint64_t digest = Crc64(digestBytes);
	std::string segmentRecord(kSegmentMagic.data(), kSegmentMagic.size());
	AppendU64(segmentRecord, impl->nextSequence); AppendU64(segmentRecord, firstFrame);
	AppendU64(segmentRecord, lastFrame);
	AppendU32(segmentRecord, static_cast<std::uint32_t>(frames.size()));
	segmentRecord.push_back(static_cast<char>(SegmentCodec::Zstd));
	AppendU64(segmentRecord, static_cast<std::uint64_t>(impl->payloadScratch.size()));
	AppendU64(segmentRecord, static_cast<std::uint64_t>(impl->storedPayloadScratch.size()));
	AppendU64(segmentRecord, impl->previousDigest); segmentRecord.append(impl->storedPayloadScratch);
	const std::uint64_t segmentOffset = impl->durableByteOffset;
	std::string commitRecord(kCommitMagic.data(), kCommitMagic.size());
	AppendU64(commitRecord, impl->nextSequence); AppendU64(commitRecord, digest);
	AppendU64(commitRecord, static_cast<std::uint64_t>(frames.size()));
	// A segment is recoverable only once its footer is durable.  Send the payload
	// and footer as one sink operation so the worker performs one flush per
	// committed segment, while an interrupted partial write still has no COM1
	// record and is therefore rejected as an uncommitted tail.
	segmentRecord.append(commitRecord);
	if (!impl->sink->WriteAndFlush(segmentRecord, outError))
	{
		if (outError.empty())
		{
			outError = "Failed to commit profiler journal V1 segment.";
		}
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	impl->durableByteOffset += static_cast<std::uint64_t>(segmentRecord.size());
	impl->lastStoredSegmentBytes = static_cast<std::uint64_t>(segmentRecord.size());
	for (const ProfilerSealedFrame& sealed : frames)
	{
		impl->summary.logicalInputBytes += sealed.ownedByteCount;
	}
	impl->summary.encodedPayloadBytes += static_cast<std::uint64_t>(impl->payloadScratch.size());
	impl->summary.storedSegmentBytes += impl->lastStoredSegmentBytes;
	delta.committedSegmentCount = 1u;
	MergeSummary(impl->summary, delta);
	impl->previousDigest = digest;
	impl->previousFrameIndex = frames.back().frame.frameIndex;
	impl->haveFrame = true;
	++impl->nextSequence;
	// Window samples become observable only after the segment commit succeeds.
	// This prevents a writable-destination retry from double-counting frames.
	for (const ProfilerSealedFrame& sealed : frames)
	{
		for (const ProfilerTimingStreamSample& stream : sealed.frame.timingStreams)
		{
			if (stream.scope != ProfilerTimingStreamScope::EngineCpu
				|| stream.status != ProfilerTimingStreamStatus::Valid)
			{
				continue;
			}
			const std::uint64_t value = static_cast<std::uint64_t>(
				std::llround(stream.milliseconds * 1000000.0));
			if (!impl->firstEngineCpuFrameStartMs.has_value())
			{
				impl->firstEngineCpuFrameStartMs = sealed.frame.frameStartMs;
			}
			if (sealed.frame.frameStartMs - *impl->firstEngineCpuFrameStartMs
				<= kFiveMinuteWindowMilliseconds
				&& impl->firstEngineCpuWindow.size() < kMaximumFiveMinuteSamples)
			{
				impl->firstEngineCpuWindow.push_back(value);
			}
			impl->finalEngineCpuWindow.emplace_back(sealed.frame.frameStartMs, value);
			while (!impl->finalEngineCpuWindow.empty()
				&& (sealed.frame.frameStartMs - impl->finalEngineCpuWindow.front().first
					> kFiveMinuteWindowMilliseconds
					|| impl->finalEngineCpuWindow.size() > kMaximumFiveMinuteSamples))
			{
				impl->finalEngineCpuWindow.pop_front();
			}
			break;
		}
	}
	// Directory pages amortize flushes across thirty-two committed segments.
	// Only this bounded pending page stays resident; each row carries the durable
	// segment offset and digest needed for later selected-frame seek/validation.
	for (const ProfilerSealedFrame& sealed : frames)
	{
		impl->pendingCheckpointEntries.push_back({
			sealed.frame.frameIndex,
			sealed.frame.durationMs,
			sealed.frame.complete,
			segmentOffset,
			digest});
	}
	++impl->pendingCheckpointSegmentCount;
	if (impl->pendingCheckpointSegmentCount >= kCheckpointSegmentStride)
	{
		return FlushPendingCheckpoint(outError);
	}
	return ProfilerJournalV1ResultCode::Ok;
}

ProfilerJournalV1ResultCode ProfilerJournalV1Writer::Finalize(bool complete, std::string& outError)
{
	const auto finalizationStart = std::chrono::steady_clock::now();
	outError.clear();
	if (!impl->sink->IsOpen())
	{
		return ProfilerJournalV1ResultCode::NotOpen;
	}
	if (impl->finalized)
	{
		return ProfilerJournalV1ResultCode::AlreadyFinalized;
	}
	const ProfilerJournalV1ResultCode checkpointResult = FlushPendingCheckpoint(outError);
	if (checkpointResult != ProfilerJournalV1ResultCode::Ok)
	{
		return checkpointResult;
	}
	CompleteEngineCpuWindowSummary(
		impl->summary, impl->firstEngineCpuWindow, impl->finalEngineCpuWindow);
	// Terminal contains the final bounded summary itself, so it can persist only
	// the deterministic work completed before its own WriteAndFlush. The caller
	// separately tracks the complete sink-inclusive finalization duration in
	// live health; encoding that future duration in these immutable bytes would
	// be impossible without a second terminal-format record.
	impl->summary.profilerFinalizationNanoseconds += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - finalizationStart).count());
	std::string terminal(kTerminalMagic.data(), kTerminalMagic.size());
	AppendU64(terminal, impl->nextSequence); AppendU64(terminal, impl->summary.frameCount);
	AppendU64(terminal, impl->summary.completeFrameCount); AppendU64(terminal, impl->summary.totalDurationNanoseconds);
	AppendU64(terminal, impl->summary.droppedDetailRecordCount); AppendU64(terminal, impl->summary.droppedDetailByteCount);
	AppendU64(terminal, impl->summary.committedSegmentCount); AppendU64(terminal, impl->summary.engineCpuSampleCount);
	AppendU64(terminal, impl->summary.engineCpuTotalNanoseconds); AppendU64(terminal, impl->summary.persistenceLagHighWaterFrames);
	for (const ProfilerJournalV1SubsystemSummary& subsystem : impl->summary.subsystemSummaries)
	{
		AppendU64(terminal, subsystem.availableFrameCount);
		AppendU64(terminal, subsystem.totalNanoseconds);
	}
	AppendU64(terminal, impl->summary.firstEngineCpuWindowSampleCount);
	AppendU64(terminal, impl->summary.firstEngineCpuWindowTotalNanoseconds);
	AppendU64(terminal, impl->summary.firstEngineCpuWindowMedianNanoseconds);
	AppendU64(terminal, impl->summary.firstEngineCpuWindowP95Nanoseconds);
	AppendU64(terminal, impl->summary.finalEngineCpuWindowSampleCount);
	AppendU64(terminal, impl->summary.finalEngineCpuWindowTotalNanoseconds);
	AppendU64(terminal, impl->summary.finalEngineCpuWindowMedianNanoseconds);
	AppendU64(terminal, impl->summary.finalEngineCpuWindowP95Nanoseconds);
	AppendU64(terminal, impl->summary.profilerCollectionNanoseconds);
	AppendU64(terminal, impl->summary.profilerSealingNanoseconds);
	AppendU64(terminal, impl->summary.profilerQueueingNanoseconds);
	AppendU64(terminal, impl->summary.profilerPersistenceNanoseconds);
	AppendU64(terminal, impl->summary.profilerFinalizationNanoseconds);
	AppendU64(terminal, impl->summary.persistedBytes);
	AppendU64(terminal, impl->summary.persistenceOperationCount);
	AppendU64(terminal, impl->summary.persistenceThroughputBytesPerSecond);
	AppendU64(terminal, impl->summary.logicalInputBytes);
	AppendU64(terminal, impl->summary.encodedPayloadBytes);
	AppendU64(terminal, impl->summary.storedSegmentBytes);
	AppendU64(terminal, impl->lastCheckpointOffset);
	AppendU64(terminal, impl->checkpointPageCount);
	AppendU64(terminal, impl->previousDigest);
	terminal.push_back(complete ? '\1' : '\0');
	const std::uint64_t terminalChecksum = Crc64(terminal);
	AppendU64(terminal, terminalChecksum);
	if (!impl->sink->WriteAndFlush(terminal, outError))
	{
		if (outError.empty())
		{
			outError = "Failed to commit profiler journal V1 terminal.";
		}
		return ProfilerJournalV1ResultCode::IoFailure;
	}
	impl->finalized = true;
	return ProfilerJournalV1ResultCode::Ok;
}

bool ProfilerJournalV1Writer::IsOpen() const noexcept { return impl && impl->sink && impl->sink->IsOpen(); }
bool ProfilerJournalV1Writer::IsFinalized() const noexcept { return impl && impl->finalized; }
const ProfilerJournalV1Summary& ProfilerJournalV1Writer::GetSummary() const noexcept { return impl->summary; }
std::uint64_t ProfilerJournalV1Writer::GetLastStoredSegmentBytes() const noexcept { return impl->lastStoredSegmentBytes; }

void ProfilerJournalV1Writer::UpdateProfilerSelfSummary(
	const ProfilerCaptureHealth& health) noexcept
{
	// These values are produced by the bounded runtime data plane, not inferred
	// by the reader. The drain job calls this immediately before Finalize, while
	// it has exclusive ownership of the writer.
	impl->summary.persistenceLagHighWaterFrames = std::max(
		impl->summary.persistenceLagHighWaterFrames, health.persistenceLagHighWaterFrames);
	impl->summary.profilerCollectionNanoseconds = health.collectionNanoseconds;
	impl->summary.profilerSealingNanoseconds = health.sealingNanoseconds;
	impl->summary.profilerQueueingNanoseconds = health.queueingNanoseconds;
	impl->summary.profilerPersistenceNanoseconds = health.persistenceNanoseconds;
	impl->summary.profilerFinalizationNanoseconds = health.finalizationNanoseconds;
	impl->summary.persistedBytes = health.persistedBytes;
	impl->summary.persistenceOperationCount = health.persistenceOperationCount;
	impl->summary.persistenceThroughputBytesPerSecond = health.persistenceThroughputBytesPerSecond;
}

ProfilerJournalV1ResultCode OpenProfilerJournalV1(
	const std::filesystem::path& path,
	ProfilerCaptureDocument& outDocument,
	ProfilerJournalV1OpenResult& outResult,
	std::string& outError)
{
	return ReadJournal(path, &outDocument, std::nullopt, nullptr, outResult, outError);
}

ProfilerJournalV1ResultCode ReadProfilerJournalV1Index(
	const std::filesystem::path& path,
	ProfilerJournalV1OpenResult& outResult,
	std::string& outError)
{
	return ReadJournal(path, nullptr, std::nullopt, nullptr, outResult, outError);
}

ProfilerJournalV1ResultCode ReadProfilerJournalV1IndexPage(
	const std::filesystem::path& path,
	std::uint64_t offset,
	std::uint32_t pageSize,
	ProfilerJournalV1OpenResult& outResult,
	std::uint64_t& outTotalFrameCount,
	std::string& outError)
{
	outTotalFrameCount = 0u;
	if (pageSize > 512u)
	{
		outError = "Profiler journal V1 index page exceeds the bounded 512-row limit.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	IndexPageSelection selection{offset, pageSize, 0u};
	const ProfilerJournalV1ResultCode code = ReadJournal(
		path, nullptr, std::nullopt, &selection, outResult, outError);
	outTotalFrameCount = outResult.summary.frameCount;
	return code;
}

ProfilerJournalV1ResultCode ReadProfilerJournalV1SelectedFrame(
	const std::filesystem::path& path,
	std::uint64_t frameIndex,
	ProfilerFrameRecord& outFrame,
	std::string& outError)
{
	// A completed capture starts at END1, walks only compact IDX1 pages, then
	// seeks directly to one SEG1/CMT1 pair. This avoids replaying every prior
	// segment header or allocating a full index for an ordinary detail request.
	const ProfilerJournalV1ResultCode directResult =
		ReadProfilerJournalV1SelectedFrameDirect(path, frameIndex, outFrame, outError);
	if (directResult != ProfilerJournalV1ResultCode::IncompleteTail)
	{
		return directResult;
	}
	// Interrupted journals have no END1 anchor. Retain the existing bounded-tail
	// recovery path so a power loss after a durable commit still exposes the
	// recoverable prefix with its established result semantics.
	ProfilerCaptureDocument selected;
	ProfilerJournalV1OpenResult result;
	const ProfilerJournalV1ResultCode code = ReadJournal(
		path, &selected, frameIndex, nullptr, result, outError);
	if (code != ProfilerJournalV1ResultCode::Ok
		&& code != ProfilerJournalV1ResultCode::IncompleteTail)
	{
		return code;
	}
	if (selected.frames.size() != 1u || selected.frames.front().frameIndex != frameIndex)
	{
		outError = "The requested profiler journal frame was not committed.";
		return ProfilerJournalV1ResultCode::InvalidInput;
	}
	outFrame = std::move(selected.frames.front());
	// Preserve the historical selected-frame contract: a committed selected
	// frame remains usable when a later uncommitted tail was interrupted.
	return ProfilerJournalV1ResultCode::Ok;
}
}
