// NavigationRebuildPipeline.h - worker-safe dynamic ground replacement plans.
//
// The NavigationSystem supplies this pipeline only copied source snapshots and
// dirty dependencies. It returns detached provider generations plus one root
// candidate; the system retains ownership of the final protected publication.

#pragma once

#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Dynamic/NavigationPublication.h"
#include "Navigation/Ground/GroundPortalGraph.h"

#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	// A retained shard pack is captured at a protected boundary. It contains no
	// provider state and is useful only while the exact root/coverage tuple named
	// by its request remains the authoritative baseline.
	struct NavigationDynamicShardReuseBaseline {
		GroundShardReference reference{};
		NavigationGeneration rootGeneration = 0U;
		NavigationGeneration terrainPublicationGeneration = 0U;
		NavigationGeneration sourceIndexRevision = 0U;
		std::shared_ptr<const RetainedGroundTilePack> retainedTiles;
	};

	struct NavigationDynamicRebuildRequest {
		NavigationDynamicSourceChange change{};
		NavigationGeneration expectedRootGeneration = 0U;
		NavigationGeneration expectedTerrainPublicationGeneration = 0U;
		NavigationGeneration expectedSourceIndexRevision = 0U;
		NavigationProfile profile{};
		NavigationSourceTombstoneSnapshot tombstones{};
		// Every receipt remains pending until the profile group publishes. Keeping the
		// complete value chain lets a worker derive a conservative profile-specific
		// Recast halo instead of trusting the fixed generic dirty expansion.
		std::vector<NavigationDynamicSourceChange> pendingChanges;
		std::vector<NavigationDirtyCoverageDependency> dirtyCoverage;
		std::vector<NavigationSourceIndexRecord> sourceIndex;
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		std::vector<NavigationDynamicShardReuseBaseline> reuseBaselines;
		// The caller leases this budget through the asynchronous packet lifetime.
		// Recast uses it for detached tile bytes so cancellation/retirement cannot
		// hide dynamic provider output outside the navigation memory limits.
		NavigationBudget* budget = nullptr;
		std::size_t maximumShards = 256U;
	};

	// This is a publication *plan*, not a publication itself. The caller must
	// exchange its shards, coverage root, and portal intervals at one protected
	// boundary after checking the captured source/root generations again.
	struct NavigationDynamicRebuildPlan {
		bool accepted = false;
		std::string reason;
		// A rejected worker plan still needs a typed public disposition. Protected
		// publication uses this only after proving the copied receipt is current,
		// so an old provider failure cannot terminalize a newer recovery.
		NavigationOutcome failureOutcome = NavigationOutcome::UnavailableCoverage;
		NavigationReason failureReason = NavigationReason::MissingNeighbor;
		bool failurePermanent = false;
		NavigationPublicationCandidate candidate{};
		std::vector<std::shared_ptr<const DetourGroundShardGeneration>> shards;
		std::vector<Ground::GroundBoundaryInterval> boundaryIntervals;
		// Scalar evidence is intentionally enough for validation without retaining a
		// history of tile packets in diagnostics or exposing provider byte buffers.
		bool incrementalReuseAttempted = false;
		std::size_t reusedTileCount = 0U;
		std::size_t rebuiltTileCount = 0U;
		std::size_t fullShardFallbackCount = 0U;
		std::string incrementalReuseFallbackReason;
	};

	[[nodiscard]] TERRAENGINE_API NavigationDynamicRebuildPlan PrepareDynamicGroundReplacement(
		const NavigationDynamicRebuildRequest& request);
}
