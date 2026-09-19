// NavigationAgentPreparationTests.cpp - pure worker-safe navigation agent rules.

#include "../TestHarness.h"

#include "Navigation/Agents/NavigationAgentPreparation.h"

#include <cstdint>
#include <limits>

namespace TerraEngine::Tests {
	using namespace Navigation;

	namespace {
		// Each test starts with a complete, immutable capture packet. This keeps the
		// tests focused on worker preparation rather than live ObjectManager access.
		NavigationLocation Location(const NavigationWorldHandle& world,
			double x, double y = 0.0, double z = 0.0) {
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, y, z};
			return location;
		}

		NavigationAgentCapture Capture() {
			NavigationAgentCapture capture{};
			capture.runtime.identity.world = {NavigationWorldId{77U}, 1U};
			capture.runtime.identity.object.objectId[0] = 0x7AU;
			capture.runtime.identity.object.lifetimeGeneration = 1U;
			capture.runtime.identity.attachmentGeneration = 1U;
			capture.runtime.identity.runtimeSlot = 0U;
			capture.runtime.identity.runtimeSlotGeneration = 1U;
			capture.runtime.intentSequence = 1U;
			capture.runtime.destination.kind = NavigationDestinationKind::Location;
			capture.runtime.destination.location = Location(capture.runtime.identity.world, 10.0);

			capture.owner.object = capture.runtime.identity.object;
			capture.owner.location = Location(capture.runtime.identity.world, 0.0);
			capture.owner.activeInHierarchy = true;
			capture.owner.transformRevision = 1U;
			capture.routePoints = {capture.owner.location, capture.runtime.destination.location};
			capture.routeUsable = true;
			capture.observation = {1U, 1U, 1U, 1U, 1U, 1U};
			capture.publicationGeneration = 1U;
			capture.routeGeneration = 1U;
			return capture;
		}
	}

	void RegisterNavigationAgentPreparationTests(TestRegistry& registry) {
		registry.Add("Navigation.AgentPreparation.StartWaypointDoesNotArrive", [](TestContext& context) {
			const NavigationAgentCandidate candidate = PrepareNavigationAgent(Capture());

			context.Expect(candidate.runtime.state == NavigationAgentState::Following,
				"An agent standing on the first route point must advance to the next point instead of arriving early.");
			context.Expect(candidate.nextWaypointIndex == 1U && candidate.runtime.guidance.IsUsable(),
				"The produced guidance must identify the final waypoint as the next movement target.");
		});

		registry.Add("Navigation.AgentPreparation.ProgressedWaypointDoesNotTurnBack", [](TestContext& context) {
			auto capture = Capture();
			capture.owner.location = Location(capture.runtime.identity.world, 3.0);
			capture.routePoints = {
				Location(capture.runtime.identity.world, 0.0),
				capture.owner.location,
				capture.runtime.destination.location};
			capture.waypointIndex = 1U;
			const NavigationAgentCandidate candidate = PrepareNavigationAgent(capture);

			context.Expect(candidate.runtime.state == NavigationAgentState::Following
				&& candidate.nextWaypointIndex == 2U,
				"A persisted reached waypoint must advance forward and never reset the agent to route point zero.");
			context.Expect(candidate.runtime.guidance.nextWaypoint == capture.routePoints[2U],
				"Guidance after a reached intermediate corner must point ahead along the immutable route.");
		});

		registry.Add("Navigation.AgentPreparation.NoRouteFailsClosed", [](TestContext& context) {
			auto capture = Capture();
			capture.routeUsable = false;
			const NavigationAgentCandidate candidate = PrepareNavigationAgent(capture);

			context.Expect(candidate.runtime.state == NavigationAgentState::Requesting && candidate.requestReplan,
				"An unavailable route must request protected replanning rather than inventing a direct path.");
			context.Expect(!candidate.runtime.guidance.IsUsable(),
				"An unavailable route must publish movement-neutral guidance until a validated route exists.");
		});

		registry.Add("Navigation.AgentPreparation.MovingObjectTargetRequestsReplan", [](TestContext& context) {
			auto capture = Capture();
			capture.runtime.destination.kind = NavigationDestinationKind::Object;
			capture.runtime.destination.object.objectId[0] = 0xB4U;
			capture.runtime.destination.object.lifetimeGeneration = 1U;
			NavigationObjectEndpoint target{};
			target.object = capture.runtime.destination.object;
			target.location = Location(capture.runtime.identity.world, 11.0);
			target.activeInHierarchy = true;
			target.transformRevision = 2U;
			capture.target = target;
			const NavigationAgentCandidate candidate = PrepareNavigationAgent(capture);

			context.Expect(candidate.runtime.state == NavigationAgentState::Requesting && candidate.requestReplan,
				"A moving target beyond the configured displacement threshold must invalidate the old route.");
			context.Expect(!candidate.runtime.guidance.IsUsable(),
				"Target drift must withdraw prior movement guidance until the protected route request is replaced.");
		});

		registry.Add("Navigation.AgentPreparation.StoppedAndFailedRemainTerminal", [](TestContext& context) {
			auto stopped = Capture();
			stopped.runtime.state = NavigationAgentState::Stopped;
			const NavigationAgentCandidate stoppedCandidate = PrepareNavigationAgent(stopped);

			auto failed = Capture();
			failed.runtime.state = NavigationAgentState::Failed;
			const NavigationAgentCandidate failedCandidate = PrepareNavigationAgent(failed);

			context.Expect(stoppedCandidate.runtime.state == NavigationAgentState::Stopped
				&& !stoppedCandidate.runtime.guidance.IsUsable(),
				"Stop must remain terminal and must remove movement authority until a later command is admitted.");
			context.Expect(failedCandidate.runtime.state == NavigationAgentState::Failed
				&& !failedCandidate.runtime.guidance.IsUsable(),
				"A failed intent must remain terminal instead of retrying or reusing stale guidance in a worker frame.");
		});

		registry.Add("Navigation.AgentPreparation.HugeSignedCellDeltaFailsSafely", [](TestContext& context) {
			auto capture = Capture();
			capture.owner.location.position.cell.x = std::numeric_limits<std::int64_t>::lowest();
			capture.runtime.destination.location.position.cell.x = std::numeric_limits<std::int64_t>::max();
			capture.routePoints = {capture.owner.location, capture.runtime.destination.location};
			const NavigationAgentCandidate candidate = PrepareNavigationAgent(capture);

			context.Expect(candidate.runtime.state == NavigationAgentState::Failed
				&& !candidate.runtime.guidance.IsUsable(),
				"A cell delta that cannot be safely localized must fail closed instead of overflowing signed world coordinates.");
		});

		registry.Add("Navigation.AgentPreparation.ArrivesOnlyAtFinalSupportedEndpoint", [](TestContext& context) {
			auto capture = Capture();
			capture.owner.location = capture.runtime.destination.location;
			capture.routePoints = {
				Location(capture.runtime.identity.world, 0.0),
				capture.runtime.destination.location};
			capture.waypointIndex = 1U;
			const NavigationAgentCandidate candidate = PrepareNavigationAgent(capture);

			context.Expect(candidate.runtime.state == NavigationAgentState::Arrived,
				"Arrival is valid only after the owner reaches the final route point and the requested endpoint agrees.");
			context.Expect(!candidate.runtime.guidance.IsUsable(),
				"Arrival must retire movement guidance rather than leaving a controller with a stale direction.");
		});
	}
}
