// NavigationLargeWorldTests.cpp - canonical large-world and seam contracts.

#include "../TestHarness.h"

#include "Navigation/Ground/GroundPortalGraph.h"
#include "Navigation/Streaming/NavigationStreamingCoordinator.h"

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
	}
}
