// TerraEngineTerrainFile.h -- Versioned, bounded .teterrain and .tearea contracts.

#pragma once

#include "Terrain/TerrainTypes.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Terrain::Format
{
	inline constexpr std::string_view kTerrainRootFormatName = "TerraEngineTerrain";
	inline constexpr std::string_view kTerrainHashAlgorithmName = "xxh3-128-v1";
	inline constexpr std::uint16_t kTerrainRootMajorVersion = 1u;
	inline constexpr std::uint16_t kTerrainRootMinorVersion = 0u;
	inline constexpr std::size_t kTerrainMaximumDirectoryShards = 1u << 20u;
	inline constexpr std::size_t kTerrainMaximumDirectoryEntriesPerShard = 1u << 20u;
	inline constexpr std::size_t kTerrainMaximumLayers = 64u;
	inline constexpr std::array<char, 8u> kTerrainAreaMagic = {'T','E','A','R','E','A','0','1'};
	inline constexpr std::uint16_t kTerrainAreaMajorVersion = 1u;
	inline constexpr std::uint16_t kTerrainAreaMinorVersion = 0u;
	inline constexpr std::uint32_t kTerrainAreaEndianMarker = 0x01020304u;
	inline constexpr std::size_t kTerrainAreaMaximumHeaderBytes = 64u * 1024u;
	inline constexpr std::size_t kTerrainAreaMaximumChunkCount = 32u;
	inline constexpr std::size_t kTerrainAreaMaximumCompressedBytes = 16u * 1024u * 1024u;
	inline constexpr std::size_t kTerrainAreaMaximumDecodedBytes = 2u * 1024u * 1024u;

	enum class TerrainAreaCompression : std::uint16_t { None = 0u, Zstd = 1u };

	struct TerrainFormatVersion
	{
		std::uint16_t major = kTerrainRootMajorVersion;
		std::uint16_t minor = kTerrainRootMinorVersion;
		[[nodiscard]] constexpr bool IsSupported() const noexcept
		{
			return major == kTerrainRootMajorVersion && minor <= kTerrainRootMinorVersion;
		}
	};

	// Directory shards use half-open area bounds just like sample edits. Keeping
	// the contract signed preserves exact addressing on both sides of the origin.
	struct TerrainAreaBounds
	{
		std::int64_t minX = 0;
		std::int64_t minZ = 0;
		std::int64_t maxXExclusive = 0;
		std::int64_t maxZExclusive = 0;
		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return minX < maxXExclusive && minZ < maxZExclusive;
		}
	};

	struct TerrainPredecessorReference
	{
		Generation generation = 0u;
		ContentHash rootHash{};
		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return generation != 0u && rootHash.IsValid();
		}
	};

	struct TerrainDirectoryShardReference
	{
		TerrainAreaBounds bounds{};
		ContentHash directoryHash{};
		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return bounds.IsValid() && directoryHash.IsValid();
		}
	};

	// This provider-neutral value mirrors terrain-asset-v1.schema.json. JSON IO
	// is intentionally kept outside the contract so runtime mounts and editor
	// saves can share the same validated representation.
	struct TerrainRootManifestContract
	{
		TerrainFormatVersion version{};
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		Generation persistedRootGeneration = 0u;
		std::optional<TerrainPredecessorReference> predecessor;
		double sampleSpacingMetres = 1.0;
		double heightDatumMetres = 0.0;
		std::int32_t defaultHeightMm = 0;
		TerrainSampleBounds authoredBounds{};
		std::uint16_t defaultLayerId = 1u;
		Generation layerStackGeneration = 0u;
		std::size_t layerCount = 1u;
		std::vector<TerrainDirectoryShardReference> directoryShards;

		[[nodiscard]] bool IsStructurallyValid() const noexcept
		{
			if (!version.IsSupported() || assetUuid == TerraEngine::EMPTY_UUID
				|| persistedRootGeneration == 0u || !std::isfinite(sampleSpacingMetres)
				|| sampleSpacingMetres <= 0.0 || !std::isfinite(heightDatumMetres)
				|| !authoredBounds.IsValid() || defaultLayerId == 0u
				|| layerStackGeneration == 0u || layerCount == 0u
				|| layerCount > kTerrainMaximumLayers
				|| directoryShards.size() > kTerrainMaximumDirectoryShards)
			{
				return false;
			}
			if (predecessor.has_value() && !predecessor->IsValid()) return false;
			for (const TerrainDirectoryShardReference& shard : directoryShards)
				if (!shard.IsValid()) return false;
			return true;
		}
	};

	struct TerrainDirectoryEntryContract
	{
		TerrainAreaCoordinate coordinate{};
		Generation editGeneration = 0u;
		ContentHash authoredContentHash{};
		ContentHash heightHash{};
		std::optional<ContentHash> holeHash;
		std::vector<std::pair<std::uint16_t, ContentHash>> weightGroupHashes;
		std::int32_t minimumHeightMm = 0;
		std::int32_t maximumHeightMm = 0;

		[[nodiscard]] bool IsStructurallyValid() const noexcept
		{
			if (editGeneration == 0u || !authoredContentHash.IsValid()
				|| !heightHash.IsValid() || minimumHeightMm > maximumHeightMm
				|| weightGroupHashes.size() > kTerrainMaximumWeightGroups)
			{
				return false;
			}
			if (holeHash.has_value() && !holeHash->IsValid()) return false;
			std::uint16_t previousGroup = 0u;
			for (const auto& [group, hash] : weightGroupHashes)
			{
				if (group == 0u || group <= previousGroup || !hash.IsValid()) return false;
				previousGroup = group;
			}
			return true;
		}
	};

	struct TerrainDirectoryShardContract
	{
		TerrainFormatVersion version{};
		TerrainAreaBounds bounds{};
		ContentHash identity{};
		std::vector<TerrainDirectoryEntryContract> entries;

		[[nodiscard]] bool IsStructurallyValid() const noexcept
		{
			if (!version.IsSupported() || !bounds.IsValid() || !identity.IsValid()
				|| entries.size() > kTerrainMaximumDirectoryEntriesPerShard)
			{
				return false;
			}
			for (const TerrainDirectoryEntryContract& entry : entries)
			{
				if (!entry.IsStructurallyValid()
					|| entry.coordinate.x < bounds.minX || entry.coordinate.x >= bounds.maxXExclusive
					|| entry.coordinate.z < bounds.minZ || entry.coordinate.z >= bounds.maxZExclusive)
				{
					return false;
				}
			}
			return true;
		}
	};

	// This is an in-memory description, never a packed C++ wire layout. The
	// codec appends every member explicitly in little-endian order.
	struct TerrainAreaEnvelopeHeader
	{
		std::uint16_t majorVersion = kTerrainAreaMajorVersion;
		std::uint16_t minorVersion = kTerrainAreaMinorVersion;
		TerrainChannelKind channelKind = TerrainChannelKind::Height;
		TerrainAreaCompression compression = TerrainAreaCompression::Zstd;
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		TerrainAreaCoordinate area{};
		std::uint32_t width = static_cast<std::uint32_t>(kTerrainAreaExtentSamples);
		std::uint32_t height = static_cast<std::uint32_t>(kTerrainAreaExtentSamples);
		std::uint16_t weightGroup = 0u;
		std::uint64_t uncompressedBytes = 0u;
		std::uint64_t compressedBytes = 0u;
		ContentHash payloadHash{};
		ContentHash envelopeHash{};
	};
}
