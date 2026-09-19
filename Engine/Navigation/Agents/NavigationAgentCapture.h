// Captured values own all worker inputs; there are no live scene pointers.
#pragma once
#include "NavigationAgentStore.h"
#include <optional>
#include <vector>
class ObjectManager;
namespace TerraEngine::Navigation {
	struct NavigationAgentCapture {
		NavigationAgentRuntimeRecord runtime{};
		NavigationObjectEndpoint owner{};
		std::optional<NavigationObjectEndpoint> target{};
		std::vector<NavigationLocation> routePoints{};
		std::size_t waypointIndex = 0U;
		bool routeUsable = false;
		NavigationObservationEvidence observation{};
		NavigationGeneration publicationGeneration = 0U;
		NavigationGeneration routeGeneration = 0U;
	};
	// Must be called from protected capture. Missing owner returns no packet;
	// missing target remains an explicit failure input for pure preparation.
	TERRAENGINE_API std::optional<NavigationAgentCapture> CaptureNavigationAgent(
		ObjectManager&, const NavigationAgentRuntimeRecord&);
}
