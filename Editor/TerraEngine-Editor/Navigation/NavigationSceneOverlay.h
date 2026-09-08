// NavigationSceneOverlay.h - bounded reusable editor overlay extraction.

#pragma once

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <vector>

namespace TerraEngine::Editor::Navigation {
	struct NavigationSceneOverlayInput {
		std::vector<TerraEngine::Navigation::NavigationCoverageRecord> coverage;
		std::vector<TerraEngine::Scene::WorldPosition> connectionEndpoints;
		std::vector<TerraEngine::Scene::WorldPosition> routePoints;
		std::vector<TerraEngine::Navigation::NavigationCoverageKey> dirtyCoverage;
	};

	struct NavigationSceneOverlaySnapshot {
		std::vector<TerraEngine::Navigation::NavigationCoverageRecord> coverage;
		std::vector<TerraEngine::Scene::WorldPosition> connectionEndpoints;
		std::vector<TerraEngine::Scene::WorldPosition> routePoints;
		std::vector<TerraEngine::Navigation::NavigationCoverageKey> dirtyCoverage;
		bool truncated = false;
	};

	[[nodiscard]] NavigationSceneOverlaySnapshot ExtractNavigationOverlay(
		const NavigationSceneOverlayInput& input,
		std::size_t maximumItems = 4096U);
}
