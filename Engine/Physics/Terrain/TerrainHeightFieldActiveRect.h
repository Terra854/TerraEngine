// TerrainHeightFieldActiveRect.h -- preserve authored terrain cells in fixed collision grids.

#pragma once

#include "Core/Export.h"
#include "Physics/PhysicsTypes.h"
#include "Terrain/TerrainTypes.h"

namespace TerraEngine::Physics::Terrain {
	// The physics backend always consumes a 257 by 257 sample grid (256 cells),
	// while an authored edge area can own fewer samples. This function keeps the
	// fixed provider shape and marks only its padded *cells* as holes. It validates
	// the complete recipe and active rectangle before changing any caller-owned
	// bytes, so a malformed extent cannot accidentally broaden collision support.
	[[nodiscard]] TERRAENGINE_API bool ApplyTerrainActiveRectToRecipe(
		TerrainHeightFieldRecipe& recipe,
		const TerraEngine::Terrain::TerrainAreaActiveRect& activeRect) noexcept;
}
