// SpatialDiagnostics.h — Spatial System Diagnostic Types
//
// Defines the structures used to capture and report performance metrics for the spatial partitioning system.
// It tracks statistics such as object counts, query times, and structure health for both editor and runtime analysis.
// These diagnostics help developers tune spatial parameters and identify performance bottlenecks in large scenes.

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

	// struct SpatialDiagnosticsSnapshot groups related state and operations so callers have one documented contract.
	struct SpatialDiagnosticsSnapshot
	{
		std::uint32_t indexedObjectCount = 0;
		std::uint32_t staticObjectCount = 0;
		std::uint32_t dynamicObjectCount = 0;
		std::uint32_t dirtyObjectCount = 0;
		std::uint32_t transformDirtyRootCount = 0;
		std::uint32_t transformExpandedObjectCount = 0;
		std::uint32_t transformProcessedObjectCount = 0;
		std::uint32_t transformSkippedCleanObjectCount = 0;
		std::uint32_t transformHierarchyLevelCount = 0;
		std::uint32_t boundsRecomputedObjectCount = 0;
		std::uint32_t invalidTransformObjectCount = 0;
		std::uint32_t invalidBoundsObjectCount = 0;
		// These refresh counters describe the index update step, not the query
		// that runs afterward for a particular camera.
		std::uint32_t addedObjectCount = 0;
		std::uint32_t updatedObjectCount = 0;
		std::uint32_t removedObjectCount = 0;
		std::uint32_t unchangedObjectCount = 0;
		std::uint32_t fullRefreshObjectCount = 0;
		std::uint32_t incrementalRefreshCount = 0;
		std::uint32_t fullRefreshCount = 0;
		std::uint32_t revisionMismatchFallbackCount = 0;
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
