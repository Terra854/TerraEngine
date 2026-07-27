// RagdollProfileAsset.h - Durable, provider-neutral ragdoll authoring data.
//
// The asset describes physics intent without exposing Jolt types. A provider can
// therefore build its own immutable bodies and constraints from one checked,
// versioned record, while animation and editor code retain stable joint IDs.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kRagdollProfileAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kRagdollProfilePayloadMagic =
		MakeAnimationFourCC('R', 'A', 'G', 'D');
	inline constexpr std::size_t kRagdollProfileMaximumBodies = 4096U;
	inline constexpr std::size_t kRagdollProfileMaximumConstraints = 8192U;
	inline constexpr std::size_t kRagdollProfileMaximumColliders = 8192U;
	inline constexpr std::size_t kRagdollProfileMaximumConvexHullVerticesPerCollider = 256U;
	inline constexpr std::size_t kRagdollProfileMaximumTotalConvexHullVertices = 65536U;
	inline constexpr std::size_t kRagdollProfileMaximumBlendRecords = 4096U;
	inline constexpr std::size_t kRagdollProfileMaximumRecoveryRecords = 256U;
	inline constexpr std::size_t kRagdollProfileMaximumRecoveryContacts = 4096U;
	inline constexpr std::size_t kRagdollProfileMaximumPayloadBytes = 64U * 1024U * 1024U;

	// Persisted enum values are append-only. Provider-specific shape objects never
	// enter this asset; a collider either carries a primitive/point cloud or names
	// an immutable cooked shape asset through the common dependency envelope.
	enum class RagdollColliderShape : std::uint8_t {
		Sphere = 0U,
		Capsule = 1U,
		Box = 2U,
		ConvexHull = 3U
	};

	enum class RagdollConvexHullSource : std::uint8_t {
		None = 0U,
		InlineVertices = 1U,
		ImmutableCookedAsset = 2U
	};

	enum class RagdollMotionType : std::uint8_t {
		Dynamic = 0U,
		Kinematic = 1U,
		AnimationDriven = 2U
	};

	enum class RagdollInertiaMode : std::uint8_t {
		ProviderComputed = 0U,
		ExplicitDiagonal = 1U
	};

	enum class RagdollConstraintKind : std::uint8_t {
		Fixed = 0U,
		Hinge = 1U,
		Cone = 2U,
		SwingTwist = 3U,
		SixDof = 4U
	};

	enum class RagdollConstraintAxis : std::uint8_t {
		LinearX = 0U,
		LinearY = 1U,
		LinearZ = 2U,
		AngularX = 3U,
		AngularY = 4U,
		AngularZ = 5U,
		Count = 6U
	};

	enum class RagdollLimitMode : std::uint8_t {
		Free = 0U,
		Limited = 1U,
		Locked = 2U
	};

	enum class RagdollMotorMode : std::uint8_t {
		Disabled = 0U,
		Position = 1U,
		Velocity = 2U
	};

	// These values are already consumed by the blend path. Their numeric identity
	// remains unchanged while the asset adds richer boundary timing metadata.
	enum class RagdollBoundaryPolicy : std::uint8_t {
		MatchAnimation = 0U,
		KinematicAnchor = 1U,
		MotorToAnimation = 2U
	};

	enum class RagdollCoveragePolicy : std::uint8_t {
		MappedBodiesOnly = 0U,
		RequiredJointSet = 1U,
		AllDeformingJoints = 2U
	};

	enum class RagdollRecoveryPosture : std::uint8_t {
		Any = 0U,
		FaceUp = 1U,
		FaceDown = 2U,
		LeftSide = 3U,
		RightSide = 4U
	};

	struct RagdollImmutableShapeReference {
		AnimationGuid assetId{};
		AnimationGeneration expectedGeneration = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;

		auto operator<=>(const RagdollImmutableShapeReference&) const = default;
	};

	struct RagdollColliderRecord {
		std::uint64_t colliderId = 0U;
		RagdollColliderShape shape = RagdollColliderShape::Capsule;
		RagdollConvexHullSource convexHullSource = RagdollConvexHullSource::None;
		AnimationFloat3 localCenter{};
		AnimationQuaternion localOrientation{};
		// Sphere uses x as radius, capsule uses x as radius and y as cylinder
		// half-height, and box uses xyz as half-extents.
		AnimationFloat3 dimensions{0.1F, 0.1F, 0.1F};
		std::vector<AnimationFloat3> convexHullVertices;
		RagdollImmutableShapeReference immutableShape{};
		float friction = 0.5F;
		float restitution = 0.0F;
		bool isSensor = false;

		auto operator<=>(const RagdollColliderRecord&) const = default;
	};

	// bodyId and stableJointId intentionally remain the first fields so the small
	// aggregate initializers used by early runtime fixtures remain source-compatible.
	struct RagdollBodyRecord {
		std::uint64_t bodyId = 0U;
		std::uint64_t stableJointId = 0U;
		AnimationFloat3 jointToBodyOffset{};
		AnimationQuaternion jointToBodyOrientation{};
		std::vector<RagdollColliderRecord> colliders;
		float massKilograms = 1.0F;
		RagdollInertiaMode inertiaMode = RagdollInertiaMode::ProviderComputed;
		AnimationFloat3 centerOfMassOffset{};
		AnimationFloat3 inertiaDiagonal{1.0F, 1.0F, 1.0F};
		RagdollMotionType motionType = RagdollMotionType::Dynamic;
		float linearDamping = 0.05F;
		float angularDamping = 0.05F;
		float gravityFactor = 1.0F;
		float maximumLinearVelocity = 100.0F;
		float maximumAngularVelocity = 100.0F;
		std::uint32_t collisionLayer = 0U;
		std::uint32_t collisionMask = 0xffffffffU;
		std::uint32_t selfCollisionGroup = 0U;
		bool allowSleep = true;
		bool startAwake = true;

		auto operator<=>(const RagdollBodyRecord&) const = default;
	};

	struct RagdollConstraintFrame {
		AnimationFloat3 localPosition{};
		AnimationQuaternion localOrientation{};
		AnimationFloat3 primaryAxis{1.0F, 0.0F, 0.0F};
		AnimationFloat3 secondaryAxis{0.0F, 1.0F, 0.0F};

		auto operator<=>(const RagdollConstraintFrame&) const = default;
	};

	struct RagdollAxisLimit {
		RagdollLimitMode mode = RagdollLimitMode::Free;
		float minimum = 0.0F;
		float maximum = 0.0F;
		float restitution = 0.0F;
		float springFrequencyHertz = 0.0F;
		float springDampingRatio = 1.0F;

		auto operator<=>(const RagdollAxisLimit&) const = default;
	};

	struct RagdollAxisMotor {
		RagdollMotorMode mode = RagdollMotorMode::Disabled;
		float targetPosition = 0.0F;
		float targetVelocity = 0.0F;
		float stiffness = 0.0F;
		float damping = 0.0F;
		float maximumForce = 0.0F;

		auto operator<=>(const RagdollAxisMotor&) const = default;
	};

	struct RagdollConstraintRecord {
		std::uint64_t constraintId = 0U;
		std::uint64_t parentBodyId = 0U;
		std::uint64_t childBodyId = 0U;
		RagdollConstraintKind kind = RagdollConstraintKind::SwingTwist;
		RagdollConstraintFrame parentFrame{};
		RagdollConstraintFrame childFrame{};
		std::array<RagdollAxisLimit,
			static_cast<std::size_t>(RagdollConstraintAxis::Count)> limits{};
		std::array<RagdollAxisMotor,
			static_cast<std::size_t>(RagdollConstraintAxis::Count)> motors{};
		float breakForce = 0.0F;
		float breakTorque = 0.0F;
		bool disableCollisionBetweenBodies = true;

		auto operator<=>(const RagdollConstraintRecord&) const = default;
	};

	struct RagdollBlendRecord {
		std::uint64_t stableJointId = 0U;
		float defaultWeight = 1.0F;
		RagdollBoundaryPolicy boundaryPolicy = RagdollBoundaryPolicy::MatchAnimation;
		float activationBlendSeconds = 0.1F;
		float recoveryBlendSeconds = 0.25F;
		float motorStrengthScale = 1.0F;
		bool includeDescendants = false;

		auto operator<=>(const RagdollBlendRecord&) const = default;
	};

	struct RagdollRecoveryRecord {
		std::uint64_t candidateId = 0U;
		AnimationGuid clipAssetId{};
		std::uint64_t requiredContactMask = 0U;
		AnimationFloat3 facing{0.0F, 0.0F, 1.0F};
		float minimumUprightDot = 0.5F;
		AnimationGeneration clipGeneration = 0U;
		AnimationCompatibilityFingerprint clipSkeletonFingerprint = 0U;
		std::uint64_t requiredReferencePoseHash = 0U;
		std::uint64_t rootJointStableId = 0U;
		std::vector<std::uint64_t> contactJointStableIds;
		RagdollRecoveryPosture posture = RagdollRecoveryPosture::Any;
		AnimationFloat3 rootAlignmentOffset{};
		AnimationQuaternion rootAlignmentOrientation{};
		float maximumPoseErrorMeters = 0.25F;
		float maximumLinearSpeed = 2.0F;
		float maximumAngularSpeed = 4.0F;
		float blendDurationSeconds = 0.35F;
		float clearanceRadiusMeters = 0.3F;
		float clearanceHeightMeters = 1.8F;
		bool allowMovingSupport = true;

		auto operator<=>(const RagdollRecoveryRecord&) const = default;
	};

	struct RagdollProfileAsset {
		AnimationGuid assetId{};
		AnimationGeneration runtimeGeneration = 0U;
		AnimationGuid skeletonAssetId{};
		AnimationCompatibilityFingerprint skeletonFingerprint = 0U;
		std::uint64_t referencePoseHash = 0U;
		std::uint64_t physicsProviderFingerprint = 0U;
		std::uint64_t rootBodyId = 0U;
		RagdollCoveragePolicy coveragePolicy = RagdollCoveragePolicy::MappedBodiesOnly;
		std::vector<std::uint64_t> requiredBodyJointStableIds;
		AnimationFloat3 referenceUpAxis{0.0F, 1.0F, 0.0F};
		AnimationFloat3 referenceForwardAxis{0.0F, 0.0F, 1.0F};
		float expectedTotalMassKilograms = 0.0F;
		float expectedMassToleranceFraction = 0.1F;
		float minimumBodyMassFraction = 0.001F;
		float maximumBodyMassFraction = 0.8F;
		std::uint32_t allowedCollisionLayerMask = 0xffffffffU;
		bool allowDisconnectedBodyIslands = false;
		bool requireSymmetricCollisionFilters = true;
		std::vector<RagdollBodyRecord> bodies;
		std::vector<RagdollConstraintRecord> constraints;
		std::vector<RagdollBlendRecord> blends;
		std::vector<RagdollRecoveryRecord> recovery;
		std::uint16_t formatVersion = kRagdollProfileAssetCurrentVersion;

		auto operator<=>(const RagdollProfileAsset&) const = default;
	};

	// Structural validation is provider- and skeleton-independent. The full
	// validator additionally compares mappings with an actual SkeletonAsset.
	TERRAENGINE_API AnimationStatus ValidateRagdollProfileAssetStructure(
		const RagdollProfileAsset& asset) noexcept;
	TERRAENGINE_API AnimationStatus SerializeRagdollProfileAsset(
		const RagdollProfileAsset& asset,
		std::vector<std::uint8_t>& output) noexcept;
	TERRAENGINE_API AnimationStatus DeserializeRagdollProfileAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedSkeletonFingerprint,
		RagdollProfileAsset& output) noexcept;
}
