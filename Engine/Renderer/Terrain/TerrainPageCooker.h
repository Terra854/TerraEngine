// TerrainPageCooker.h -- Deterministic authored-height to terrain-page cooking.

#pragma once

#include "Core/Export.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"
#include "Terrain/TerrainTypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	enum class TerrainPageEdge : std::uint8_t { West, East, South, North };

	struct TerrainCookInput
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		double sampleSpacingMetres = 1.0;
		TerraEngine::Terrain::TerrainAreaActiveRect activeRect{0u, 0u, 256u, 256u};
		std::span<const std::int32_t> heightMillimetres{};
		// One bit per authoritative cell. Empty input means no holes for backwards
		// compatible callers; a supplied mask must contain exactly 256 by 256 bits.
		std::span<const std::uint8_t> holeCellBits{};
		// Four exact neighbor hashes are part of every derived identity. An absent
		// neighbor uses an invalid hash and selects a conservative boundary stitch.
		std::array<TerraEngine::Terrain::ContentHash, 4u> neighborAuthoredHashes{};
	};

	struct TerrainPageCookResult
	{
		bool complete = false;
		std::string diagnostic;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		float minimumHeightMetres = 0.0f;
		float maximumHeightMetres = 0.0f;
		float maximumGeometricErrorMetres = 0.0f;
		// Conservative bounds are independent worker outputs consumed by page
		// selection; they never become authoritative terrain height storage.
		struct ConservativeBounds
		{
			float minimumHeightMetres = 0.0f;
			float maximumHeightMetres = 0.0f;
			float maximumGeometricErrorMetres = 0.0f;
		};
		struct ErrorPyramidLevel
		{
			std::uint16_t width = 0u;
			std::uint16_t height = 0u;
			std::vector<float> errors;
			std::vector<ConservativeBounds> bounds;
		};
		struct MeshletRecord
		{
			std::uint32_t meshletId = 0u;
			// Spatial ownership is explicit so page decoding never has to infer
			// which quadtree node produced a meshlet from payload ordering.
			std::uint32_t nodeId = 0u;
			std::uint16_t minX = 0u;
			std::uint16_t minZ = 0u;
			std::uint16_t maxXExclusive = 0u;
			std::uint16_t maxZExclusive = 0u;
			std::uint16_t renderedCellWidth = 0u;
			std::uint16_t renderedCellHeight = 0u;
			std::uint32_t vertexCount = 0u;
			std::uint32_t triangleCount = 0u;
			ConservativeBounds bounds{};
			std::uint32_t firstPayloadByte = 0u;
			std::uint32_t payloadByteCount = 0u;
			bool hasHoleBoundary = false;
		};
		struct ClusterRecord
		{
			std::uint32_t clusterId = 0u;
			std::uint32_t parentClusterId = 0xffffffffu;
			std::uint32_t firstMeshlet = 0u;
			std::uint32_t meshletCount = 0u;
			ConservativeBounds bounds{};
		};
		struct PageRange
		{
			std::uint32_t pageId = 0u;
			std::uint32_t firstByte = 0u;
			std::uint32_t byteCount = 0u;
			TerraEngine::Terrain::ContentHash payloadIdentity{};
		};
		struct LodNode
		{
			std::uint32_t nodeId = 0u;
			std::uint32_t parentNodeId = 0xffffffffu;
			std::uint16_t lodLevel = 0u;
			std::uint16_t cellOriginX = 0u;
			std::uint16_t cellOriginZ = 0u;
			std::uint16_t cellExtent = 0u;
			std::uint32_t firstMeshlet = 0u;
			std::uint32_t meshletCount = 0u;
			ConservativeBounds bounds{};
			// The stored geometric bound includes both values. Keeping them
			// separately lets diagnostics prove the conservative-error equation.
			float maximumQuantizationErrorMetres = 0.0f;
			float maximumMorphDisplacementMetres = 0.0f;
			float morphStartMetres = 0.0f;
			float morphEndMetres = 0.0f;
			bool refinesHoleBoundary = false;
		};
		struct EdgeStitchVariant
		{
			std::uint32_t meshletId = 0u;
			TerrainPageEdge edge = TerrainPageEdge::West;
			std::int8_t neighborLodDelta = 0;
			bool conservativeBoundary = false;
			std::uint8_t stitchMask = 0u;
			std::uint32_t firstIndex = 0u;
			std::uint32_t indexCount = 0u;
		};
		std::vector<ErrorPyramidLevel> errorPyramid;
		std::vector<MeshletRecord> meshlets;
		std::vector<ClusterRecord> clusters;
		std::vector<LodNode> lodNodes;
		std::vector<EdgeStitchVariant> edgeStitchVariants;
		std::vector<PageRange> pageRanges;
		std::vector<std::uint16_t> packedNormals;
		std::vector<std::uint16_t> stitchIndices;
		std::vector<std::uint8_t> pagePayload;
		TerraEngine::Terrain::ContentHash sourceIdentity{};
		TerraEngine::Terrain::ContentHash neighborDependencyHash{};
	};

	// Worker-safe: input and output are copied values and it touches no renderer
	// or residency state. The protected VG publication boundary owns visibility.
	TERRAENGINE_API TerrainPageCookResult CookTerrainPage(const TerrainCookInput& input);
	// Neighboring cuts may differ by one level only. This small rule means every
	// requested edge has a precomputed stitch or a parent fallback.
	TERRAENGINE_API bool AreTerrainLodLevelsCompatible(std::uint16_t first, std::uint16_t second) noexcept;
}
