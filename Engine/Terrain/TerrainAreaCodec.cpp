// TerrainAreaCodec.cpp -- Strict little-endian .tearea envelope implementation.

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>
#include <zstd.h>
#include <xxhash.h>

namespace TerraEngine::Terrain
{
namespace
{
	constexpr std::uint32_t kHeaderBytes = 120u;
	constexpr std::uint32_t kChunkEntryBytes = 48u;
	constexpr std::uint32_t kChunkCount = 1u;
	constexpr std::size_t kEnvelopeHashOffset = 100u;

	void AppendU16(std::vector<std::uint8_t>& output, std::uint16_t value) { output.push_back(static_cast<std::uint8_t>(value)); output.push_back(static_cast<std::uint8_t>(value >> 8u)); }
	void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value) { for (std::uint32_t shift = 0; shift < 32u; shift += 8u) output.push_back(static_cast<std::uint8_t>(value >> shift)); }
	void AppendU64(std::vector<std::uint8_t>& output, std::uint64_t value) { for (std::uint32_t shift = 0; shift < 64u; shift += 8u) output.push_back(static_cast<std::uint8_t>(value >> shift)); }
	void AppendHash(std::vector<std::uint8_t>& output, ContentHash hash) { AppendU64(output, hash.low64); AppendU64(output, hash.high64); }

	bool ConsumeU16(std::span<const std::uint8_t>& input, std::uint16_t& value) { if (input.size() < 2u) return false; value = static_cast<std::uint16_t>(input[0]) | (static_cast<std::uint16_t>(input[1]) << 8u); input = input.subspan(2u); return true; }
	bool ConsumeU32(std::span<const std::uint8_t>& input, std::uint32_t& value) { if (input.size() < 4u) return false; value = 0u; for (std::uint32_t shift = 0; shift < 32u; shift += 8u) value |= static_cast<std::uint32_t>(input[shift / 8u]) << shift; input = input.subspan(4u); return true; }
	bool ConsumeU64(std::span<const std::uint8_t>& input, std::uint64_t& value) { if (input.size() < 8u) return false; value = 0u; for (std::uint32_t shift = 0; shift < 64u; shift += 8u) value |= static_cast<std::uint64_t>(input[shift / 8u]) << shift; input = input.subspan(8u); return true; }
	bool ConsumeHash(std::span<const std::uint8_t>& input, ContentHash& value) { return ConsumeU64(input, value.low64) && ConsumeU64(input, value.high64); }

	TerrainCodecResult Fail(TerrainResultCode code, std::string message) { return {code, std::move(message)}; }

	std::size_t ExpectedPayloadBytes(TerrainChannelKind kind)
	{
		switch (kind)
		{
		case TerrainChannelKind::Height: return static_cast<std::size_t>(kTerrainAreaSampleCount) * sizeof(std::int32_t);
		case TerrainChannelKind::Holes: return static_cast<std::size_t>(kTerrainAreaSampleCount) / 8u;
		case TerrainChannelKind::WeightGroup: return static_cast<std::size_t>(kTerrainAreaSampleCount) * 12u;
		}
		return 0u;
	}

	TerrainCodecResult ValidateCanonicalPayload(const Format::TerrainAreaEnvelopeHeader& header, std::span<const std::uint8_t> payload)
	{
		if (header.width != kTerrainAreaExtentSamples || header.height != kTerrainAreaExtentSamples)
			return Fail(TerrainResultCode::InvalidArgument, "Terrain V1 areas must be exactly 256 by 256 samples.");
		if ((header.channelKind == TerrainChannelKind::WeightGroup) != (header.weightGroup != 0u))
			return Fail(TerrainResultCode::InvalidArgument, "Weight channels require a non-zero group and other channels require group zero.");
		const std::size_t expected = ExpectedPayloadBytes(header.channelKind);
		if (expected == 0u || payload.size() != expected)
			return Fail(TerrainResultCode::InvalidArgument, "Canonical terrain channel length does not match its channel kind.");
		if (header.channelKind == TerrainChannelKind::WeightGroup)
		{
			for (std::size_t offset = 0u; offset < payload.size(); offset += 12u)
			{
				std::uint16_t previousLayer = std::numeric_limits<std::uint16_t>::max();
				std::uint16_t previousWeight = std::numeric_limits<std::uint16_t>::max();
				std::uint32_t totalWeight = 0u;
				for (std::size_t slot = 0u; slot < 4u; ++slot)
				{
					const std::uint16_t layer = static_cast<std::uint16_t>(payload[offset + slot * 2u]) | (static_cast<std::uint16_t>(payload[offset + slot * 2u + 1u]) << 8u);
					const std::uint8_t weight = payload[offset + 8u + slot];
					if ((layer == 0u) != (weight == 0u) || (slot > 0u && (weight > previousWeight || (weight == previousWeight && weight != 0u && layer < previousLayer))))
						return Fail(TerrainResultCode::CorruptData, "Terrain weight slots are not canonical.");
					previousLayer = layer;
					previousWeight = weight;
					totalWeight += weight;
				}
				if (totalWeight != 0u && totalWeight != 255u)
					return Fail(TerrainResultCode::CorruptData, "Non-empty terrain weights must sum to exactly 255.");
			}
		}
		return {};
	}

