// TerrainHeightFieldActiveRect.cpp -- checked active-cell masking for fixed terrain recipes.

#include "Physics/Terrain/TerrainHeightFieldActiveRect.h"

namespace TerraEngine::Physics::Terrain {
	bool ApplyTerrainActiveRectToRecipe(
		TerrainHeightFieldRecipe& recipe,
		const TerraEngine::Terrain::TerrainAreaActiveRect& activeRect) noexcept {
		constexpr std::uint32_t kCellExtent = TerrainHeightFieldRecipe::kGridExtent - 1U;
		// Validate every dimension before copying or changing the caller's recipe.
		// `max*Exclusive` addresses samples, therefore it cannot exceed the fixed
		// 256-sample authored storage even though the provider has a 257th seam sample.
		if(!recipe.IsComplete() || !activeRect.IsValid()
			|| activeRect.maxXExclusive > kCellExtent
			|| activeRect.maxZExclusive > kCellExtent) return false;

		for(std::uint32_t z = 0U; z < kCellExtent; ++z) {
			for(std::uint32_t x = 0U; x < kCellExtent; ++x) {
				// A cell needs both of its endpoint samples inside the authored rectangle.
				// OR the generated bit so a user-authored hole remains a hole after padding.
				if(x >= activeRect.minX && z >= activeRect.minZ
					&& x + 1U < activeRect.maxXExclusive && z + 1U < activeRect.maxZExclusive) continue;
				const std::uint32_t bit = z * kCellExtent + x;
				recipe.holeBits[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
			}
		}
		return true;
	}
}
