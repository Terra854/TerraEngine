// NavigationAgentPreviewTests.cpp - immutable editor presentation contracts for navigation agents.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Navigation/NavigationSceneOverlay.h"
#include "Editor/TerraEngine-Editor/Windows/NavigationWindow.h"

namespace TerraEngine::EditorTests {
	using namespace TerraEngine::Editor::Navigation;
	using namespace TerraEngine::Navigation;

	namespace {
		NavigationLocation Location(double x) {
			NavigationLocation location{};
			location.world = {NavigationWorldId{1U}, 1U};
			location.position.offsetMetres = {x, 0.0, 0.0};
			return location;
		}

		NavigationAgentOverlayRecord FollowingAgent() {
			NavigationAgentOverlayRecord agent{};
			agent.guidance.identity.world = {NavigationWorldId{1U}, 1U};
			agent.guidance.identity.object.objectId = GenerateUUID();
			agent.guidance.identity.object.lifetimeGeneration = 1U;
			agent.guidance.identity.attachmentGeneration = 1U;
			agent.guidance.identity.runtimeSlot = 0U;
			agent.guidance.identity.runtimeSlotGeneration = 1U;
			agent.guidance.identity.runtimeSlot = 0U;
			agent.guidance.identity.runtimeSlotGeneration = 1U;
			agent.guidance.configurationRevision = 1U;
			agent.guidance.intentSequence = 1U;
			agent.guidance.publicationGeneration = 1U;
			agent.guidance.routeGeneration = 1U;
			agent.guidance.observation = {1U, 1U, 1U, 1U, 1U, 1U};
			agent.guidance.state = NavigationAgentState::Following;
			agent.guidance.outcome = NavigationOutcome::Complete;
			agent.guidance.nextWaypoint = Location(2.0);
			agent.guidance.suggestedDirection = {1.0, 0.0, 0.0};
			agent.guidance.remainingDistanceMetres = 3.0;
			agent.guidance.usable = true;
		// The route is already copied by the selected-record API; these values are
		// never aliases to a mutable NavigationAgentSystem binding.
			agent.routePoints = {Location(0.0).position, Location(2.0).position, Location(5.0).position};
			agent.routeCurrent = true;
			return agent;
		}
	}

	void RegisterNavigationAgentPreviewTests(TestRegistry& registry) {
		registry.Add("Editor.Navigation.AgentPreview.DirectPreviewRemainsComponentFree", [](TestContext& context) {
			NavigationSceneOverlayInput input{};
			input.routePoints = {Location(0.0).position, Location(4.0).position};
			const NavigationSceneOverlaySnapshot overlay = ExtractNavigationOverlay(input, 16U);
			const NavigationPreviewPresentation preview = BuildNavigationPreview(
				{NavigationOutcome::Complete, NavigationReason::None});

			context.Expect(preview.routeVisible && !preview.isPreparing,
				"A complete direct-query preview must remain visible without selecting or attaching an agent.");
			context.Expect(overlay.routePoints.size() == 2U && !overlay.selectedAgent.has_value(),
				"Direct preview points must survive an empty agent overlay input.");
		});

		registry.Add("Editor.Navigation.AgentPreview.SelectedGuidanceUsesOneImmutablePresentationRecord", [](TestContext& context) {
			NavigationSceneOverlayInput input{};
			input.selectedAgent = FollowingAgent();
			input.agentDiagnostics.activeAgents = 1U;
			input.agentDiagnostics.followingAgents = 1U;
			input.agentDiagnostics.pendingCommands = 2U;
			input.agentDiagnostics.currentOwnedMemoryBytes = 512U;
			const NavigationSceneOverlaySnapshot overlay = ExtractNavigationOverlay(input, 16U);
			const NavigationAgentOverlayPresentation presentation = BuildNavigationAgentOverlayPresentation(overlay.selectedAgent);

			context.Expect(overlay.selectedAgent.has_value() && overlay.selectedAgent->routePoints.size() == 3U
				&& overlay.agentDiagnostics.followingAgents == 1U,
				"Selected-agent route, state, and aggregate diagnostics must come from one copied presentation input.");
			context.Expect(presentation.routeVisible && presentation.usableGuidance
				&& presentation.state == NavigationAgentState::Following
				&& presentation.reason == NavigationReason::None,
				"Only current Following guidance may render as usable selected-agent movement advice.");
		});

		registry.Add("Editor.Navigation.AgentPreview.UnusableSelectedRouteIsDistinctAndBounded", [](TestContext& context) {
			NavigationSceneOverlayInput input{};
			NavigationAgentOverlayRecord stale = FollowingAgent();
			stale.guidance.state = NavigationAgentState::Requesting;
			stale.guidance.outcome = NavigationOutcome::UnavailableCoverage;
			stale.guidance.reason = NavigationReason::MissingNeighbor;
			stale.guidance.usable = false;
			stale.routeCurrent = false;
			input.selectedAgent = stale;
			input.coverage.resize(4U);
			input.routePoints = {Location(0.0).position, Location(1.0).position};
			const NavigationSceneOverlaySnapshot overlay = ExtractNavigationOverlay(input, 4U);
			const NavigationAgentOverlayPresentation presentation = BuildNavigationAgentOverlayPresentation(overlay.selectedAgent);

			context.Expect(overlay.truncated && overlay.selectedAgent.has_value()
				&& overlay.selectedAgent->routePoints.empty(),
				"A bounded overlay must retain selected diagnostics while omitting an unusable route rather than presenting stale waypoints as guidance.");
			context.Expect(!presentation.routeVisible && !presentation.usableGuidance
				&& presentation.state == NavigationAgentState::Requesting
				&& presentation.reason == NavigationReason::MissingNeighbor,
				"Requesting or invalidated guidance must expose its reason but never render as a current route.");
		});
	}
}