	void AppendHeader(std::vector<std::uint8_t>& output, const Format::TerrainAreaEnvelopeHeader& header, bool clearEnvelopeHash)
	{
		output.insert(output.end(), Format::kTerrainAreaMagic.begin(), Format::kTerrainAreaMagic.end());
		AppendU16(output, header.majorVersion); AppendU16(output, header.minorVersion); AppendU32(output, kHeaderBytes + kChunkEntryBytes);
		AppendU32(output, Format::kTerrainAreaEndianMarker); AppendU16(output, static_cast<std::uint16_t>(header.channelKind)); AppendU16(output, static_cast<std::uint16_t>(header.compression));
		output.insert(output.end(), header.assetUuid.begin(), header.assetUuid.end()); AppendU64(output, static_cast<std::uint64_t>(header.area.x)); AppendU64(output, static_cast<std::uint64_t>(header.area.z));
		AppendU32(output, header.width); AppendU32(output, header.height); AppendU16(output, header.weightGroup); AppendU16(output, 0u);
		AppendU64(output, header.uncompressedBytes); AppendU64(output, header.compressedBytes); AppendHash(output, header.payloadHash); AppendHash(output, clearEnvelopeHash ? ContentHash{} : header.envelopeHash); AppendU32(output, kChunkCount);
		// DATA chunk describes the one mandatory canonical payload. Its offset is
		// checked before any decoder allocation.
		output.insert(output.end(), {'D','A','T','A'}); AppendU32(output, 1u); AppendU64(output, kHeaderBytes + kChunkEntryBytes); AppendU64(output, header.compressedBytes); AppendU64(output, header.uncompressedBytes); AppendHash(output, header.payloadHash);
	}

