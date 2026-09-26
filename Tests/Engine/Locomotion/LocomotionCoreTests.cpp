// LocomotionCoreTests.cpp - worker-safe velocity and feedback preparation.

#include "../TestHarness.h"

#include "Locomotion/LocomotionPreparation.h"

#include <cmath>
#include <limits>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;

		// All identities are complete so preparation can distinguish a valid copied
		// controller packet from a stale or unsupported one without live ECS access.
		LocomotionPreparationInput MakeInput() {
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
			record.bindings.controller.objectGeneration = record.handle.objectGeneration;
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
			input.physical.handle = record.bindings.controller.character;
			input.physical.world = record.bindings.controller.world;
			input.physical.objectId = record.handle.objectId;
			input.physical.objectGeneration = record.handle.objectGeneration;
			input.physical.componentGeneration = 3U;
			input.physical.tick = 17U;
			input.physical.groundState = Physics::PhysicsCharacterGroundState::OnGround;
			input.physical.supported = true;
			input.physical.completedStepDuration = 0.02F;
			input.physical.realizedDisplacement = {0.1F, 0.0F, 0.0F};
			input.physical.motionDiscontinuity = false;
			input.frame = record.bindings.frame;
			input.locomotionUpdateEpoch = 2U;
			input.physicalMotionEpoch = 3U;
			record.intent = MovementIntent{31U, IntentKind::Player, 1U,
				DirectionSpace::World, {1.0, 4.0, 1.0}, 4.0,
				IntentLifetime::OneUpdate, 2U};
			return input;
		}

		bool Near(double actual, double expected, double tolerance = 0.0002) {
			return std::abs(actual - expected) < tolerance;
		}
	}

	void RegisterLocomotionCoreTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Core.WorldDirectionNormalizesPlanarDiagonal", [](TestContext& context) {
			const auto output = PrepareLocomotionMotion(MakeInput());
			const double component = 4.0 / std::sqrt(2.0);
			context.Expect(output.reason == LocomotionReason::None && output.publishTarget,
				"A complete copied velocity input must produce a publishable target.");
			context.Expect(Near(output.target.planarVelocity.x, component)
				&& Near(output.target.planarVelocity.y, 0.0)
				&& Near(output.target.planarVelocity.z, component),
				"World-space diagonal input must project off up and normalize to requested speed.");
			context.Expect(Near(output.observation.desiredVelocity.x, component)
				&& Near(output.observation.desiredVelocity.z, component),
				"The copied observation must report the same desired planar velocity.");
		});

		registry.Add("Engine.Locomotion.Core.LocalSpaceAndCandidateAreMathOnly", [](TestContext& context) {
			auto input = MakeInput();
			input.runtime.authority.state = AuthorityState::Idle;
			input.runtime.authority.ownerToken = 0U;
			input.runtime.intent.reset();
			input.candidateIntent = MovementIntent{0U, IntentKind::Script, 8U,
				DirectionSpace::ControllerLocal, {0.0, 0.0, 1.0}, 3.0,
				IntentLifetime::UntilStopped, 2U};
			const float halfSqrt = std::sqrt(0.5F);
			input.physical.pose.rotation = {0.0F, halfSqrt, 0.0F, halfSqrt};
			const auto output = PrepareLocomotionMotion(input);
			context.Expect(output.reason == LocomotionReason::None && !output.publishTarget,
				"An Idle owner's candidate can be prepared but cannot authorize publication.");
			context.Expect(Near(output.target.planarVelocity.x, 3.0)
				&& Near(output.target.planarVelocity.z, 0.0),
				"Controller-local forward follows the copied controller orientation.");
		});

		registry.Add("Engine.Locomotion.Core.RejectsNonfiniteDirectionAndReportsExactFeedbackTick", [](TestContext& context) {
			auto input = MakeInput();
			input.runtime.intent->direction.x = std::numeric_limits<double>::infinity();
			context.Expect(PrepareLocomotionMotion(input).reason == LocomotionReason::InvalidArgument,
				"A non-finite copied direction must never reach the Physics target.");
			input = MakeInput();
			auto output = PrepareLocomotionMotion(input);
			context.Expect(output.observation.physicalTick == 17U
				&& Near(output.observation.observedPlanarSpeed, 5.0)
				&& output.observation.motionHistoryAvailable
				&& output.observation.grounded,
				"Realized speed uses displacement/duration from the same completed physical tick.");
			input.physical.motionDiscontinuity = true;
			output = PrepareLocomotionMotion(input);
			context.Expect(!output.observation.motionHistoryAvailable
				&& Near(output.observation.observedPlanarSpeed, 0.0),
				"Teleport or discontinuity suppresses derivative motion feedback.");
		});

		registry.Add("Engine.Locomotion.Core.ExpiryAndLargeFiniteDirectionStayBounded", [](TestContext& context) {
			auto input = MakeInput();
			input.runtime.intent->lifetime = IntentLifetime::UntilStopped;
			context.Expect(PrepareLocomotionMotion(input).reason == LocomotionReason::InvalidArgument,
				"Pure preparation also rejects a malformed persistent Player intent.");
			input.runtime.intent->lifetime = IntentLifetime::OneUpdate;
			input.runtime.intent->acceptedUpdateEpoch = 1U;
			const auto expired = PrepareLocomotionMotion(input);
			context.Expect(Near(expired.target.planarVelocity.x, 0.0)
				&& !expired.target.clearImmediately && expired.publishTarget,
				"A missed OneUpdate refresh brakes toward zero rather than stopping ownership.");
			input.runtime.intent->acceptedUpdateEpoch = 2U;
			input.runtime.intent->direction = {1.0e308, 0.0, 1.0e308};
			const auto huge = PrepareLocomotionMotion(input);
			context.Expect(huge.reason == LocomotionReason::None
				&& std::isfinite(huge.target.planarVelocity.x)
				&& Near(huge.target.planarVelocity.x, 4.0 / std::sqrt(2.0)),
				"Large finite input normalizes safely without NaN or float overflow.");
			input.runtime.authority.state = AuthorityState::Stopped;
			input.runtime.intent.reset();
			input.candidateIntent = MovementIntent{31U, IntentKind::Player, 3U,
				DirectionSpace::World, {1.0, 0.0, 0.0}, 2.0,
				IntentLifetime::OneUpdate, 2U};
			const auto resume = PrepareLocomotionMotion(input);
			context.Expect(!resume.publishTarget && !resume.target.clearImmediately
				&& resume.target.hasFacing && Near(resume.target.planarVelocity.x, 2.0),
				"A pending resume has complete math without inheriting the old Stop clear flag.");
		});
	}
}
