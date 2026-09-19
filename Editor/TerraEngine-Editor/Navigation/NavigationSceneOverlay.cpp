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
		// A selected route has priority over broad coverage diagnostics. It is the
		// small, demand-driven copy a user explicitly asked to inspect.
		if(input.selectedAgent) {
			result.selectedAgent = input.selectedAgent;
			result.selectedAgent->routePoints.clear();
			result.selectedAgent->routeTruncated = false;
			if(input.selectedAgent->routeCurrent && input.selectedAgent->guidance.IsUsable()) {
				copyBounded(input.selectedAgent->routePoints, result.selectedAgent->routePoints);
				result.selectedAgent->routeTruncated = result.selectedAgent->routePoints.size()
					!= input.selectedAgent->routePoints.size();
			}
		}
		result.agentDiagnostics = input.agentDiagnostics;
		copyBounded(input.coverage, result.coverage);
		copyBounded(input.connectionEndpoints, result.connectionEndpoints);
		copyBounded(input.routePoints, result.routePoints);
		copyBounded(input.dirtyCoverage, result.dirtyCoverage);
		return result;
	}
}
