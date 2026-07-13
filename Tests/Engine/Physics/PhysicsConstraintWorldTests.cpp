// PhysicsConstraintWorldTests.cpp - provider-neutral constraint break policy.

#include "../TestHarness.h"

#include "Physics/PhysicsWorld.h"

#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	void RegisterPhysicsConstraintWorldTests(TestRegistry& registry) {
		registry.Add("Physics.ConstraintWorld.BreaksOnceAndRequiresNewerReset", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "A valid world must exist before runtime constraint registration.");
			const PhysicsConstraintHandle handle = world.AllocateConstraint();
			PhysicsConstraintRuntimeRegistration registration;
			registration.handle = handle;
			registration.ownerObjectId = TerraEngine::GenerateUUID();
			registration.ownerObjectGeneration = 3U;
			registration.componentGeneration = 5U;
			registration.recordId = TerraEngine::GenerateUUID();
			registration.recordGeneration = 7U;
			registration.breakForce = 10.0F;
			registration.breakTorque = 100.0F;
			registration.resetGeneration = 1U;
			context.Expect(world.RegisterConstraint(registration), "A generation-complete constraint must register exactly once.");

			PhysicsConstraintImpulseSample sample;
			sample.handle = handle;
			sample.ownerObjectId = registration.ownerObjectId;
			sample.ownerObjectGeneration = registration.ownerObjectGeneration;
			sample.componentGeneration = registration.componentGeneration;
			sample.recordId = registration.recordId;
			sample.recordGeneration = registration.recordGeneration;
			sample.resetGeneration = 1U;
			sample.linearImpulseMagnitude = 1.0F;
			std::vector<PhysicsEvent> events;
			context.Expect(world.EvaluateConstraintImpulses(1U, 0.05F, {&sample, 1U}, events),
				"A synchronized solver impulse sample must be accepted at its fixed tick.");
			context.Expect(events.size() == 1U && events.front().type == PhysicsEventType::ConstraintBroken
				&& events.front().constraint == handle && events.front().constraintBreakForce == 20.0F,
				"Impulse divided by fixed dt must produce one explicit, provider-neutral break event.");
			context.Expect(!world.EvaluateConstraintImpulses(1U, 0.05F, {&sample, 1U}, events),
				"A duplicate sample tick must be rejected without partially mutating runtime state.");
			context.Expect(!world.ResetConstraint(handle, 1U) && world.ResetConstraint(handle, 2U),
				"A broken constraint must reject duplicate reset tokens and accept only a strictly newer reset.");
			sample.resetGeneration = 2U;
			events.clear();
			context.Expect(world.EvaluateConstraintImpulses(2U, 0.05F, {&sample, 1U}, events) && events.size() == 1U,
				"A newer reset must permit one later break event while retaining one-shot behavior per reset generation.");
		});
	}
}
