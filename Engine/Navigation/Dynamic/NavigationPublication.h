// NavigationPublication.h -- Protected atomic dynamic navigation publication.

#pragma once

#include "Navigation/Dynamic/NavigationDirtyIndex.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Navigation
{
	enum class NavigationPublicationRejection : std::uint8_t
	{
		None,
		StaleRootGeneration,
		StaleSourceRevision,
		CapacityExceeded,
		InvalidCandidate
	};

	struct NavigationPublicationCandidate
	{
		NavigationGeneration expectedRootGeneration = 0U;
		NavigationGeneration expectedSourceRevision = 0U;
		NavigationSourceTombstoneSnapshot tombstones{};
		// Source rows travel with the coverage replacement so a protected caller
		// cannot publish geometry from a newer source while retaining stale source
		// metadata in the same immutable world root.
		std::vector<NavigationSourceIndexRecord> sourceReplacements;
		std::vector<NavigationCoverageRecord> coverage;
	};

	struct NavigationPublicationReceipt
	{
		bool accepted = false;
		NavigationPublicationRejection reason = NavigationPublicationRejection::InvalidCandidate;
		NavigationGeneration publishedRootGeneration = 0U;
	};

	// The protected caller supplies a detached candidate and the current dirty
	// index. A rejected candidate cannot mutate the world root or remove a
	// committed tombstone; accepted coverage replaces only matching local keys.
	[[nodiscard]] TERRAENGINE_API NavigationPublicationReceipt PublishNavigationCandidateAtProtectedBoundary(
		NavigationWorld& world, const NavigationDirtyIndex& dirty, const NavigationPublicationCandidate& candidate);
}
