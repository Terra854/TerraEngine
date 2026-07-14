#pragma once

#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "PhysicsSettings.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Physics {

	// These records are deliberately coarse. They are immutable input packets
	// shared by worker jobs and contain no provider pointers or provider IDs.
	struct PhysicsBodyDescriptor {
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
		PhysicsPose worldPose;
		PhysicsPose parentWorldPose;
		PhysicsVector3 parentWorldScale{1.0F, 1.0F, 1.0F};
		bool hasParent = false;
		PhysicsMotionType motionType = PhysicsMotionType::Static;
		PhysicsMotionQuality motionQuality = PhysicsMotionQuality::Discrete;
		PhysicsVector3 initialLinearVelocity;
		PhysicsVector3 initialAngularVelocity;
		float mass = 1.0F;
		float linearDamping = 0.0F;
		float angularDamping = 0.05F;
		float maximumLinearVelocity = 500.0F;
		float maximumAngularVelocity = 100.0F;
		std::uint32_t allowedDegreesOfFreedom = 0x3FU;
		std::uint32_t collisionMask = 0xFFFFFFFFU;
		bool useGravity = true;
		bool allowSleeping = true;
		bool startAwake = true;
		bool enabled = true;
		std::uint8_t collisionLayer = 0;
		// This is an engine-owned dependency revision for the resolved world pose.
		// It includes ancestor transforms, so authored parent edits cannot be hidden
		// by an unchanged component generation on a child rigid body.
		std::uint64_t sourceTransformRevision = 0;
	};

	struct PhysicsColliderDescriptor {
		TerraEngine::UUID objectId{};
		PhysicsGeneration componentGeneration = 0;
		TerraEngine::UUID recordId{};
		PhysicsGeneration recordGeneration = 0;
		PhysicsPose localPose;
		PhysicsShapeKind kind = PhysicsShapeKind::Box;
		PhysicsVector3 dimensions{1.0F, 1.0F, 1.0F};
		TerraEngine::UUID cookedColliderAssetId{};
		TerraEngine::UUID materialId{};
		std::uint64_t stableRecipeHash = 0;
		std::uint32_t subShapeKey = 0;
		std::uint8_t collisionLayer = 0;
		std::uint32_t queryFlags = 0xFU;
		bool trigger = false;
		bool enabled = true;
	};

	struct PhysicsConstraintDescriptor {
		TerraEngine::UUID ownerObjectId{};
		PhysicsGeneration ownerObjectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
		TerraEngine::UUID recordId{};
		PhysicsGeneration recordGeneration = 0;
		PhysicsConstraintKind kind = PhysicsConstraintKind::Fixed;
		PhysicsConstraintEndpointKind firstEndpointKind = PhysicsConstraintEndpointKind::ConnectedObject;
		TerraEngine::UUID firstObjectId{};
		PhysicsConstraintEndpointKind secondEndpointKind = PhysicsConstraintEndpointKind::WorldAnchor;
		TerraEngine::UUID secondObjectId{};
		PhysicsPose firstFrame;
		PhysicsPose secondFrame;
		PhysicsVector3 firstAxis{0.0F, 1.0F, 0.0F};
		PhysicsVector3 secondAxis{0.0F, 1.0F, 0.0F};
		PhysicsConstraintLimits limits;
		PhysicsConstraintSpring spring;
		PhysicsConstraintMotor motor;
		std::array<PhysicsConstraintLimits, 6U> axisLimits{};
		std::array<PhysicsConstraintSpring, 6U> axisSprings{};
		std::array<PhysicsConstraintMotor, 6U> axisMotors{};
		TerraEngine::UUID firstGearConstraintId{};
		TerraEngine::UUID secondGearConstraintId{};
		float gearRatio = 1.0F;
		float breakForce = std::numeric_limits<float>::infinity();
		float breakTorque = std::numeric_limits<float>::infinity();
		bool collideConnected = false;
		bool enabled = true;
		PhysicsGeneration resetGeneration = 1U;
	};

	struct PhysicsCharacterDescriptor {
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
		PhysicsPose worldPose;
		PhysicsCharacterShapeKind shapeKind = PhysicsCharacterShapeKind::Capsule;
		float radius = 0.5F;
		float height = 2.0F;
		PhysicsVector3 localCenter;
		PhysicsVector3 upDirection{0.0F, 1.0F, 0.0F};
		float stepHeight = 0.4F;
		float supportVolumeRadius = 0.5F;
		float supportVolumeHeight = 0.2F;
		float maximumSlopeAngleDegrees = 50.0F;
		float contactPadding = 0.02F;
		float mass = 75.0F;
		float maximumStrength = 1000.0F;
		std::uint8_t collisionLayer = 0;
		std::uint32_t collisionMask = 0xFFFFFFFFU;
		float groundAdhesion = 0.0F;
		float gravityScale = 1.0F;
		float jumpSpeed = 5.0F;
		PhysicsCharacterRotationPolicy rotationPolicy = PhysicsCharacterRotationPolicy::AlignToUp;
		bool allowStairWalking = true;
		bool useGravity = true;
		bool enabled = true;
	};

	struct PhysicsDirtyObjectGeneration {
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
		PhysicsGeneration recordGeneration = 0;
	};

	// Authoring diagnostics explain why one object was safely omitted from an
	// otherwise valid snapshot. They are provider-neutral values so editor tools,
	// automation, and tests can present an actionable fix without reading Jolt
	// state or turning a local authoring problem into a world-wide failure.
	struct PhysicsAuthoringDiagnostic {
		TerraEngine::UUID objectId{};
		std::string code;
		std::string message;
	};

	struct PhysicsAuthoringData {
		PhysicsWorldHandle world;
		PhysicsSettings settings;
		PhysicsTick tick = 0;
		PhysicsGeneration snapshotGeneration = 0;
		std::uint64_t hierarchyGeneration = 0;
		std::vector<PhysicsBodyDescriptor> bodies;
		std::vector<PhysicsColliderDescriptor> colliders;
		std::vector<PhysicsConstraintDescriptor> constraints;
		std::vector<PhysicsCharacterDescriptor> characters;
		std::vector<PhysicsDirtyObjectGeneration> dirtyObjects;
		std::vector<PhysicsAuthoringDiagnostic> diagnostics;
	};

	class PhysicsAuthoringSnapshot final {
	public:
		static std::optional<PhysicsAuthoringSnapshot> Create(PhysicsAuthoringData data);
		// Capture runs at PhysicsSnapshot's protected boundary. It resolves the
		// hierarchy once, then publishes immutable provider-neutral values to jobs.
		static std::optional<PhysicsAuthoringSnapshot> Capture(
			ObjectManager& objects,
			PhysicsWorldHandle world,
			const PhysicsSettings& settings,
			PhysicsTick tick,
			const PhysicsAuthoringSnapshot* previous = nullptr);

		const PhysicsAuthoringData& Data() const noexcept { return *data_; }
		bool IsValid() const noexcept { return data_ != nullptr; }

	private:
		explicit PhysicsAuthoringSnapshot(std::shared_ptr<const PhysicsAuthoringData> data)
			: data_(std::move(data)) {}

		std::shared_ptr<const PhysicsAuthoringData> data_;
	};

	struct PhysicsBodyState {
		PhysicsBodyHandle handle;
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
		PhysicsPose worldPose;
		PhysicsVector3 linearVelocity;
		PhysicsVector3 angularVelocity;
		bool awake = true;
	};

	struct PhysicsBodyStateSnapshot {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0;
		std::shared_ptr<const std::vector<PhysicsBodyState>> bodies;
	};

	struct PhysicsCommandResult {
		PhysicsCommandId commandId = 0;
		PhysicsResultStatus status = PhysicsResultStatus::InternalFailure;
		PhysicsTick effectiveTick = 0;
		std::string diagnostic;
	};

	struct PhysicsCommandResultSnapshot {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0;
		std::shared_ptr<const std::vector<PhysicsCommandResult>> results;
	};

}
