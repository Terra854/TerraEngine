// TerrainPageFormat.h -- versioned, bounded terrain virtual-geometry page ABI.

#pragma once

#include "Terrain/TerrainTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace TerraEngine::Renderer::Terrain::Format
{
	inline constexpr std::array<char, 8u> kMagic{'T', 'E', 'R', 'P', 'A', 'G', 'E', '1'};
	inline constexpr std::uint16_t kMajorVersion = 1u;
	inline constexpr std::uint16_t kMinorVersion = 0u;
	inline constexpr std::uint32_t kAreaCellExtent = 256u;
	inline constexpr std::uint32_t kCookHaloSampleExtent = 258u;
	inline constexpr std::uint32_t kRenderedNodeCellExtent = 32u;
	inline constexpr std::uint32_t kRenderedNodeSampleExtent = 33u;
	inline constexpr std::uint32_t kMaximumMeshletCellExtent = 7u;
	inline constexpr std::uint32_t kMaximumMeshletVertices = 64u;
	inline constexpr std::uint32_t kMaximumMeshletTriangles = 98u;
	inline constexpr std::uint32_t kStitchVariantCount = 16u;
	inline constexpr std::uint32_t kNominalPageBytes = 64u * 1024u;
	inline constexpr std::uint32_t kMaximumDecodedPageBytes = 256u * 1024u;
	inline constexpr std::uint32_t kInvalidIndex = 0xffffffffu;

	enum class TerrainPageFlags : std::uint32_t
	{
		None = 0u,
		ContainsHoleBoundary = 1u << 0u,
		MissingOuterHalo = 1u << 1u
	};

	struct TerrainPageHeader
	{
		std::array<char, 8u> magic = kMagic;
		std::uint16_t majorVersion = kMajorVersion;
		std::uint16_t minorVersion = kMinorVersion;
		// Encoders assign sizeof(TerrainPageHeader) once the type is complete.
		std::uint32_t headerBytes = 0u;
		std::uint32_t payloadBytes = 0u;
		std::uint32_t nodeCount = 0u;
		std::uint32_t meshletCount = 0u;
		std::uint32_t vertexCount = 0u;
		std::uint32_t triangleCount = 0u;
		std::uint32_t stitchVariantCount = kStitchVariantCount;
		std::uint32_t flags = 0u;
		std::uint64_t checksumLow = 0u;
		std::uint64_t checksumHigh = 0u;
	};

	struct TerrainHierarchyNode
	{
		std::uint32_t nodeId = kInvalidIndex;
		std::uint32_t parentNodeId = kInvalidIndex;
		std::array<std::uint32_t, 4u> childNodeIds{
			kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex};
		std::uint16_t originCellX = 0u;
		std::uint16_t originCellZ = 0u;
		std::uint16_t coveredCellExtent = 0u;
		std::uint16_t lodLevel = 0u;
		float minimumHeightMetres = 0.0F;
		float maximumHeightMetres = 0.0F;
		float maximumGeometricErrorMetres = 0.0F;
		std::uint32_t firstMeshlet = 0u;
		std::uint32_t meshletCount = 0u;
		std::uint32_t pageId = kInvalidIndex;
		std::uint32_t flags = 0u;
	};

	struct TerrainPackedVertex
	{
		std::uint16_t localX = 0u;
		std::uint16_t localZ = 0u;
		std::int32_t heightMillimetres = 0;
		std::int32_t parentHeightMillimetres = 0;
		std::uint16_t packedNormal = 0u;
		std::uint16_t packedParentNormal = 0u;
		std::uint16_t flags = 0u;
		std::uint16_t reserved = 0u;
	};

	struct TerrainMeshletRecord
	{
		std::uint32_t meshletId = kInvalidIndex;
		std::uint32_t nodeId = kInvalidIndex;
		std::uint32_t firstVertex = 0u;
		std::uint16_t vertexCount = 0u;
		std::uint16_t triangleCount = 0u;
		std::uint32_t firstTriangleByte = 0u;
		std::array<std::uint32_t, kStitchVariantCount> firstStitchIndex{};
		std::array<std::uint16_t, kStitchVariantCount> stitchIndexCount{};
		std::uint32_t flags = 0u;
	};

	struct TerrainMorphRecord
	{
		std::uint32_t nodeId = kInvalidIndex;
		float startProjectedErrorPixels = 0.0F;
		float endProjectedErrorPixels = 0.0F;
		float conservativeIntervalErrorMetres = 0.0F;
		std::uint32_t reserved = 0u;
	};

	struct TerrainPageDependency
	{
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		std::uint8_t presenceMask = 0u;
		std::array<std::uint8_t, 7u> reserved{};
	};

	struct TerrainPageDirectoryEntry
	{
		std::uint32_t pageId = kInvalidIndex;
		std::uint32_t firstByte = 0u;
		std::uint32_t byteCount = 0u;
		std::uint32_t firstNode = 0u;
		std::uint32_t nodeCount = 0u;
		std::uint32_t checksum = 0u;
	};

	enum class TerrainPageDecodeCode : std::uint8_t
	{
		Ok,
		Truncated,
		UnsupportedVersion,
		BudgetExceeded,
		InvalidCounts,
		ChecksumMismatch
	};

	struct TerrainPageDecodeResult
	{
		TerrainPageDecodeCode code = TerrainPageDecodeCode::Ok;
		std::size_t consumedBytes = 0u;
		[[nodiscard]] constexpr bool Succeeded() const noexcept
		{
			return code == TerrainPageDecodeCode::Ok;
		}
	};

	[[nodiscard]] inline TerrainPageDecodeResult ValidateTerrainPageHeader(
		const TerrainPageHeader& header, std::size_t availableBytes) noexcept
	{
		if(header.magic != kMagic) return {TerrainPageDecodeCode::UnsupportedVersion, 0u};
		if(header.majorVersion != kMajorVersion || header.minorVersion > kMinorVersion)
			return {TerrainPageDecodeCode::UnsupportedVersion, 0u};
		if(header.headerBytes != sizeof(TerrainPageHeader) || availableBytes < header.headerBytes)
			return {TerrainPageDecodeCode::Truncated, 0u};
		if(header.payloadBytes > kMaximumDecodedPageBytes)
			return {TerrainPageDecodeCode::BudgetExceeded, 0u};
		if(availableBytes - header.headerBytes < header.payloadBytes)
			return {TerrainPageDecodeCode::Truncated, 0u};
		if(header.nodeCount == 0u || header.meshletCount == 0u
			|| header.vertexCount < header.meshletCount
			|| header.stitchVariantCount != kStitchVariantCount)
			return {TerrainPageDecodeCode::InvalidCounts, 0u};
		return {TerrainPageDecodeCode::Ok,
			static_cast<std::size_t>(header.headerBytes) + header.payloadBytes};
	}
}
