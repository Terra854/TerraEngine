// TerrainLodSelection.h -- immutable per-view terrain cut selection contracts.

#pragma once

#include "Core/Export.h"
#include "Resource/VirtualGeometryPagePublication.h"
#include "Terrain/TerrainTypes.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	enum class TerrainLodProjection : std::uint8_t { Perspective, Orthographic };

	struct TerrainLodView
	{
		TerrainLodProjection projection = TerrainLodProjection::Perspective;
		glm::dvec3 cameraPositionMetres{0.0};
		glm::dvec3 forward{0.0, 0.0, -1.0};
		double viewportHeightPixels = 1.0;
		double verticalFovRadians = 1.0;
		double orthographicHeightMetres = 1.0;
	};

	[[nodiscard]] TERRAENGINE_API double ComputeTerrainProjectedErrorPixels(
		double geometricErrorMetres, double viewDepthMetres,
		const TerrainLodView& view) noexcept;

	inline constexpr std::uint32_t kInvalidTerrainLodNodeId = 0xffffffffu;
	struct TerrainLodNodeKey
	{
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		std::uint32_t nodeId = kInvalidTerrainLodNodeId;
		[[nodiscard]] constexpr bool operator==(const TerrainLodNodeKey&) const noexcept = default;
	};

	struct TerrainLodSelectionNode
	{
		TerrainLodNodeKey key{};
		std::uint32_t parentNodeId = kInvalidTerrainLodNodeId;
		std::array<std::uint32_t, 4u> childNodeIds{
			kInvalidTerrainLodNodeId, kInvalidTerrainLodNodeId,
			kInvalidTerrainLodNodeId, kInvalidTerrainLodNodeId};
		std::uint16_t lodLevel = 0u;
		glm::dvec3 boundsMinMetres{0.0};
		glm::dvec3 boundsMaxMetres{0.0};
		double geometricErrorMetres = 0.0;
		bool resident = false;
		bool crossesHoleBoundary = false;
	};

	struct TerrainLodSelectionSettings
	{
		double targetErrorPixels = 1.5;
		double hysteresisPixels = 0.25;
		std::uint16_t maximumNeighborLevelDelta = 1u;
	};

	struct TerrainLodSelectedNode
	{
		TerrainLodNodeKey key{};
		std::uint16_t lodLevel = 0u;
		double projectedErrorPixels = 0.0;
		double morphFactor = 0.0;
		std::uint8_t stitchMask = 0u;
		bool holePinned = false;
	};

	struct TerrainLodSelectionRequest
	{
		TerrainLodView view{};
		std::vector<TerrainLodSelectionNode> nodes;
		std::vector<TerrainLodNodeKey> previousCut;
		TerrainLodSelectionSettings settings{};
	};

	struct TerrainLodSelectionResult
	{
		std::vector<TerrainLodSelectedNode> selectedNodes;
		bool complete = false;
		bool balanced = false;
		bool changedFromPrevious = false;
	};

	[[nodiscard]] TERRAENGINE_API TerrainLodSelectionResult SelectTerrainLodCut(
		const TerrainLodSelectionRequest& request);

	enum TerrainStitchMaskBits : std::uint8_t
	{
		TerrainStitchNorth = 1u,
		TerrainStitchEast = 2u,
		TerrainStitchSouth = 4u,
		TerrainStitchWest = 8u
	};

	[[nodiscard]] TERRAENGINE_API std::uint8_t BuildTerrainStitchMask(
		std::uint16_t selectedLevel,
		const std::array<std::uint16_t, 4u>& neighborLevels) noexcept;

	struct TerrainMorphEndpoints
	{
		double parentHeightMetres = 0.0;
		double childHeightMetres = 0.0;
		glm::dvec3 parentNormal{0.0, 1.0, 0.0};
		glm::dvec3 childNormal{0.0, 1.0, 0.0};
	};

	struct TerrainMorphedVertex
	{
		double heightMetres = 0.0;
		glm::dvec3 normal{0.0, 1.0, 0.0};
	};

	[[nodiscard]] TERRAENGINE_API TerrainMorphedVertex EvaluateTerrainMorph(
		const TerrainMorphEndpoints& endpoints, double factor) noexcept;

	inline constexpr std::uint32_t kInvalidTerrainPageId = 0xffffffffu;

	struct TerrainPageResidencyEntry
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		std::uint32_t pageId = kInvalidTerrainPageId;
		std::uint32_t parentPageId = kInvalidTerrainPageId;
		TerraEngine::Resource::VirtualGeometryPagePublicationState state =
			TerraEngine::Resource::VirtualGeometryPagePublicationState::Missing;
		bool uploadCompleted = false;
		bool published = false;
		bool pinnedCoarseCoverage = false;
		std::uint64_t retireAfterFrame = 0u;
	};

	struct TerrainPageSelectionRequest
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		std::uint32_t desiredPageId = kInvalidTerrainPageId;
		std::uint32_t priority = 0u;
	};

	struct TerrainPageResolution
	{
		std::uint32_t desiredPageId = kInvalidTerrainPageId;
		std::uint32_t actualPageId = kInvalidTerrainPageId;
		std::uint16_t fallbackDepth = 0u;
		bool desiredResident = false;
		bool pinnedCoarseCoverage = false;
		bool completeCoverage = false;
	};

	struct TerrainPageDemand
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		std::uint32_t pageId = kInvalidTerrainPageId;
		std::uint32_t priority = 0u;
		std::uint32_t requestingViewCount = 0u;
	};

	struct TerrainPageDemandResult
	{
		std::vector<TerrainPageResolution> resolutions;
		std::vector<TerrainPageDemand> demands;
		bool completeCoverage = false;
	};

	[[nodiscard]] TERRAENGINE_API TerrainPageResolution ResolveTerrainOwnAncestorPage(
		const TerrainPageSelectionRequest& request,
		std::span<const TerrainPageResidencyEntry> entries) noexcept;
	[[nodiscard]] TERRAENGINE_API TerrainPageDemandResult BuildTerrainPageDemands(
		std::span<const TerrainPageSelectionRequest> requests,
		std::span<const TerrainPageResidencyEntry> entries);
}
