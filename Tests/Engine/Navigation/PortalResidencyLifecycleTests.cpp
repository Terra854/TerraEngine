// Exercise actual immutable portal publication and coverage lease transitions.
#include "TestHarness.h"
#include "Navigation/Ground/GroundPortalGraph.h"
#include "Navigation/Streaming/NavigationStreamingCoordinator.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterPortalResidencyLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Navigation.PortalResidency.OnlyInwardOrientedAdjacentEdgesJoin", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			using namespace TerraEngine::Navigation::Ground;
			for(const std::uint8_t axis : {0U, 1U})
				for(const bool reverse : {false, true})
				{
					const NavigationCoverageKey lower{-2, -3, 0, 19U};
					auto upper = lower;
					if(axis == 0U) ++upper.shardX; else ++upper.shardZ;
					std::vector<GroundBoundaryInterval> intervals{
						{lower, 1U, axis, true, 0U, 0.0, 4.0, 1.0F, true, true},
						{upper, 1U, axis, false, 0U, 0.0, 4.0, 1.0F, true, true}};
					if(reverse) std::reverse(intervals.begin(), intervals.end());
					GroundPortalGraph graph;
					context.Expect(graph.PublishBoundaryIntervals(intervals, 0U), "The actual seam must publish a complete portal candidate.");
					const auto retained = graph.AcquireSnapshot();
					context.Expect(retained.Portals().size() == 2U, "The lower positive and upper negative edges form two directed real portals.");
					for(auto& interval : intervals) interval.positiveSide = !interval.positiveSide;
					context.Expect(graph.PublishBoundaryIntervals(intervals, retained.Revision()), "A second immutable portal generation must publish.");
					context.Expect(graph.AcquireSnapshot().Portals().empty() && retained.Portals().size() == 2U,
						"Outward edges must not invent a seam, and prior immutable readers must remain valid.");
				}
		});

		registry.Add("Navigation.PortalResidency.OrdinaryInterestReloadsRetiredCoverage", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationStreamingCoordinator coordinator(2U);
			const NavigationCoverageInterest interest{4, -4, 0, 0, 0U, 23U};
			for(std::uint64_t cycle = 1U; cycle <= 8U; ++cycle)
			{
				const auto requested = coordinator.RequestInterest(interest);
				context.Expect(requested.size() == 1U, "Ordinary renewed interest must request the same coverage key.");
				const auto key = requested.front();
				const auto oldTicket = coordinator.BeginPublication(key);
				context.Expect(coordinator.Observe(key).state == NavigationCoverageState::Requested,
					"A retired key must become requested without an explicit invalidation workaround.");
				context.Expect(coordinator.PublishReady(coordinator.BeginPublication(key), cycle, 1024U) && coordinator.AcquireRouteLease(key, "route", cycle * 2U),
					"The renewed generation must become ready and support a real route lease.");
				context.Expect(coordinator.RetireExpired(cycle * 2U - 1U) == 0U,
					"An unexpired route lease must retain its ready coverage.");
				context.Expect(coordinator.RetireExpired(cycle * 2U) == 1U,
					"The exact lease expiry must permit retirement before the next return.");
				context.Expect(!coordinator.PublishReady(oldTicket, cycle, 1024U) && !coordinator.AcquireRouteLease(key, "stale", cycle * 2U + 1U),
					"Late completion cannot resurrect retired coverage or reacquire a lease.");
				context.Expect(coordinator.RequestInterest(interest).size() == 1U && !coordinator.PublishReady(oldTicket, cycle, 1024U),
					"Renewed same-key interest must revoke every earlier lifetime ticket.");
				context.Expect(coordinator.PublishReady(coordinator.BeginPublication(key), cycle, 64U) && coordinator.RetireExpired(cycle * 2U) == 1U,
					"Fresh work can publish after the stale ticket was rejected.");
			}
		});

		registry.Add("Navigation.PortalResidency.DisjointInterestRetainsBoundedStorage", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationStreamingCoordinator coordinator(2U);
			for(std::int64_t index = 0; index < 12; ++index)
			{
				const auto requested = coordinator.RequestInterest({index, -index, 0, 0, 0U, 23U});
				context.Expect(requested.size() == 1U && coordinator.RetainedCoverageCount() <= 2U,
					"Retired coverage must release bounded retention so ordinary disjoint interest remains admissible.");
				context.Expect(coordinator.PublishReady(coordinator.BeginPublication(requested.front()), static_cast<std::uint64_t>(index + 1), 64U),
					"Each newly requested generation must publish without a manual invalidation workaround.");
				context.Expect(coordinator.RetireExpired(static_cast<std::uint64_t>(index + 1)) == 1U,
					"Unleased ready coverage must retire before moving to the next region.");
			}
		});
	}
}
