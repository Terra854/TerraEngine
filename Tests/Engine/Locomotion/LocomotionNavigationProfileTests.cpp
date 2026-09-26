// LocomotionNavigationProfileTests.cpp - copied ground-route compatibility.

#include "../TestHarness.h"

#include "Locomotion/LocomotionNavigationCompatibility.h"
#include "Locomotion/LocomotionPreparation.h"

#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;

		NavigationCompatibilityInput Compatible() {
			NavigationCompatibilityInput input{};
			input.profile.radiusMetres = 0.5;
			input.profile.heightMetres = 2.0;
			input.profile.clearanceMarginMetres = 0.02;
			input.profile.stepHeightMetres = 0.4;
			input.profile.maxSlopeDegrees = 50.0;
			return input;
		}

		// A complete copied guidance record lets these tests exercise worker math
		// without asking a live agent, route provider or ECS component for state.
		LocomotionPreparationInput NavigationInput() {
			LocomotionPreparationInput input{};
			auto& record = input.runtime;
			record.handle.sceneLifetime = 7U;
			record.handle.objectId[0] = 1U;
			record.handle.objectGeneration = 2U;
			record.handle.attachmentGeneration = 3U;
			record.handle.slotIndex = 4U;
			record.handle.slotGeneration = 5U;
			record.bindings.configurationRevision = 1U;
			record.bindings.maximumSpeed = 5.0;
			record.bindings.controller.world = {{8U}, 1U};
			record.bindings.controller.character = {{8U}, 1U, 9U, 2U};
			record.bindings.controller.objectId = record.handle.objectId;
			record.bindings.controller.objectGeneration = 2U;
			record.bindings.controller.componentGeneration = 3U;
			record.bindings.controllerSupported = true;
			record.bindings.frame = {Physics::kPrimaryPhysicsFrameId, 1U};
			record.authority.handle = record.handle;
			record.authority.controller = record.bindings.controller;
			record.authority.ownerToken = 31U;
			record.authority.ownerProducerId = 41U;
			record.authority.state = AuthorityState::Controlled;
			record.authority.authorityEpoch = 1U;
			record.authority.configurationRevision = 1U;
			record.intent = MovementIntent{31U, IntentKind::Navigation, 1U,
				DirectionSpace::World, {0.0, 0.0, 1.0}, 5.0,
				IntentLifetime::UntilStopped, 2U};
			input.physical.handle = record.bindings.controller.character;
			input.physical.world = record.bindings.controller.world;
			input.physical.objectId = record.handle.objectId;
			input.physical.objectGeneration = 2U;
			input.physical.componentGeneration = 3U;
			input.physical.tick = 17U;
			input.physical.completedStepDuration = 0.02F;
			input.physical.requestedPlanarVelocity = {0.0F, 0.0F, 5.0F};
			input.physical.realizedDisplacement = {0.0F, 0.0F, 0.1F};
			input.physical.motionDiscontinuity = false;
			input.frame = record.bindings.frame;
			input.locomotionUpdateEpoch = 2U;
			input.physicalMotionEpoch = 3U;
			input.navigationCompatibility = {true, LocomotionReason::None,
				NavigationCompatibilityField::None, 0.0, 0.0};
			auto& guidance = input.navigationGuidance.emplace();
			guidance.identity.world = {{77U}, 1U};
			guidance.identity.object.objectId = record.handle.objectId;
			guidance.identity.object.lifetimeGeneration = 2U;
			guidance.identity.attachmentGeneration = 3U;
			guidance.identity.runtimeSlot = 4U;
			guidance.identity.runtimeSlotGeneration = 5U;
			guidance.configurationRevision = 1U;
			guidance.intentSequence = 1U;
			guidance.publicationGeneration = 1U;
			guidance.routeGeneration = 1U;
			guidance.observation.sceneGeneration = 7U;
			guidance.observation.terrainPublicationGeneration = 1U;
			guidance.observation.worldPublicationGeneration = 1U;
			guidance.observation.sourceIndexRevision = 1U;
			guidance.state = Navigation::NavigationAgentState::Following;
			guidance.outcome = Navigation::NavigationOutcome::Complete;
			guidance.nextWaypoint.world = guidance.identity.world;
			guidance.suggestedDirection = {0.0, 0.0, 1.0};
			guidance.remainingDistanceMetres = 0.5;
			guidance.usable = true;
			return input;
		}
	}

	void RegisterLocomotionNavigationProfileTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.NavigationProfile.UprightGroundCapsuleOnly", [](TestContext& context) {
			auto input = Compatible();
			context.Expect(CheckNavigationCompatibility(input).compatible,
				"An upright enabled ground-origin capsule matching its profile is compatible.");
			input.shape = Physics::PhysicsShapeKind::Cylinder;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::Shape,
				"A cylinder's general facing support cannot authorize capsule navigation.");
			input = Compatible();
			input.upDirection = {0.0F, 0.997F, 0.0F};
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::UpDirection,
				"Ground navigation requires +Y within a unit-vector tolerance.");
			input = Compatible();
			input.localCenter.y = 0.95F;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::LocalCenter,
				"The owner origin is ground level only when center Y equals half total height.");
			input = Compatible();
			input.ownerColliderSetEnabled = true;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::OwnerColliderSet,
				"An enabled owner ColliderSet would create ambiguous self-contact.");
		});

		registry.Add("Engine.Locomotion.NavigationProfile.ProfileDimensionsNameMismatch", [](TestContext& context) {
			auto input = Compatible();
			input.profile.radiusMetres = 0.48;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::Radius,
				"A route profile cannot reserve less than the controller's effective radius.");
			input = Compatible();
			input.profile.heightMetres = 1.98;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::Height,
				"A route profile cannot reserve less than the controller's total height.");
			input = Compatible();
			input.totalHeightMetres = 0.8;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::Height,
				"An impossible capsule shorter than its diameter reports Height, not Radius.");
			input = Compatible();
			input.profile.clearanceMarginMetres = 0.0;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::ClearanceMargin,
				"Route clearance must cover physical contact padding.");
			input = Compatible();
			input.profile.stepHeightMetres = 0.42;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::StepHeight,
				"Navigation may not choose a step higher than the controller can climb.");
			input = Compatible();
			input.profile.maxSlopeDegrees = 50.01;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::SlopeAngle,
				"Navigation may not choose a slope steeper than the controller supports.");
		});

		registry.Add("Engine.Locomotion.NavigationProfile.ScaleAndDimensionedTolerances", [](TestContext& context) {
			auto input = Compatible();
			input.worldScale = {1.2F, 1.2F, 1.2F};
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::WorldScale,
				"Provider geometry is unscaled, so a non-unit authored scale must fail closed.");
			input = Compatible();
			input.worldScale = {1.0F, 2.0F, 1.0F};
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::WorldScale,
				"A nonuniform or sheared effective scale cannot be guessed into a capsule radius.");
			input = Compatible();
			input.profile.radiusMetres = 0.4995;
			input.profile.maxSlopeDegrees = 50.0005;
			input.upDirection.x = 0.0005F;
			context.Expect(CheckNavigationCompatibility(input).compatible,
				"Separate 0.001 metre, degree and unit-axis tolerances admit small float roundoff.");
			input.profile.maxSlopeDegrees = 50.005;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::SlopeAngle,
				"The angular tolerance remains 0.001 degree, not a reused distance unit.");
		});

		registry.Add("Engine.Locomotion.NavigationProfile.EnablementAndInvalidProfile", [](TestContext& context) {
			auto input = Compatible();
			input.controllerEnabled = false;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::ControllerEnabled,
				"Disabled controller cannot follow a route.");
			input = Compatible();
			input.physicsEnabled = false;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::PhysicsEnabled,
				"A controller with disabled physics cannot follow a route.");
			input = Compatible();
			input.profile.radiusMetres = -1.0;
			context.Expect(CheckNavigationCompatibility(input).field == NavigationCompatibilityField::InvalidProfile,
				"Malformed profile values reject before comparison against controller dimensions.");
		});

		registry.Add("Engine.Locomotion.NavigationProfile.BrakingUsesRemainingRouteBudget", [](TestContext& context) {
			auto input = NavigationInput();
			const auto output = PrepareLocomotionMotion(input);
			const double budget = 0.4;
			const double cap = std::sqrt(2.0 * input.braking * budget);
			context.Expect(output.observation.navigationState == NavigationMotionState::Following
				&& std::abs(output.observation.remainingRouteDistanceMetres - 0.5) < 0.0001
				&& std::abs(output.target.planarVelocity.z - cap) < 0.001
				&& std::abs(output.target.approachDistanceMetres - budget) < 0.0001,
				"The target brakes against remaining route distance minus arrival tolerance, with a physical step budget.");
		});

		registry.Add("Engine.Locomotion.NavigationProfile.WaitingBlockedAndObservedArrivalDiffer", [](TestContext& context) {
			auto input = NavigationInput();
			input.navigationGuidance->usable = false;
			input.navigationGuidance->state = Navigation::NavigationAgentState::Requesting;
			const auto waiting = PrepareLocomotionMotion(input);
			context.Expect(waiting.observation.navigationState == NavigationMotionState::Waiting
				&& waiting.target.planarVelocity.z == 0.0F,
				"Unavailable guidance waits with a zero target but does not imply arrival.");
			input = NavigationInput();
			input.physical.realizedDisplacement = {};
			const auto blocked = PrepareLocomotionMotion(input);
			context.Expect(blocked.observation.navigationState == NavigationMotionState::Blocked
				&& blocked.target.planarVelocity.z > 0.0F,
				"A valid route plus requested movement and insufficient realized progress is blocked, not arrived.");
			input = NavigationInput();
			input.navigationGuidance->usable = false;
			input.navigationGuidance->state = Navigation::NavigationAgentState::Arrived;
			input.navigationGuidance->remainingDistanceMetres = 0.0;
			const auto arrived = PrepareLocomotionMotion(input);
			context.Expect(arrived.observation.navigationState == NavigationMotionState::Arrived
				&& arrived.target.planarVelocity.z == 0.0F,
				"Only agent-observed Arrived evidence produces an arrival state; zero target alone never does.");
		});

		registry.Add("Engine.Locomotion.NavigationProfile.MismatchReachesObservation", [](TestContext& context) {
			auto input = NavigationInput();
			input.navigationCompatibility = {false, LocomotionReason::UnsupportedController,
				NavigationCompatibilityField::Radius, 0.5, 0.4};
			const auto output = PrepareLocomotionMotion(input);
			context.Expect(output.reason == LocomotionReason::UnsupportedController
				&& output.observation.navigationState == NavigationMotionState::Failed
				&& output.observation.navigationCompatibilityField == NavigationCompatibilityField::Radius
				&& output.observation.navigationControllerValue == 0.5
				&& output.observation.navigationProfileValue == 0.4
				&& output.target.planarVelocity.z == 0.0F,
				"A profile mismatch remains field-specific and neutralizes movement in the observation.");
		});
	}
}
