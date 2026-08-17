// TerrainRenderPackets.h -- Immutable terrain view, pick, and VG packet values.

#pragma once

#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Resource/VirtualGeometryPagePublication.h"

#include <optional>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	struct TerrainViewPacket
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		TerraEngine::Terrain::ContentHash pageSourceIdentity{};
		std::uint64_t authoredGeneration = 0u;
		std::uint32_t pageId = 0u;
		std::uint16_t requestedLod = 0u;
		// A complete-cut identity groups pages chosen for one view. It blocks a
		// renderer from mixing arbitrary resident detail with a regional fallback.
		TerraEngine::Terrain::ContentHash completeCutIdentity{};
		TerraEngine::Terrain::ContentHash neighborDependencyHash{};
		TerraEngine::Terrain::ContentHash surfaceControlIdentity{};
		std::uint64_t derivedGeneration = 0u;
		bool fallback = false;
	};

	struct TerrainPickIdentity
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		std::uint32_t localCellX = 0u;
		std::uint32_t localCellZ = 0u;
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		TerraEngine::Terrain::ContentHash pageSourceIdentity{};
		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct TerrainPreparedRenderPacket
	{
		TerrainViewPacket view{};
		TerrainPickIdentity pick{};
		TerraEngine::Resource::VirtualGeometryPagePublicationCandidate candidate{};
		bool usesLastKnownGood = false;
		bool usesRegionalFallback = false;
	};

	TERRAENGINE_API std::optional<TerrainPreparedRenderPacket> PrepareTerrainRenderPacket(
		const TerrainViewPacket& view,
		const TerrainPageCookResult& cooked,
		const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication>& priorVisiblePages);
}
