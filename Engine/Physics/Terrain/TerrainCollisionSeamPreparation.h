// Immutable same-root inputs for derived collision endpoints; authored storage stays 256.
#pragma once

#include "Physics/PhysicsTypes.h"
#include "Terrain/TerrainTypes.h"
#include <map>
#include <memory>

namespace TerraEngine::Physics::Terrain
{
	struct TerrainCollisionAreaCapture
	{
		TerraEngine::Terrain::TerrainAreaActiveRect active{};
		std::shared_ptr<const std::vector<std::int32_t>> heights;
	};
	struct TerrainCollisionRootCapture
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::ContentHash rootHash{};
		std::uint64_t generation = 0u;
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate, TerrainCollisionAreaCapture> areas;
	};
	// Worker-safe: reads only retained immutable captures, changes a detached recipe.
	// A missing endpoint disables its cell; no extrapolation creates collision.
	[[nodiscard]] TERRAENGINE_API bool PrepareTerrainCollisionSeams(
		TerrainHeightFieldRecipe& recipe, const TerrainCollisionRootCapture& capture);
	// Worker-only expensive preparation; no live world or scene storage is read.
	[[nodiscard]] TERRAENGINE_API bool CookTerrainCollisionRecipe(TerrainHeightFieldRecipe& recipe);
}
