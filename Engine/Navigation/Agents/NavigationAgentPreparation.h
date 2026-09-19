// Pure candidate preparation, safe for disjoint worker chunks.
#pragma once
#include "NavigationAgentCapture.h"
namespace TerraEngine::Navigation {
	struct NavigationAgentCandidate {
		NavigationAgentRuntimeRecord runtime{};
		std::size_t nextWaypointIndex = 0U;
		bool requestReplan = false;
	};
	TERRAENGINE_API NavigationAgentCandidate PrepareNavigationAgent(const NavigationAgentCapture&);
}
