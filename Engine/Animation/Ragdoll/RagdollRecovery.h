// RagdollRecovery.h - Deterministic recovery choice and copied geometry gate.

#pragma once

#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Ragdoll/RagdollActivation.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"

#include <cstdint>

namespace TerraEngine::Animation {
	enum class RagdollRecoveryBlockReason : std::uint8_t {
		None,
		StaleSnapshot,
		InvalidProfile,
		StaleClearance,
		SpaceBlocked,
		NoCompatibleCandidate,
		MovingSupportNotAllowed,
		PoseErrorExceeded,
		VelocityExceeded
	};

	struct RagdollRecoveryClearanceSnapshot {
		AnimationGeneration worldGeneration = 0U;
		AnimationFixedTick fixedStep = 0;
		AnimationFloat3 center{};
		float radiusMeters = 0.0F;
		float heightMeters = 0.0F;
		bool clear = false;
	};

	struct RagdollRecoveryRequest {
		std::uint64_t observedContactMask = 0U;
		AnimationFloat3 up{0.0F, 1.0F, 0.0F};
		AnimationFloat3 desiredFacing{0.0F, 0.0F, 1.0F};
		bool spaceClear = false;
		std::uint64_t requestedCandidateId = 0U;
		RagdollRecoveryPosture observedPosture = RagdollRecoveryPosture::Any;
		AnimationFloat3 rootPosition{};
		float rootLinearSpeed = 0.0F;
		float rootAngularSpeed = 0.0F;
		float poseErrorMeters = 0.0F;
		AnimationFloat3 supportVelocity{};
		AnimationGeneration supportGeneration = 0U;
		bool onMovingSupport = false;
		RagdollRecoveryClearanceSnapshot clearance{};
	};

	struct RagdollRecoveryResult {
		AnimationStatus status = AnimationStatus::NotReady;
		std::uint64_t candidateId = 0U;
		AnimationFloat3 alignedFacing{};
		AnimationFloat3 alignedRootPosition{};
		AnimationFloat3 inheritedSupportVelocity{};
		AnimationFixedTick transitionStartTick = 0;
		float transitionDurationSeconds = 0.0F;
		bool blocked = false;
		RagdollRecoveryBlockReason blockReason = RagdollRecoveryBlockReason::None;
	};

	struct RagdollRecoveryObservation {
		// This is the current complete animation pose expressed in the same copied
		// world space as the physics snapshot. No scene or provider pointer is read.
		PhysicsPoseSnapshot completeAnimationPose{};
		std::uint64_t observedContactMask = 0U;
		RagdollRecoveryPosture observedPosture = RagdollRecoveryPosture::Any;
		bool clearanceConfirmed = false;
		bool onMovingSupport = false;
		AnimationFloat3 supportVelocity{};
		AnimationGeneration supportGeneration = 0U;
	};

	TERRAENGINE_API AnimationStatus BuildRagdollRecoveryRequest(
		const RagdollProfileAsset& profile,
		const PhysicsPoseSnapshot& physicsPose,
		const RagdollGenerationStamp& expected,
		const RagdollRecoveryObservation& observation,
		RagdollRecoveryRequest& output) noexcept;

	TERRAENGINE_API RagdollRecoveryResult SelectRagdollRecovery(
		const RagdollProfileAsset& profile,
		const PhysicsPoseSnapshot& snapshot,
		const RagdollGenerationStamp& expected,
		const RagdollRecoveryRequest& request) noexcept;
}
