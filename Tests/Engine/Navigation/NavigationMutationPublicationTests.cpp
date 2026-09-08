// Test-first contracts for dynamic navigation source transactions/publication.
//
// The dynamic update implementation must provide the provider-neutral dirty-index and
// publication APIs used below. The tests intentionally avoid live scene
// objects: worker inputs and protected-boundary results are copied values.

#include "../TestHarness.h"

#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationPublication.h"
#include "Navigation/NavigationWorld.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Navigation;

		constexpr NavigationGeometryBounds Bounds(float minimumX, float maximumX)
		{
			return { { minimumX, 0.0F, -1.0F }, { maximumX, 2.0F, 1.0F } };
		}

		NavigationDynamicSourceChange MakeChange(NavigationDynamicChangeKind kind,
			NavigationGeneration revision, NavigationGeometryBounds previousBounds,
			NavigationGeometryBounds currentBounds)
		{
			NavigationDynamicSourceChange change{};
			change.source = NavigationUuid::FromString("8f0b1e49-41c9-4f53-8651-067e310fab06");
			change.sourceRevision = revision;
			change.physicalReadyRevision = revision;
			change.kind = kind;
			change.previousBounds = previousBounds;
			change.currentBounds = currentBounds;
			return change;
		}

	}

	void RegisterNavigationMutationPublicationTests(TestRegistry& registry)
	{
		registry.Add("Navigation.DynamicMutation.UsesExpandedOldAndNewBoundsForEveryCommittedChange", [](TestContext& context) {
			NavigationDirtyIndex dirty({ .clearanceExpansionMetres = 0.55F, .buildHaloMetres = 0.25F });
			const NavigationDynamicSourceChange moved = MakeChange(NavigationDynamicChangeKind::Moved, 7U,
				Bounds(-8.0F, -4.0F), Bounds(4.0F, 8.0F));
			const NavigationDynamicChangeReceipt receipt = dirty.CommitSourceChange(moved);
			context.Expect(receipt.accepted, "A finite source move must commit as one copied navigation change receipt.");
			context.Expect(receipt.affectedBounds.minimum.x <= -8.0F && receipt.affectedBounds.maximum.x >= 8.0F,
				"Dirty indexing must cover both the old and new source extents, not merely its latest placement.");
			context.Expect(receipt.affectedBounds.minimum.x < -8.0F && receipt.affectedBounds.maximum.x > 8.0F,
				"Old/new dirty bounds must expand by clearance and build halo before work is selected.");
		});

		registry.Add("Navigation.DynamicMutation.RemovalTombstoneIsObservableBeforeReplacementPreparation", [](TestContext& context) {
			NavigationDirtyIndex dirty({});
			const NavigationDynamicSourceChange removed = MakeChange(NavigationDynamicChangeKind::Removed, 9U,
				Bounds(-2.0F, 2.0F), {});
			context.Expect(dirty.CommitSourceChange(removed).accepted, "A committed removal must create a typed dirty/tombstone receipt.");
			const NavigationSourceTombstoneSnapshot observed = dirty.ObserveAtProtectedBoundary();
			context.Expect(observed.Contains(removed.source, removed.sourceRevision),
				"A removed source must become unusable at the next observation boundary before replacement preparation completes.");
			context.Expect(dirty.PendingPreparationCount() == 1U,
				"Removal invalidation must remain queued for replacement work without delaying the tombstone observation.");
		});

		registry.Add("Navigation.DynamicPublication.RejectedCandidateRollsBackAtomicallyWithoutClearingTombstones", [](TestContext& context) {
			NavigationWorld world({ { 412U }, 3U });
			NavigationDirtyIndex dirty({});
			const NavigationDynamicSourceChange removed = MakeChange(NavigationDynamicChangeKind::Removed, 12U,
				Bounds(-1.0F, 1.0F), {});
			dirty.CommitSourceChange(removed);
			const NavigationSourceTombstoneSnapshot tombstones = dirty.ObserveAtProtectedBoundary();
			const std::shared_ptr<const NavigationWorldRoot> before = world.PublishedRoot();

			NavigationPublicationCandidate stale{};
			stale.expectedRootGeneration = before->rootGeneration + 1U;
			stale.expectedSourceRevision = removed.sourceRevision - 1U;
			stale.tombstones = tombstones;
			stale.coverage.push_back({ { 0, 0, 0, 1U }, 5U, NavigationCoverageState::Ready, 128U, true });
			const NavigationPublicationReceipt rejected = PublishNavigationCandidateAtProtectedBoundary(world, dirty, stale);
			context.Expect(!rejected.accepted && rejected.reason == NavigationPublicationRejection::StaleSourceRevision,
				"A candidate built from a prior source revision must reject as a whole at protected publication.");
			context.Expect(world.PublishedRoot() == before,
				"Rejecting a stale group must retain the exact previously published root rather than partially replacing coverage.");
			context.Expect(dirty.ObserveAtProtectedBoundary().Contains(removed.source, removed.sourceRevision),
				"Publication rejection must never erase a committed removal tombstone.");
		});

		registry.Add("Navigation.DynamicPublication.OutOfOrderCompletionCannotOverwriteNewerAuthority", [](TestContext& context) {
			NavigationWorld world({ { 412U }, 3U });
			NavigationDirtyIndex dirty({});
			const NavigationDynamicSourceChange first = MakeChange(NavigationDynamicChangeKind::Reshaped, 20U,
				Bounds(-2.0F, 2.0F), Bounds(-3.0F, 3.0F));
			const NavigationDynamicSourceChange second = MakeChange(NavigationDynamicChangeKind::Reshaped, 21U,
				Bounds(-3.0F, 3.0F), Bounds(-4.0F, 4.0F));
			dirty.CommitSourceChange(first);
			dirty.CommitSourceChange(second);

			NavigationPublicationCandidate newer{};
			newer.expectedRootGeneration = world.PublishedRoot()->rootGeneration;
			newer.expectedSourceRevision = second.sourceRevision;
			newer.coverage.push_back({ { 0, 0, 0, 1U }, 21U, NavigationCoverageState::Ready, 64U, true });
			context.Expect(PublishNavigationCandidateAtProtectedBoundary(world, dirty, newer).accepted,
				"The newest compatible completion should establish the next complete navigation authority.");
			const std::shared_ptr<const NavigationWorldRoot> afterNewer = world.PublishedRoot();

			NavigationPublicationCandidate lateOlder{};
			lateOlder.expectedRootGeneration = newer.expectedRootGeneration;
			lateOlder.expectedSourceRevision = first.sourceRevision;
			lateOlder.coverage.push_back({ { 0, 0, 0, 1U }, 20U, NavigationCoverageState::Ready, 64U, true });
			const NavigationPublicationReceipt stale = PublishNavigationCandidateAtProtectedBoundary(world, dirty, lateOlder);
			context.Expect(!stale.accepted && stale.reason == NavigationPublicationRejection::StaleSourceRevision,
				"An older worker completion must not resurrect superseded geometry after a newer revision publishes.");
			context.Expect(world.PublishedRoot() == afterNewer,
				"Out-of-order completion must leave the newer authoritative root untouched.");
		});

		registry.Add("Navigation.DynamicPublication.KeepsUnrelatedCoverageWhenOneCandidateFails", [](TestContext& context) {
			NavigationWorld world({ { 412U }, 3U });
			NavigationDirtyIndex dirty({});
			NavigationPublicationCandidate baseline{};
			baseline.expectedRootGeneration = world.PublishedRoot()->rootGeneration;
			baseline.expectedSourceRevision = 1U;
			baseline.coverage = {
				{ { 0, 0, 0, 1U }, 1U, NavigationCoverageState::Ready, 64U, true },
				{ { 4, 0, 0, 1U }, 1U, NavigationCoverageState::Ready, 64U, true }
			};
			context.Expect(PublishNavigationCandidateAtProtectedBoundary(world, dirty, baseline).accepted,
				"The fixture needs two ready unaffected coverage records before testing an isolated rejection.");
			const std::shared_ptr<const NavigationWorldRoot> prior = world.PublishedRoot();

			NavigationPublicationCandidate malformed{};
			malformed.expectedRootGeneration = prior->rootGeneration;
			malformed.expectedSourceRevision = 2U;
			malformed.coverage.push_back({ { 8, 0, 0, 1U }, 2U, NavigationCoverageState::Ready,
				std::numeric_limits<std::size_t>::max(), true });
			const NavigationPublicationReceipt receipt = PublishNavigationCandidateAtProtectedBoundary(world, dirty, malformed);
			context.Expect(!receipt.accepted, "A candidate that exceeds bounded publication memory must reject before a root replacement.");
			context.Expect(world.PublishedRoot() == prior && world.PublishedRoot()->coverage.size() == 2U,
				"One rejected local update must retain complete unrelated coverage without a whole-world rollback.");
		});
	}
}
