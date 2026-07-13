// PhysicsCharacterContractTests.cpp - immutable character publication checks.

#include "../TestHarness.h"

#include "Physics/PhysicsWorld.h"

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	void RegisterPhysicsCharacterContractTests(TestRegistry& registry) {
		registry.Add("Physics.CharacterWorld.PublishesBoundedImmutableGenerationSafeState", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "A valid world must exist before character state publication.");
			const PhysicsCharacterHandle handle = world.AllocateCharacter();
			PhysicsCharacterState state;
			state.handle = handle;
			state.objectId = TerraEngine::GenerateUUID();
			state.objectGeneration = 2U;
			state.componentGeneration = 4U;
			state.world = world.Handle();
			state.tick = 1U;
			state.pose.position = {1.0F, 2.0F, 3.0F};
			state.groundState = PhysicsCharacterGroundState::OnGround;
			state.supported = true;
			state.contactCount = 2U;
			context.Expect(world.PublishCharacterStates(1U, {&state, 1U}),
				"A current-world character observation must publish an immutable packet.");
			const auto packet = world.AcquireCharacterStatePacket(1U, 1U);
			context.Expect(packet != nullptr && packet->states->size() == 1U
				&& packet->states->front().handle == handle && packet->states->front().supported,
				"Readers must receive the exact grounded/support state without a provider object.");
			context.Expect(!world.PublishCharacterStates(1U, {&state, 1U}),
				"A duplicate publication tick must not replace an already visible immutable packet.");
			state.tick = 2U;
			state.world.worldGeneration += 1U;
			context.Expect(!world.PublishCharacterStates(2U, {&state, 1U}),
				"A state from a recreated world generation must be rejected before history retention.");
			context.Expect(world.Release(handle) && world.AcquireCharacterStatePacket(1U, 1U) == nullptr,
				"Releasing a character must invalidate retained packets that mention its stale handle.");
		});
	}
}
