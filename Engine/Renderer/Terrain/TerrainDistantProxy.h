// TerrainDistantProxy.h -- conservative regional terrain coverage when detail is absent.

#pragma once

#include "Renderer/Terrain/TerrainPageCooker.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	// This value represents a whole regional fallback cut. It cannot be confused
	// with an arbitrary resident detail page because it carries the root identity.
	struct TerrainDistantProxy
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate regionalRoot{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		std::uint32_t coveredAreaWidth = 0u;
		float minimumHeightMetres = 0.0f;
		float maximumHeightMetres = 0.0f;
		float conservativeErrorMetres = 0.0f;
		TerraEngine::Terrain::ContentHash identity{};
		[[nodiscard]] bool IsComplete() const noexcept;
	};

	// Builds a deterministic proxy from a complete regional cut. Missing input is
	// rejected rather than synthesising non-conservative distant terrain.
	TERRAENGINE_API TerrainDistantProxy BuildTerrainDistantProxy(
		TerraEngine::UUID assetId,
		TerraEngine::Terrain::TerrainAreaCoordinate regionalRoot,
		std::uint32_t coveredAreaWidth,
		std::span<const TerrainPageCookResult> pages);
}
