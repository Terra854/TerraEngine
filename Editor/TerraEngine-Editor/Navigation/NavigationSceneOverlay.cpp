// NavigationSceneOverlay.cpp - immutable copies for scene-view debug drawing.

#include "NavigationSceneOverlay.h"

#include <algorithm>

namespace TerraEngine::Editor::Navigation {
	NavigationSceneOverlaySnapshot ExtractNavigationOverlay(
		const NavigationSceneOverlayInput& input,
		std::size_t maximumItems) {
		NavigationSceneOverlaySnapshot result{};
		std::size_t remaining = maximumItems;
		const auto copyBounded = [&remaining, &result](const auto& source, auto& destination) {
			const std::size_t count = std::min(remaining, source.size());
			destination.assign(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(count));
			remaining -= count;
			result.truncated = result.truncated || count != source.size();
		};
		copyBounded(input.coverage, result.coverage);
		copyBounded(input.connectionEndpoints, result.connectionEndpoints);
		copyBounded(input.routePoints, result.routePoints);
		copyBounded(input.dirtyCoverage, result.dirtyCoverage);
		return result;
	}
}
