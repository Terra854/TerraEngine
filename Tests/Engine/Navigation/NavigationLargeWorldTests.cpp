// NavigationLargeWorldTests.cpp - canonical large-world and seam contracts.

#include "../TestHarness.h"

#include "Navigation/Ground/GroundPortalGraph.h"
#include "Navigation/Streaming/NavigationStreamingCoordinator.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;
	using namespace TerraEngine::Navigation::Ground;

	void RegisterNavigationLargeWorldTests(TestRegistry& registry)
	{
		registry.Add("Navigation.LargeWorld.CanonicalShardKeysRemainEquivalentAfterReferenceShift", [](TestContext& context) {
			NavigationStreamingCoordinator coordinator;
			const auto near = coordinator.RequestInterest({0, 0, 0, 0, 0U, 17U});
			const auto millionKm = coordinator.RequestInterest({1000000000LL, -1000000000LL, 0, 0, 0U, 17U});
			context.Expect(near.size() == 1U && millionKm.size() == 1U,
				"A reference shift must preserve one canonical bounded coverage request.");
			context.Expect(near.front().profileBuildKey == millionKm.front().profileBuildKey
				&& near.front().verticalBand == millionKm.front().verticalBand,
				"Million-kilometre localization must preserve profile and vertical-band identity.");
		});

		registry.Add("Navigation.LargeWorld.VerticalBandsAndAllOverlappingSeamIntervalsRemainAvailable", [](TestContext& context) {
			NavigationStreamingCoordinator coordinator;
			const auto requested = coordinator.RequestInterest({0, 0, -1, 1, 0U, 19U});
			context.Expect(requested.size() == 3U, "Interest must retain every requested vertical band.");
			GroundPortalGraph graph;
			std::vector<GroundBoundaryInterval> intervals{
				{{0, 0, 0, 19U}, 1U, 0U, true, 0U, 0.0, 4.0, 1.0F, true, true},
				{{1, 0, 0, 19U}, 1U, 0U, false, 0U, 0.0, 2.0, 1.0F, true, true},
				{{1, 0, 0, 19U}, 1U, 0U, false, 0U, 2.0, 4.0, 1.0F, true, true}};
			context.Expect(graph.PublishBoundaryIntervals(intervals, 0U),
				"Seam publication must accept all overlapping boundary intervals as one immutable generation.");
			context.Expect(graph.AcquireSnapshot().Portals().size() == 4U,
				"The portal graph must retain every compatible interval, not only a boundary center sample.");
		});

		registry.Add("Navigation.LargeWorld.RejectedPortalRootTransactionLeavesBothAuthoritiesUnchanged", [](TestContext& context) {
			GroundPortalGraph graph;
			NavigationWorld world({{901U}, 1U});
			const GroundPortalGraphSnapshot portalsBefore = graph.AcquireSnapshot();
			const std::shared_ptr<const NavigationWorldRoot> rootBefore = world.PublishedRoot();
			bool callbackRan = false;
			const std::vector<GroundBoundaryInterval> intervals{
				{{0, 0, 0, 91U}, 1U, 0U, true, 0U, 0.0, 8.0, 1.0F, true, true},
				{{1, 0, 0, 91U}, 1U, 0U, false, 0U, 0.0, 8.0, 1.0F, true, true}};
			const bool published = graph.PublishBoundaryIntervalsTransactionally(
				intervals, portalsBefore.Revision(), [&callbackRan] {
					// A root rejection must run before the graph swaps its candidate.
					callbackRan = true;
					return false;
				});
			const GroundPortalGraphSnapshot portalsAfter = graph.AcquireSnapshot();
			const std::shared_ptr<const NavigationWorldRoot> rootAfter = world.PublishedRoot();
			context.Expect(callbackRan && !published,
				"The transaction fixture requires the typed root callback to reject publication.");
			context.Expect(portalsAfter.Revision() == portalsBefore.Revision()
				&& portalsAfter.Portals().empty(),
				"A rejected root callback must leave the immutable portal graph unchanged.");
			context.Expect(rootAfter == rootBefore
				&& rootAfter->rootGeneration == rootBefore->rootGeneration,
				"A rejected portal transaction must not mutate the paired navigation world root.");
		});

		registry.Add("Navigation.LargeWorld.StackedFloorsInOneVerticalBandDoNotCreateCrossFloorPortals", [](TestContext& context) {
			GroundPortalGraph graph;
			auto edge = [](NavigationCoverageKey coverage, bool positive, float height) {
				GroundBoundaryInterval interval{coverage, 1U, 0U, positive, 0U,
					0.0, 32.0, 1.0F, true, true};
				// The test represents two separate horizontal floor edges in the same
				// coarse 128m vertical band. A 0.5m climb may join matching floors only.
				interval.heightAtIntervalStart = height;
				interval.heightAtIntervalEnd = height;
				interval.maximumStepHeightMetres = 0.5F;
				return interval;
			};
			const NavigationCoverageKey left{0, 0, 0, 71U};
			const NavigationCoverageKey right{1, 0, 0, 71U};
			const std::vector<GroundBoundaryInterval> intervals{
				edge(left, true, 0.0F), edge(right, false, 0.0F),
				edge(left, true, 10.0F), edge(right, false, 10.0F)};
			context.Expect(graph.PublishBoundaryIntervals(intervals, 0U),
				"The graph fixture requires an immutable stacked-floor seam publication.");
			const auto& portals = graph.AcquireSnapshot().Portals();
			const bool onlyMatchingFloors = portals.size() == 4U
				&& std::all_of(portals.begin(), portals.end(), [](const GroundPortalRecord& portal) {
					return std::abs(portal.fromHeightMetres - portal.toHeightMetres) <= 0.05F;
				});
			context.Expect(onlyMatchingFloors,
				"Separate floors in one vertical band must produce only same-height directed portals, never a lower-to-upper seam shortcut.");
		});

		registry.Add("Navigation.LargeWorld.MissingNeighboursAreUnavailableAndLeasesInvalidateBeforeRetire", [](TestContext& context) {
			NavigationStreamingCoordinator coordinator;
			const NavigationCoverageKey key{4, -4, 0, 23U};
			context.Expect(coordinator.PublishReady(key, 5U, 1024U), "The fixture requires one ready coverage record.");
			context.Expect(coordinator.AcquireRouteLease(key, "route-1", 10U), "A ready route must hold a lease on its coverage.");
			context.Expect(coordinator.MarkInvalidated(key), "Changed coverage must become dirty before retirement.");
			context.Expect(coordinator.Observe(key).reason == NavigationStreamingReason::Invalidated,
				"Invalidation must be observable before an old generation is retired.");
			context.Expect(coordinator.RetireExpired(5U) == 0U, "An active route lease must defer retirement.");
			context.Expect(coordinator.RetireExpired(10U) == 1U, "Expired leases must permit bounded retirement.");
			context.Expect(coordinator.Observe({9, -4, 0, 23U}).reason == NavigationStreamingReason::MissingCoverage,
				"A missing neighbour must return explicit unavailable coverage rather than Unreachable.");
		});

		registry.Add("Navigation.LargeWorld.CompleteCoverageDistinguishesObstructionFromMissingNeighbour", [](TestContext& context) {
			// These ready keys model a fully built strip. A profile can still have no
			// portal through it (for example because a low roof removed every polygon),
			// which is an unreachable route rather than a streaming coverage miss.
			const std::vector<NavigationCoverageKey> complete{
				{-1, 0, 0, 31U}, {0, 0, 0, 31U}, {1, 0, 0, 31U}};
			const GroundPortalSearchResult roofDivider = ClassifyPortalMissingNeighbor(
				complete.front(), complete.back(), complete, 3U);
			context.Expect(roofDivider.outcome == NavigationOutcome::Unreachable
				&& roofDivider.reason == NavigationReason::MissingSupport,
				"A complete cardinal coverage chain must classify a fully built roof divider as unreachable, not missing coverage.");
			const std::vector<NavigationCoverageKey> missingMiddle{
				{-1, 0, 0, 31U}, {1, 0, 0, 31U}};
			const GroundPortalSearchResult missingCoverage = ClassifyPortalMissingNeighbor(
				missingMiddle.front(), missingMiddle.back(), missingMiddle, 3U);
			context.Expect(missingCoverage.outcome == NavigationOutcome::UnavailableCoverage
				&& missingCoverage.reason == NavigationReason::MissingNeighbor,
				"A genuine missing intermediate shard must remain unavailable coverage rather than being reclassified as an obstruction.");
			const std::vector<NavigationCoverageKey> stackedFloor{
				{-1, 0, 0, 31U}, {0, 0, 1, 31U}, {1, 0, 0, 31U}};
			const GroundPortalSearchResult stackedCoverage = ClassifyPortalMissingNeighbor(
				stackedFloor.front(), stackedFloor.back(), stackedFloor, 3U);
			context.Expect(stackedCoverage.outcome == NavigationOutcome::UnavailableCoverage
				&& stackedCoverage.reason == NavigationReason::MissingNeighbor,
				"Coverage from a different vertical band must not bridge stacked floors when classifying a portal failure.");
		});
	}
}
