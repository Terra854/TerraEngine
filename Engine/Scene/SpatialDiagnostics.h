#pragma once

#include <cstdint>

namespace TerraEngine::Scene
{
	struct SpatialDiagnosticsSnapshot
	{
		std::uint32_t indexedObjectCount = 0;
		std::uint32_t staticObjectCount = 0;
		std::uint32_t dynamicObjectCount = 0;
		std::uint32_t dirtyObjectCount = 0;
		std::uint32_t visibleCandidateCount = 0;
		std::uint32_t rejectedObjectCount = 0;
		std::uint32_t fallbackObjectCount = 0;
		std::uint32_t largeObjectCount = 0;
		std::uint32_t maxNodeDepth = 0;
		double lastQueryDurationMs = 0.0;
		double lastUpdateDurationMs = 0.0;
	};
}
