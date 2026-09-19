// Test-first publication contracts for dynamic navigation updates.
//
// Publication is a protected boundary: candidates are either committed as a
// complete immutable root or rejected without changing the prior root.
#include "../TestHarness.h"

#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationPublication.h"
#include "Navigation/NavigationWorld.h"

#include <limits>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		NavigationGeometryBounds Bounds(float minimum, float maximum)
		{
			return { { minimum, 0.0F, -1.0F }, { maximum, 2.0F, 1.0F } };
		}

		NavigationDynamicSourceChange Removal(NavigationGeneration revision)
		{
			NavigationDynamicSourceChange value{};
			value.source = NavigationUuid::FromString("f6a77ae8-0d42-4d28-9132-000000000001");
			value.sourceRevision = revision;
			value.physicalReadyRevision = revision;
			value.kind = NavigationDynamicChangeKind::Removed;
			value.previousBounds = Bounds(-2.0F, 2.0F);
			return value;
		}
	}

	void RegisterNavigationPublicationContractTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Publication.RejectsStaleCandidateAtomically", [](TestContext& context) {
			NavigationWorld world({ { 59001U }, 1U });
			NavigationDirtyIndex dirty({});
			const NavigationDynamicSourceChange removed = Removal(12U);
			context.Expect(dirty.CommitSourceChange(removed).accepted, "The fixture must commit a removal first.");
			const NavigationSourceTombstoneSnapshot tombstones = dirty.ObserveAtProtectedBoundary();
			const std::shared_ptr<const NavigationWorldRoot> before = world.PublishedRoot();

			NavigationPublicationCandidate stale{};
			stale.expectedRootGeneration = before->rootGeneration + 1U;
			stale.expectedSourceRevision = removed.sourceRevision - 1U;
			stale.tombstones = tombstones;
			stale.coverage.push_back({ { 0, 0, 0, 1U }, 5U, NavigationCoverageState::Ready, 128U, true });
			const NavigationPublicationReceipt receipt = PublishNavigationCandidateAtProtectedBoundary(world, dirty, stale);
			context.Expect(!receipt.accepted && receipt.reason == NavigationPublicationRejection::StaleSourceRevision,
				"A stale worker candidate must be rejected at the protected publication boundary.");
			context.Expect(world.PublishedRoot() == before,
				"Rejected candidates must not partially replace the prior immutable root.");
			context.Expect(dirty.ObserveAtProtectedBoundary().Contains(removed.source, removed.sourceRevision),
				"Rejected publication must retain removal tombstones for the next valid build.");
		});

		registry.Add("Navigation.Publication.OutOfOrderCompletionCannotRestoreOlderGeometry", [](TestContext& context) {
			NavigationWorld world({ { 59001U }, 1U });
			NavigationDirtyIndex dirty({});
			// Seed the dirty index with both source revisions. Without this copied
			// evidence, a standalone publication has no authoritative latest source
			// revision and an older candidate is indistinguishable from a new one.
			NavigationDynamicSourceChange firstChange{};
			firstChange.source = NavigationUuid::FromString("f6a77ae8-0d42-4d28-9132-000000000002");
			firstChange.sourceRevision = 20U;
			firstChange.physicalReadyRevision = 20U;
			firstChange.kind = NavigationDynamicChangeKind::Reshaped;
			firstChange.previousBounds = {{-2.0F, 0.0F, -1.0F}, {2.0F, 2.0F, 1.0F}};
			firstChange.currentBounds = {{-3.0F, 0.0F, -1.0F}, {3.0F, 2.0F, 1.0F}};
			NavigationDynamicSourceChange secondChange = firstChange;
			secondChange.sourceRevision = 21U;
			secondChange.physicalReadyRevision = 21U;
			secondChange.currentBounds = {{-4.0F, 0.0F, -1.0F}, {4.0F, 2.0F, 1.0F}};
			context.Expect(dirty.CommitSourceChange(firstChange).accepted
				&& dirty.CommitSourceChange(secondChange).accepted,
				"The fixture must retain authoritative source revisions before testing out-of-order completion.");
			NavigationPublicationCandidate current{};
			current.expectedRootGeneration = world.PublishedRoot()->rootGeneration;
			current.expectedSourceRevision = 21U;
			current.coverage.push_back({ { 0, 0, 0, 1U }, 21U, NavigationCoverageState::Ready, 64U, true });
			context.Expect(PublishNavigationCandidateAtProtectedBoundary(world, dirty, current).accepted,
				"The newer candidate must establish the current immutable authority.");
			const std::shared_ptr<const NavigationWorldRoot> newer = world.PublishedRoot();

			NavigationPublicationCandidate older{};
			older.expectedRootGeneration = newer->rootGeneration;
			older.expectedSourceRevision = 20U;
			older.coverage.push_back({ { 0, 0, 0, 1U }, 20U, NavigationCoverageState::Ready, 64U, true });
			const NavigationPublicationReceipt receipt = PublishNavigationCandidateAtProtectedBoundary(world, dirty, older);
			context.Expect(!receipt.accepted && receipt.reason == NavigationPublicationRejection::StaleSourceRevision,
				"A late older completion must fail closed instead of resurrecting removed geometry.");
			context.Expect(world.PublishedRoot() == newer,
				"Out-of-order completion must leave the newer root untouched.");
		});

		registry.Add("Navigation.Publication.RejectsOversizedCandidateWithoutClearingUnrelatedCoverage", [](TestContext& context) {
			NavigationWorld world({ { 59001U }, 1U });
			NavigationDirtyIndex dirty({});
			NavigationPublicationCandidate baseline{};
			baseline.expectedRootGeneration = world.PublishedRoot()->rootGeneration;
			baseline.expectedSourceRevision = 1U;
			baseline.coverage = {
				{ { 0, 0, 0, 1U }, 1U, NavigationCoverageState::Ready, 64U, true },
				{ { 4, 0, 0, 1U }, 1U, NavigationCoverageState::Ready, 64U, true } };
			context.Expect(PublishNavigationCandidateAtProtectedBoundary(world, dirty, baseline).accepted,
				"The fixture requires two independent ready coverage records.");
			const std::shared_ptr<const NavigationWorldRoot> before = world.PublishedRoot();

			NavigationPublicationCandidate oversized{};
			oversized.expectedRootGeneration = before->rootGeneration;
			oversized.expectedSourceRevision = 2U;
			oversized.coverage.push_back({ { 8, 0, 0, 1U }, 2U, NavigationCoverageState::Ready,
				std::numeric_limits<std::size_t>::max(), true });
			const NavigationPublicationReceipt receipt = PublishNavigationCandidateAtProtectedBoundary(world, dirty, oversized);
			context.Expect(!receipt.accepted, "An oversized candidate must be rejected before root replacement.");
			context.Expect(world.PublishedRoot() == before && world.PublishedRoot()->coverage.size() == 2U,
				"A rejected local update must preserve all unrelated coverage records.");
		});
	}
}
