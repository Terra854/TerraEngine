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
	struct NavigationDynamicRebuildRequest {
		NavigationDynamicSourceChange change{};
		NavigationGeneration expectedRootGeneration = 0U;
		NavigationProfile profile{};
		NavigationSourceTombstoneSnapshot tombstones{};
		std::vector<NavigationDirtyCoverageDependency> dirtyCoverage;
		std::vector<NavigationSourceIndexRecord> sourceIndex;
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		std::size_t maximumShards = 256U;
	};

	// This is a publication *plan*, not a publication itself. The caller must
	// exchange its shards, coverage root, and portal intervals at one protected
	// boundary after checking the captured source/root generations again.
	struct NavigationDynamicRebuildPlan {
		bool accepted = false;
		std::string reason;
		NavigationPublicationCandidate candidate{};
		std::vector<std::shared_ptr<const DetourGroundShardGeneration>> shards;
		std::vector<Ground::GroundBoundaryInterval> boundaryIntervals;
	};

	[[nodiscard]] TERRAENGINE_API NavigationDynamicRebuildPlan PrepareDynamicGroundReplacement(
		const NavigationDynamicRebuildRequest& request);
}
