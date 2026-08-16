// UnboundedSceneTestFixtures.cpp - implementations for canonical test inputs.

#include "UnboundedSceneTestFixtures.h"

#include <limits>

namespace TerraEngine::Tests::UnboundedSceneFixtures {

	Scene::WorldCoordinateResult MakePosition(
		std::int64_t cellX,
		std::int64_t cellY,
		std::int64_t cellZ,
		double offsetX,
		double offsetY,
		double offsetZ)
	{
		// Normalization is intentionally delegated to the production boundary so
		// tests exercise exactly the carry and overflow rules used at runtime.
		return Scene::NormalizeWorldPosition({ { cellX, cellY, cellZ }, { offsetX, offsetY, offsetZ } });
	}

	Scene::WorldPosition MakeExtremePositivePosition()
	{
		constexpr std::int64_t kCellAtOneQuadrillionMetres = 15'258'789'062LL;
		return MakePosition(kCellAtOneQuadrillionMetres, 0, 0, 1.0, 0.0, 0.0).position;
	}

	Scene::WorldPosition MakeExtremeNegativePosition()
	{
		constexpr std::int64_t kCellAtOneQuadrillionMetres = -15'258'789'063LL;
		return MakePosition(kCellAtOneQuadrillionMetres, 0, 0, -1.0, 0.0, 0.0).position;
	}

	Scene::WorldPosition MakeNonFiniteOffsetPosition()
	{
		return { {}, { std::numeric_limits<double>::infinity(), 0.0, 0.0 } };
	}

	Scene::WorldPosition MakeOverflowPosition()
	{
		return { { std::numeric_limits<std::int64_t>::max(), 0, 0 }, { Scene::kWorldCellHalfEdgeMetres, 0.0, 0.0 } };
	}

}
