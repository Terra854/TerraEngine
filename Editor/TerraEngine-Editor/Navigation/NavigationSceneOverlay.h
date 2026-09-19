// NavigationSceneOverlay.h - bounded reusable editor overlay extraction.

#pragma once

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Agents/NavigationAgentTypes.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace TerraEngine::Editor::Navigation {
	// This value is copied from a protected agent publication. It deliberately
	// contains no runtime-store reference, so Scene View can draw it safely.
	struct NavigationAgentOverlayRecord {
		TerraEngine::Navigation::NavigationAgentGuidanceSnapshot guidance{};
		std::vector<TerraEngine::Scene::WorldPosition> routePoints;
		bool routeCurrent = false;
		// The source guidance remains current, but only a prefix fit the editor
		// budget. Renderers must not treat that prefix's final point as destination.
		bool routeTruncated = false;
	};

	// Aggregate counters make saturation and retry pressure visible without a
	// per-frame scan of live agents or a copy of every agent route.
	struct NavigationAgentOverlayDiagnostics {
		std::size_t activeAgents = 0U;
		std::size_t requestingAgents = 0U;
		std::size_t followingAgents = 0U;
		std::size_t arrivedAgents = 0U;
		std::size_t stoppedAgents = 0U;
		std::size_t failedAgents = 0U;
		std::size_t pendingCommands = 0U;
		std::size_t commandCapacity = 0U;
		std::size_t retainedReceipts = 0U;
		std::size_t currentOwnedMemoryBytes = 0U;
		std::uint64_t retryCount = 0U;
		std::uint64_t rejectedCommandCount = 0U;
		std::uint64_t coalescedCommandCount = 0U;
		std::uint64_t lastProtectedPublishMicroseconds = 0U;
	};

	struct NavigationSceneOverlayInput {
		std::vector<TerraEngine::Navigation::NavigationCoverageRecord> coverage;
		std::vector<TerraEngine::Scene::WorldPosition> connectionEndpoints;
		std::vector<TerraEngine::Scene::WorldPosition> routePoints;
		std::vector<TerraEngine::Navigation::NavigationCoverageKey> dirtyCoverage;
		std::optional<NavigationAgentOverlayRecord> selectedAgent;
		NavigationAgentOverlayDiagnostics agentDiagnostics{};
	};

	struct NavigationSceneOverlaySnapshot {
		std::vector<TerraEngine::Navigation::NavigationCoverageRecord> coverage;
		std::vector<TerraEngine::Scene::WorldPosition> connectionEndpoints;
		std::vector<TerraEngine::Scene::WorldPosition> routePoints;
		std::vector<TerraEngine::Navigation::NavigationCoverageKey> dirtyCoverage;
		std::optional<NavigationAgentOverlayRecord> selectedAgent;
		NavigationAgentOverlayDiagnostics agentDiagnostics{};
		bool truncated = false;
	};

	[[nodiscard]] NavigationSceneOverlaySnapshot ExtractNavigationOverlay(
		const NavigationSceneOverlayInput& input,
		std::size_t maximumItems = 4096U);
}