	TerrainCodecResult ReadHeader(std::span<const std::uint8_t> input, Format::TerrainAreaEnvelopeHeader& header, std::uint32_t& outHeaderBytes)
	{
		if (input.size() < kHeaderBytes + kChunkEntryBytes || !std::equal(Format::kTerrainAreaMagic.begin(), Format::kTerrainAreaMagic.end(), input.begin())) return Fail(TerrainResultCode::CorruptData, "Terrain area magic is missing.");
		input = input.subspan(8u); std::uint32_t endian = 0u, chunkCount = 0u; std::uint16_t kind = 0u, compression = 0u, ignored = 0u;
		if (!ConsumeU16(input, header.majorVersion) || !ConsumeU16(input, header.minorVersion) || !ConsumeU32(input, outHeaderBytes) || !ConsumeU32(input, endian) || !ConsumeU16(input, kind) || !ConsumeU16(input, compression)) return Fail(TerrainResultCode::CorruptData, "Terrain area header is truncated.");
		if (header.majorVersion != Format::kTerrainAreaMajorVersion || header.minorVersion > Format::kTerrainAreaMinorVersion) return Fail(TerrainResultCode::UnsupportedVersion, "Terrain area version is unsupported.");
		if (endian != Format::kTerrainAreaEndianMarker || outHeaderBytes != kHeaderBytes + kChunkEntryBytes) return Fail(TerrainResultCode::CorruptData, "Terrain area header size or endianness is invalid.");
		header.channelKind = static_cast<TerrainChannelKind>(kind); header.compression = static_cast<Format::TerrainAreaCompression>(compression);
		const bool knownChannel = header.channelKind == TerrainChannelKind::Height
			|| header.channelKind == TerrainChannelKind::Holes
			|| header.channelKind == TerrainChannelKind::WeightGroup;
		const bool knownCompression = header.compression == Format::TerrainAreaCompression::None
			|| header.compression == Format::TerrainAreaCompression::Zstd;
		if (!knownChannel || !knownCompression) return Fail(TerrainResultCode::CorruptData, "Terrain area has an unknown required encoding.");
		if (input.size() < header.assetUuid.size()) return Fail(TerrainResultCode::CorruptData, "Terrain area UUID is truncated.");
		std::copy_n(input.begin(), header.assetUuid.size(), header.assetUuid.begin()); input = input.subspan(header.assetUuid.size()); std::uint64_t areaX = 0u, areaZ = 0u;
		if (!ConsumeU64(input, areaX) || !ConsumeU64(input, areaZ) || !ConsumeU32(input, header.width) || !ConsumeU32(input, header.height) || !ConsumeU16(input, header.weightGroup) || !ConsumeU16(input, ignored) || !ConsumeU64(input, header.uncompressedBytes) || !ConsumeU64(input, header.compressedBytes) || !ConsumeHash(input, header.payloadHash) || !ConsumeHash(input, header.envelopeHash) || !ConsumeU32(input, chunkCount)) return Fail(TerrainResultCode::CorruptData, "Terrain area scalar header is truncated.");
		header.area = {static_cast<std::int64_t>(areaX), static_cast<std::int64_t>(areaZ)};
		if (chunkCount != kChunkCount) return Fail(TerrainResultCode::CorruptData, "Terrain area requires exactly one DATA chunk in V1.");
		std::array<std::uint8_t, 4u> type{}; if (input.size() < type.size()) return Fail(TerrainResultCode::CorruptData, "Terrain area chunk table is truncated."); std::copy_n(input.begin(), type.size(), type.begin()); input = input.subspan(type.size()); std::uint32_t flags = 0u; std::uint64_t offset = 0u, stored = 0u, decoded = 0u; ContentHash chunkHash{};
		if (!ConsumeU32(input, flags) || !ConsumeU64(input, offset) || !ConsumeU64(input, stored) || !ConsumeU64(input, decoded) || !ConsumeHash(input, chunkHash)) return Fail(TerrainResultCode::CorruptData, "Terrain area chunk record is truncated.");
		if (type != std::array<std::uint8_t, 4u>{'D','A','T','A'} || flags != 1u || offset != outHeaderBytes || stored != header.compressedBytes || decoded != header.uncompressedBytes || chunkHash != header.payloadHash) return Fail(TerrainResultCode::CorruptData, "Terrain area DATA chunk does not match its header.");
		return {};
	}
}

	ContentHash HashTerrainBytes(std::span<const std::uint8_t> bytes) noexcept
	{
		const XXH128_hash_t result = XXH3_128bits(bytes.data(), bytes.size());
		return {result.low64, result.high64};
	}

	bool TerrainWeightSample::IsCanonical() const noexcept
	{
		std::uint32_t total = 0u;
		std::uint16_t previousLayer = 0u;
		std::uint8_t previousWeight = 255u;
		bool sawEmpty = false;
		for (const TerrainWeightSlot& slot : slots)
		{
			if ((slot.layerId == 0u) != (slot.weight == 0u)) return false;
			if (slot.weight == 0u) { sawEmpty = true; continue; }
			if (sawEmpty || slot.weight > previousWeight || (slot.weight == previousWeight && slot.layerId <= previousLayer)) return false;
			previousLayer = slot.layerId;
			previousWeight = slot.weight;
			total += slot.weight;
		}
		return total == 0u || total == 255u;
	}

	TerrainWeightSample NormalizeTerrainWeights(std::span<const TerrainWeightContribution> contributions)
	{
		std::vector<TerrainWeightContribution> combined;
		for (const TerrainWeightContribution& value : contributions)
		{
			if (value.layerId == 0u || value.weight == 0u) continue;
			auto prior = std::find_if(combined.begin(), combined.end(), [&value](const auto& existing) { return existing.layerId == value.layerId; });
			if (prior == combined.end()) combined.push_back(value);
			else prior->weight = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(), static_cast<std::uint64_t>(prior->weight) + value.weight));
		}
		std::sort(combined.begin(), combined.end(), [](const auto& left, const auto& right)
		{
			return left.weight != right.weight ? left.weight > right.weight : left.layerId < right.layerId;
		});
		if (combined.size() > 4u) combined.resize(4u);
		TerrainWeightSample result{};
		std::uint64_t total = 0u;
		for (const auto& value : combined) total += value.weight;
		if (total == 0u) return result;
		std::array<std::uint32_t, 4u> scaled{};
		std::uint32_t assigned = 0u;
		for (std::size_t index = 0u; index < combined.size(); ++index)
		{
			scaled[index] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(combined[index].weight) * 255u) / total);
			assigned += scaled[index];
			result.slots[index].layerId = combined[index].layerId;
		}
		// Give rounding remainder to the already deterministic highest-ranked
		// contributors, retaining both stable order and an exact sum of 255.
		for (std::uint32_t remainder = 255u - assigned, index = 0u; remainder != 0u; --remainder, index = (index + 1u) % static_cast<std::uint32_t>(combined.size())) ++scaled[index];
		for (std::size_t index = 0u; index < combined.size(); ++index) result.slots[index].weight = static_cast<std::uint8_t>(scaled[index]);
		return result;
	}

	TerrainCodecResult EncodeTerrainWeightSample(const TerrainWeightSample& sample, std::array<std::uint8_t, 12u>& outBytes)
	{
		if (!sample.IsCanonical()) return Fail(TerrainResultCode::InvalidArgument, "Terrain weight samples must be canonical before encoding.");
		outBytes.fill(0u);
		for (std::size_t slot = 0u; slot < sample.slots.size(); ++slot)
		{
			outBytes[slot * 2u] = static_cast<std::uint8_t>(sample.slots[slot].layerId);
			outBytes[slot * 2u + 1u] = static_cast<std::uint8_t>(sample.slots[slot].layerId >> 8u);
			outBytes[8u + slot] = sample.slots[slot].weight;
		}
		return {};
	}

	TerrainCodecResult DecodeTerrainWeightSample(std::span<const std::uint8_t, 12u> bytes, TerrainWeightSample& outSample)
	{
		outSample = {};
		for (std::size_t slot = 0u; slot < outSample.slots.size(); ++slot)
		{
			outSample.slots[slot].layerId = static_cast<std::uint16_t>(bytes[slot * 2u]) | (static_cast<std::uint16_t>(bytes[slot * 2u + 1u]) << 8u);
			outSample.slots[slot].weight = bytes[8u + slot];
		}
		return outSample.IsCanonical() ? TerrainCodecResult{} : Fail(TerrainResultCode::CorruptData, "Terrain weight sample bytes are not canonical.");
	}

	TerrainCodecResult MaterializeTerrainWeightGroup(std::span<const TerrainWeightSample> samples, std::vector<std::uint8_t>& outBytes)
	{
		outBytes.clear();
		if (samples.size() != kTerrainAreaSampleCount) return Fail(TerrainResultCode::InvalidArgument, "A terrain weight group requires exactly one canonical sample per area texel.");
		outBytes.resize(samples.size() * 12u);
		for (std::size_t index = 0u; index < samples.size(); ++index)
		{
			std::array<std::uint8_t, 12u> encoded{};
			if (const auto result = EncodeTerrainWeightSample(samples[index], encoded); !result.Succeeded()) { outBytes.clear(); return result; }
			std::copy(encoded.begin(), encoded.end(), outBytes.begin() + static_cast<std::ptrdiff_t>(index * encoded.size()));
		}
		return {};
	}

	TerrainCodecResult EncodeTerrainArea(const Format::TerrainAreaEnvelopeHeader& requestedHeader, std::span<const std::uint8_t> canonicalPayload, EncodedTerrainArea& outArea, TerrainCodecLimits limits)
	{
		outArea = {}; Format::TerrainAreaEnvelopeHeader header = requestedHeader;
		if (canonicalPayload.size() > limits.maximumDecodedBytes) return Fail(TerrainResultCode::BudgetExceeded, "Terrain area payload exceeds its configured decode budget.");
		if (auto validation = ValidateCanonicalPayload(header, canonicalPayload); !validation.Succeeded()) return validation;
		header.uncompressedBytes = canonicalPayload.size(); header.payloadHash = HashTerrainBytes(canonicalPayload); std::vector<std::uint8_t> stored(canonicalPayload.begin(), canonicalPayload.end());
		if (header.compression == Format::TerrainAreaCompression::Zstd)
		{
			stored.resize(ZSTD_compressBound(canonicalPayload.size())); const auto size = ZSTD_compress(stored.data(), stored.size(), canonicalPayload.data(), canonicalPayload.size(), 1);
			if (ZSTD_isError(size) != 0u) return Fail(TerrainResultCode::Failed, "Terrain area zstd compression failed.");
			stored.resize(size); if (stored.size() >= canonicalPayload.size()) { stored.assign(canonicalPayload.begin(), canonicalPayload.end()); header.compression = Format::TerrainAreaCompression::None; }
		}
		if (stored.size() > limits.maximumCompressedBytes) return Fail(TerrainResultCode::BudgetExceeded, "Terrain area compressed payload exceeds its configured budget.");
		header.compressedBytes = stored.size(); std::vector<std::uint8_t> bytes; bytes.reserve(kHeaderBytes + kChunkEntryBytes + stored.size()); AppendHeader(bytes, header, true); bytes.insert(bytes.end(), stored.begin(), stored.end()); header.envelopeHash = HashTerrainBytes(bytes); bytes.clear(); AppendHeader(bytes, header, false); bytes.insert(bytes.end(), stored.begin(), stored.end()); outArea.header = header; outArea.bytes = std::move(bytes); return {};
	}

	TerrainCodecResult DecodeTerrainArea(std::span<const std::uint8_t> encodedBytes, DecodedTerrainArea& outArea, TerrainCodecLimits limits)
	{
		outArea = {}; if (encodedBytes.size() > limits.maximumCompressedBytes + kHeaderBytes + kChunkEntryBytes) return Fail(TerrainResultCode::BudgetExceeded, "Terrain area file exceeds its configured compressed budget.");
		Format::TerrainAreaEnvelopeHeader header{}; std::uint32_t headerBytes = 0u; if (auto read = ReadHeader(encodedBytes, header, headerBytes); !read.Succeeded()) return read;
		if (header.uncompressedBytes > limits.maximumDecodedBytes || header.compressedBytes > limits.maximumCompressedBytes || headerBytes > encodedBytes.size() || header.compressedBytes != encodedBytes.size() - headerBytes) return Fail(TerrainResultCode::BudgetExceeded, "Terrain area declared ranges exceed their configured budgets.");
		std::vector<std::uint8_t> hashBytes(encodedBytes.begin(), encodedBytes.end()); std::fill(hashBytes.begin() + kEnvelopeHashOffset, hashBytes.begin() + kEnvelopeHashOffset + 16u, 0u); if (HashTerrainBytes(hashBytes) != header.envelopeHash) return Fail(TerrainResultCode::HashMismatch, "Terrain area envelope identity does not match its bytes.");
		std::vector<std::uint8_t> payload(static_cast<std::size_t>(header.uncompressedBytes)); const auto stored = encodedBytes.subspan(headerBytes);
		if (header.compression == Format::TerrainAreaCompression::None) { if (stored.size() != payload.size()) return Fail(TerrainResultCode::CorruptData, "Uncompressed terrain area length is invalid."); std::copy(stored.begin(), stored.end(), payload.begin()); }
		else { const auto decoded = ZSTD_decompress(payload.data(), payload.size(), stored.data(), stored.size()); if (ZSTD_isError(decoded) != 0u || decoded != payload.size()) return Fail(TerrainResultCode::CorruptData, "Terrain area zstd payload failed bounded decoding."); }
		if (HashTerrainBytes(payload) != header.payloadHash) return Fail(TerrainResultCode::HashMismatch, "Terrain area payload identity does not match its bytes.");
		if (auto validation = ValidateCanonicalPayload(header, payload); !validation.Succeeded()) return validation;
		outArea.header = header; outArea.canonicalPayload = std::move(payload); return {};
	}
}
