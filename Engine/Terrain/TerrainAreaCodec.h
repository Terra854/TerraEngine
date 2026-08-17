// TerrainAreaCodec.h -- Canonical, content-addressed .tearea channel codec.

#pragma once

#include "File Formats/TerraEngineTerrainFile.h"

#include <span>
#include <array>
#include <string>
#include <vector>

namespace TerraEngine::Terrain
{
	struct TerrainCodecLimits
	{
		std::size_t maximumCompressedBytes = Format::kTerrainAreaMaximumCompressedBytes;
		std::size_t maximumDecodedBytes = Format::kTerrainAreaMaximumDecodedBytes;
	};

	struct TerrainCodecResult
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		std::string message;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok; }
	};

	struct EncodedTerrainArea
	{
		Format::TerrainAreaEnvelopeHeader header{};
		std::vector<std::uint8_t> bytes;
	};

	struct DecodedTerrainArea
	{
		Format::TerrainAreaEnvelopeHeader header{};
		std::vector<std::uint8_t> canonicalPayload;
	};

	// A weight sample uses stable layer IDs, not stack indices. This keeps paint
	// valid when artists reorder metadata or temporarily lose a material source.
	struct TerrainWeightSlot
	{
		std::uint16_t layerId = 0u;
		std::uint8_t weight = 0u;
		[[nodiscard]] constexpr bool operator==(const TerrainWeightSlot&) const noexcept = default;
	};
	struct TerrainWeightSample
	{
		std::array<TerrainWeightSlot, 4u> slots{};
		[[nodiscard]] constexpr bool operator==(const TerrainWeightSample&) const noexcept = default;
		[[nodiscard]] bool IsCanonical() const noexcept;
	};
	struct TerrainWeightContribution { std::uint16_t layerId = 0u; std::uint32_t weight = 0u; };

	TERRAENGINE_API TerrainWeightSample NormalizeTerrainWeights(std::span<const TerrainWeightContribution> contributions);
	TERRAENGINE_API TerrainCodecResult EncodeTerrainWeightSample(const TerrainWeightSample& sample, std::array<std::uint8_t, 12u>& outBytes);
	TERRAENGINE_API TerrainCodecResult DecodeTerrainWeightSample(std::span<const std::uint8_t, 12u> bytes, TerrainWeightSample& outSample);
	TERRAENGINE_API TerrainCodecResult MaterializeTerrainWeightGroup(std::span<const TerrainWeightSample> samples, std::vector<std::uint8_t>& outBytes);

	TERRAENGINE_API ContentHash HashTerrainBytes(std::span<const std::uint8_t> bytes) noexcept;
	TERRAENGINE_API TerrainCodecResult EncodeTerrainArea(
		const Format::TerrainAreaEnvelopeHeader& requestedHeader,
		std::span<const std::uint8_t> canonicalPayload,
		EncodedTerrainArea& outArea,
		TerrainCodecLimits limits = {});
	TERRAENGINE_API TerrainCodecResult DecodeTerrainArea(
		std::span<const std::uint8_t> encodedBytes,
		DecodedTerrainArea& outArea,
		TerrainCodecLimits limits = {});
}
