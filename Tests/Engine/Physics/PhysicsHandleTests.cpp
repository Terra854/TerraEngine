#include "TestHarness.h"

#include "Physics/PhysicsTypes.h"

#include <cstdint>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	void RegisterPhysicsHandleTests(TestRegistry& registry) {
		registry.Add("Engine.Physics.Handles.TypedAllocatorsCarryWorldIdentity", [](TestContext& context) {
			// Each registry is typed, so a body slot can never be mistaken for a
			// constraint or character slot. The world handle is engine-owned too.
			const PhysicsWorldHandle world{PhysicsWorldId{7U}, 3U};
			PhysicsHandleAllocator<PhysicsBodyHandle> bodies;
			PhysicsHandleAllocator<PhysicsConstraintHandle> constraints;
			PhysicsHandleAllocator<PhysicsCharacterHandle> characters;
			const PhysicsBodyHandle body = bodies.Allocate(world);
			const PhysicsConstraintHandle constraint = constraints.Allocate(world);
			const PhysicsCharacterHandle character = characters.Allocate(world);

			context.Expect(bodies.Status(body) == PhysicsHandleStatus::Alive, "Allocated body handles should be alive.");
			context.Expect(constraints.Status(constraint) == PhysicsHandleStatus::Alive, "Allocated constraint handles should be alive.");
			context.Expect(characters.Status(character) == PhysicsHandleStatus::Alive, "Allocated character handles should be alive.");
			context.Expect(world.worldId == body.worldId, "Body handles must preserve their owning world ID.");
			context.ExpectEqual(world.worldGeneration, body.worldGeneration,
				"Body handles must preserve their owning world generation.");
		});

		registry.Add("Engine.Physics.Handles.InvalidDestroyedAndStaleAreDistinct", [](TestContext& context) {
			// A default value is invalid, a released value is destroyed, and a slot
			// reused with a new generation makes the old value stale.
			const PhysicsWorldHandle world{PhysicsWorldId{9U}, 1U};
			PhysicsHandleAllocator<PhysicsBodyHandle> bodies;
			const PhysicsBodyHandle neverAllocated{};
			const PhysicsBodyHandle first = bodies.Allocate(world);
			context.Expect(bodies.Status(neverAllocated) == PhysicsHandleStatus::Invalid,
				"Never-allocated handles must report Invalid.");

			bodies.Release(first);
			context.Expect(bodies.Status(first) == PhysicsHandleStatus::Destroyed,
				"Released handles must report Destroyed before their slot is reused.");

			const PhysicsBodyHandle replacement = bodies.Allocate(world);
			context.ExpectEqual(first.slotIndex, replacement.slotIndex,
				"Freed slots should be reusable to keep registries compact.");
			context.Expect(first.slotGeneration != replacement.slotGeneration,
				"Reusing a slot must advance its generation.");
			context.Expect(bodies.Status(first) == PhysicsHandleStatus::StaleSlot,
				"The old generation must be rejected as a stale slot.");
			context.Expect(bodies.Status(replacement) == PhysicsHandleStatus::Alive,
				"The replacement generation must be alive.");
		});

		registry.Add("Engine.Physics.Handles.WorldGenerationRejectsOldChildren", [](TestContext& context) {
			// Replacing a world invalidates every child handle, even when a slot index
			// is reused. This prevents commands from crossing world lifetimes.
			const PhysicsWorldHandle oldWorld{PhysicsWorldId{11U}, 4U};
			const PhysicsWorldHandle newWorld{oldWorld.worldId, 5U};
			PhysicsHandleAllocator<PhysicsConstraintHandle> constraints;
			const PhysicsConstraintHandle oldConstraint = constraints.Allocate(oldWorld);
			context.Expect(constraints.Status(oldConstraint) == PhysicsHandleStatus::Alive,
				"The original constraint should start alive.");
			context.Expect(constraints.Status(oldConstraint, newWorld) == PhysicsHandleStatus::StaleWorld,
				"A child from an older world generation must be stale in the replacement world.");
		});
	}
}
