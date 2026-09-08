// NavigationPublication.cpp -- Revision-complete local root replacement.

#include "Navigation/Dynamic/NavigationPublication.h"

#include <algorithm>
#include <limits>

namespace TerraEngine::Navigation
{
	namespace
	{
		[[nodiscard]] bool SameCoverageKey(const NavigationCoverageKey& first, const NavigationCoverageKey& second) noexcept
		{
			return first == second;
		}

		[[nodiscard]] bool CanAdd(std::size_t current, std::size_t addition) noexcept
		{
			return addition <= std::numeric_limits<std::size_t>::max() - current;
		}

		void MergeTombstones(std::vector<NavigationSourceTombstone>& target,
			const std::vector<NavigationSourceTombstone>& additions)
		{
			for(const NavigationSourceTombstone& addition : additions) {
				auto existing = std::find_if(target.begin(), target.end(), [&addition](const NavigationSourceTombstone& current) {
					return current.sourceId == addition.sourceId;
				});
				if(existing == target.end()) target.push_back(addition);
				else existing->removedRevision = std::max(existing->removedRevision, addition.removedRevision);
			}
		}
	}

	NavigationPublicationReceipt PublishNavigationCandidateAtProtectedBoundary(
		NavigationWorld& world, const NavigationDirtyIndex& dirty, const NavigationPublicationCandidate& candidate)
	{
		NavigationPublicationReceipt receipt{};
		const std::shared_ptr<const NavigationWorldRoot> published = world.PublishedRoot();
		const NavigationGeneration latestSourceRevision = dirty.LatestSourceRevision();
		if((latestSourceRevision != 0U && candidate.expectedSourceRevision != latestSourceRevision)
			|| candidate.expectedSourceRevision == 0U) {
			receipt.reason = NavigationPublicationRejection::StaleSourceRevision;
			return receipt;
		}
		if(published == nullptr || candidate.expectedRootGeneration != published->rootGeneration) {
			receipt.reason = NavigationPublicationRejection::StaleRootGeneration;
			return receipt;
		}

		NavigationWorldRoot replacement = world.CreateCandidate();
		if(replacement.rootGeneration != candidate.expectedRootGeneration + 1U) {
			receipt.reason = NavigationPublicationRejection::StaleRootGeneration;
			return receipt;
		}
		for(const NavigationCoverageRecord& changed : candidate.coverage) {
			if(changed.chargedBytes == std::numeric_limits<std::size_t>::max()) {
				receipt.reason = NavigationPublicationRejection::CapacityExceeded;
				return receipt;
			}
			auto existing = std::find_if(replacement.coverage.begin(), replacement.coverage.end(), [&changed](const NavigationCoverageRecord& current) {
				return SameCoverageKey(current.key, changed.key);
			});
			if(existing == replacement.coverage.end()) replacement.coverage.push_back(changed);
			else *existing = changed;
		}
		for(const NavigationSourceIndexRecord& changed : candidate.sourceReplacements) {
			if(changed.sourceId == EMPTY_UUID || changed.sourceRevision == 0U
				|| changed.geometryRevision == 0U) {
				receipt.reason = NavigationPublicationRejection::InvalidCandidate;
				return receipt;
			}
			auto existing = std::find_if(replacement.sources.begin(), replacement.sources.end(), [&changed](const NavigationSourceIndexRecord& current) {
				return current.sourceId == changed.sourceId;
			});
			if(existing == replacement.sources.end()) replacement.sources.push_back(changed);
			else *existing = changed;
		}

		// The dirty snapshot is authoritative for committed removals. Candidates may
		// carry the same copied snapshot, but neither a stale nor a successful build
		// can reduce a tombstone revision while the source remains removed.
		MergeTombstones(replacement.tombstones, dirty.ObserveAtProtectedBoundary().entries);
		MergeTombstones(replacement.tombstones, candidate.tombstones.entries);
		std::size_t coverageBytes = 0U;
		for(const NavigationCoverageRecord& coverage : replacement.coverage) {
			if(!CanAdd(coverageBytes, coverage.chargedBytes)) {
				receipt.reason = NavigationPublicationRejection::CapacityExceeded;
				return receipt;
			}
			coverageBytes += coverage.chargedBytes;
		}

		if(!world.Publish(std::move(replacement), candidate.expectedRootGeneration)) {
			// NavigationWorld performs its own root byte/residency admission. A failed
			// call leaves the old immutable root visible, so map it to capacity rather
			// than attempting an unsafe partial retry at this boundary.
			receipt.reason = NavigationPublicationRejection::CapacityExceeded;
			return receipt;
		}
		receipt.accepted = true;
		receipt.reason = NavigationPublicationRejection::None;
		receipt.publishedRootGeneration = candidate.expectedRootGeneration + 1U;
		return receipt;
	}
}
