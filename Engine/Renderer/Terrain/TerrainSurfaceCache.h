// TerrainSurfaceCache.h -- bounded, generation-tagged terrain control-page residency.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainTypes.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	struct TerrainSurfacePageKey
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash sourceHash{};
		std::uint64_t authoredGeneration = 0u;
		[[nodiscard]] bool IsComplete() const noexcept;
	};
	struct TerrainSurfacePageHandle
	{
		TerrainSurfacePageKey key{};
		std::uint32_t atlasTile = 0xffffffffu;
		std::size_t byteCount = 0u;
		std::uint64_t publicationGeneration = 0u;
		bool lastKnownGood = false;
		bool protectedFromEviction = false;
		[[nodiscard]] bool IsResident() const noexcept;
	};
	struct TerrainSurfaceCacheRequest { TerrainSurfacePageKey key{}; std::size_t byteCount = 0u; std::uint32_t priority = 0u; bool protectedFromEviction = false; };
	struct TerrainSurfaceCachePlan { std::uint64_t observedGeneration = 0u; std::vector<TerrainSurfacePageHandle> candidate; std::vector<TerrainSurfacePageKey> evicted; bool cancelled = false; };

	TERRAENGINE_API TerrainSurfaceCachePlan PrepareTerrainSurfaceCachePlan(
		std::uint64_t observedGeneration, std::span<const TerrainSurfacePageHandle> prior,
		std::vector<TerrainSurfaceCacheRequest> requests, std::size_t budgetBytes, const std::atomic_bool* cancellation = nullptr);
	TERRAENGINE_API bool PublishTerrainSurfaceCachePlan(std::vector<TerrainSurfacePageHandle>& inOutVisible, std::uint64_t& inOutGeneration, const TerrainSurfaceCachePlan& plan, std::uint64_t currentGeneration);
}
