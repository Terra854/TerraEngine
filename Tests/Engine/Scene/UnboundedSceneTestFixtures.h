// UnboundedSceneTestFixtures.h - deterministic inputs shared by unbounded-world tests.
//
// The builders in this file deliberately construct only the new canonical
// coordinate model.  Tests that need an old float position must model it as an
// unsupported payload rather than reintroducing a compatibility API.

#pragma once

#include "Scene/WorldCoordinate.h"

#include <cstdint>
#include <limits>

namespace TerraEngine::Tests::UnboundedSceneFixtures {

	// Creates one normalized world position and throws no hidden rounding into
	// the test input. Callers check the returned error when testing rejection.
	Scene::WorldCoordinateResult MakePosition(
		std::int64_t cellX,
		std::int64_t cellY,
		std::int64_t cellZ,
		double offsetX = 0.0,
		double offsetY = 0.0,
		double offsetZ = 0.0);

	// Supplies a repeatable extreme location used by rendering, persistence, and
	// transactional tests without materializing a lossy global floating value.
	Scene::WorldPosition MakeExtremePositivePosition();
	Scene::WorldPosition MakeExtremeNegativePosition();

	// Failure injection inputs keep range/overflow tests self-documenting.
	Scene::WorldPosition MakeNonFiniteOffsetPosition();
	Scene::WorldPosition MakeOverflowPosition();

}
