// Contract tests for durable character-controller authoring.
// Desired velocity and jump are commands, while grounded contacts are published
// runtime state; this component intentionally contains neither transient value.

#include "../TestHarness.h"

#include "Components/Physics/CharacterControllerComponent.h"

#include <type_traits>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Physics;

	void RegisterPhysicsCharacterComponentTests(TestRegistry& registry)
	{
		registry.Add("Physics.CharacterComponent.UsesCRTPStableReflectionAndAuthoringDefaults", [](TestContext& context) {
			context.Expect((std::is_base_of_v<Component<CharacterControllerComponent>, CharacterControllerComponent>),
				"CharacterControllerComponent must derive from Component<CharacterControllerComponent>.");
			context.ExpectEqual(std::string_view("CharacterControllerComponent"), CharacterControllerComponent::kComponentName,
				"Character component name is a provider-neutral authored contract.");
			CharacterControllerComponent::registerType();
			context.Expect(ComponentAccessRegistry().contains("CharacterControllerComponent"), "Character component must self-register for reflection access.");

			CharacterControllerComponent character;
			context.Expect(character.shapeKind == PhysicsCharacterShapeKind::Capsule, "Characters default to a capsule shape.");
			context.Expect(character.useGravity && character.jumpSpeed > 0.0F, "Characters default to gravity and a positive jump speed.");
			context.Expect(character.allowStairWalking, "Characters default to stair traversal support.");
			context.Expect(character.Validate().accepted, "Default character authoring must be valid.");
			const CharacterControllerComponent serializerFriendlyCopy = character;
			context.Expect(serializerFriendlyCopy.shapeKind == character.shapeKind
				&& serializerFriendlyCopy.jumpSpeed == character.jumpSpeed
				&& serializerFriendlyCopy.rotationPolicy == character.rotationPolicy,
				"Copying character authoring must preserve durable configuration without requiring runtime state.");
		});

		registry.Add("Physics.CharacterComponent.ValidatesShapeTraversalAndCommandStateBoundary", [](TestContext& context) {
			CharacterControllerComponent character;
			character.height = character.radius;
			context.Expect(character.Validate().code == CharacterControllerValidationCode::InvalidDimensions,
				"Character height must hold the selected radius.");
			character.height = 2.0F;
			character.maximumSlopeAngleDegrees = 90.0F;
			context.Expect(character.Validate().code == CharacterControllerValidationCode::InvalidTraversal,
				"Character slopes at or above 90 degrees must be rejected.");
			character.maximumSlopeAngleDegrees = 50.0F;
			character.upDirection = {};
			context.Expect(character.Validate().code == CharacterControllerValidationCode::InvalidUpDirection,
				"Character up direction must be normalizable.");

			context.Expect(CharacterControllerComponent::kDesiredVelocityIsDeferredCommand
				&& CharacterControllerComponent::kJumpIsDeferredCommand,
				"Desired velocity and jumping must remain deferred physics commands.");
			context.Expect(CharacterControllerComponent::kGroundedStateIsRuntimePublished,
				"Grounding and support contact state must remain runtime-published, not serialized authoring data.");
		});
	}
}
