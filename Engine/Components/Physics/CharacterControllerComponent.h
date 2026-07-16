// CharacterControllerComponent.h - backend-neutral character authoring data.
//
// This component stores only durable configuration. Desired movement, jumping,
// and resizing use typed deferred PhysicsCommands, while grounding and contact
// state are immutable runtime publications owned by PhysicsWorld.

#pragma once

#include "Core/Object/Component.h"
#include "Physics/PhysicsTypes.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <glm/vec3.hpp>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kCharacterControllerComponentSchemaVersion = 1U;

	enum class PhysicsCharacterShapeKind : std::uint8_t
	{
		Capsule,
		Cylinder
	};

	// Rotation policy determines authoring intent; provider orientation and
	// support normals are never stored in the durable scene component.
	enum class PhysicsCharacterRotationPolicy : std::uint8_t
	{
		Free,
		AlignToUp,
		Locked
	};

	enum class CharacterControllerValidationCode : std::uint8_t
	{
		None,
		UnsupportedSchema,
		InvalidShape,
		InvalidDimensions,
		InvalidCenter,
		InvalidUpDirection,
		InvalidTraversal,
		InvalidInteraction,
		InvalidFilter,
		InvalidGravityOrJump,
		InvalidRotationPolicy
	};

	struct CharacterControllerValidationResult
	{
		bool accepted = false;
		CharacterControllerValidationCode code = CharacterControllerValidationCode::None;
		std::string message;
	};
}

struct CharacterControllerComponent final : public Component<CharacterControllerComponent>
{
	static constexpr std::string_view kComponentName = "CharacterControllerComponent";

	std::uint32_t schemaVersion = TerraEngine::Physics::kCharacterControllerComponentSchemaVersion;
	TerraEngine::Physics::PhysicsCharacterShapeKind shapeKind = TerraEngine::Physics::PhysicsCharacterShapeKind::Capsule;
	float radius = 0.5F;
	float height = 2.0F;
	TerraEngine::Physics::PhysicsVector3 localCenter{};
	TerraEngine::Physics::PhysicsVector3 upDirection{0.0F, 1.0F, 0.0F};
	float stepHeight = 0.4F;
	float supportVolumeRadius = 0.5F;
	float supportVolumeHeight = 0.2F;
	float maximumSlopeAngleDegrees = 50.0F;
	float contactPadding = 0.02F;
	float mass = 75.0F;
	float maximumStrength = 1000.0F;
	std::uint8_t collisionLayerId = 0U;
	std::uint32_t collisionMask = 0xFFFFFFFFU;
	float groundAdhesion = 0.0F;
	bool allowStairWalking = true;
	bool useGravity = true;
	float gravityScale = 1.0F;
	float jumpSpeed = 5.0F;
	TerraEngine::Physics::PhysicsCharacterRotationPolicy rotationPolicy = TerraEngine::Physics::PhysicsCharacterRotationPolicy::AlignToUp;
	bool enabled = true;

	// These constants make the authoring/runtime split discoverable to tools and
	// tests without retaining transient commands or published contact state here.
	static constexpr bool kDesiredVelocityIsDeferredCommand = true;
	static constexpr bool kJumpIsDeferredCommand = true;
	static constexpr bool kGroundedStateIsRuntimePublished = true;

	// Pure validation lets snapshot preparation reject bad authoring data before
	// a protected world rebuild or any provider allocation occurs.
	[[nodiscard]] TerraEngine::Physics::CharacterControllerValidationResult Validate() const;

	// Convert compact physics authoring types to the fixed-width values supported
	// by the managed reflection bridge. Validation still occurs when PhysicsSystem
	// prepares the immutable authoring snapshot.
	[[nodiscard]] std::uint32_t GetShapeKindValue() const { return static_cast<std::uint32_t>(shapeKind); }
	void SetShapeKindValue(std::uint32_t value) { shapeKind = static_cast<TerraEngine::Physics::PhysicsCharacterShapeKind>(value); }
	[[nodiscard]] glm::vec3 GetLocalCenterValue() const { return { localCenter.x, localCenter.y, localCenter.z }; }
	void SetLocalCenterValue(const glm::vec3& value) { localCenter = { value.x, value.y, value.z }; }
	[[nodiscard]] glm::vec3 GetUpDirectionValue() const { return { upDirection.x, upDirection.y, upDirection.z }; }
	void SetUpDirectionValue(const glm::vec3& value) { upDirection = { value.x, value.y, value.z }; }
	[[nodiscard]] std::uint32_t GetCollisionLayerValue() const { return collisionLayerId; }
	void SetCollisionLayerValue(std::uint32_t value) { collisionLayerId = static_cast<std::uint8_t>(value > 0xFFU ? 0xFFU : value); }
	[[nodiscard]] std::uint32_t GetRotationPolicyValue() const { return static_cast<std::uint32_t>(rotationPolicy); }
	void SetRotationPolicyValue(std::uint32_t value) { rotationPolicy = static_cast<TerraEngine::Physics::PhysicsCharacterRotationPolicy>(value); }

	static void reflect(entt::meta_factory<CharacterControllerComponent>& factory);
};
