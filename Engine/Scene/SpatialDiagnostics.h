#pragma once

#include <cstdint>

namespace TerraEngine::Scene
{
	// Describes how much spatial-index storage changed during the latest
	// update. Tooling can use this to confirm stable scenes are reusing the
	// existing tree instead of rebuilding it every frame.
	enum class SpatialRefreshMode : std::uint8_t
	{
		Uninitialized,
		Unchanged,
		Incremental,
		Full
	};

	struct SpatialDiagnosticsSnapshot
	{
		std::uint32_t indexedObjectCount = 0;
		std::uint32_t staticObjectCount = 0;
		std::uint32_t dynamicObjectCount = 0;
		std::uint32_t dirtyObjectCount = 0;
		// These refresh counters describe the index update step, not the query
		// that runs afterward for a particular camera.
		std::uint32_t addedObjectCount = 0;
		std::uint32_t updatedObjectCount = 0;
		std::uint32_t removedObjectCount = 0;
		std::uint32_t unchangedObjectCount = 0;
		std::uint32_t fullRefreshObjectCount = 0;
		std::uint32_t visibleCandidateCount = 0;
		std::uint32_t rejectedObjectCount = 0;
		std::uint32_t fallbackObjectCount = 0;
		std::uint32_t largeObjectCount = 0;
		std::uint32_t maxNodeDepth = 0;
		SpatialRefreshMode refreshMode = SpatialRefreshMode::Uninitialized;
		double lastQueryDurationMs = 0.0;
		double lastUpdateDurationMs = 0.0;
	};
}
