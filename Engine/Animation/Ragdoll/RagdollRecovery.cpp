// RagdollRecovery.cpp - Pose-matched recovery with atomic copied handoff data.

#include "Animation/Ragdoll/RagdollRecovery.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		bool Normalize(AnimationFloat3 value, AnimationFloat3& output) noexcept {
			const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
			if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) return false;
			const float inverseLength = 1.0F / std::sqrt(lengthSquared);
			output = {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
			return true;
		}

		float Dot(const AnimationFloat3& left, const AnimationFloat3& right) noexcept {
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		AnimationFloat3 Rotate(
			const AnimationQuaternion& rotation,
			const AnimationFloat3& value) noexcept {
			const AnimationFloat3 twiceCross{
				2.0F * (rotation.y * value.z - rotation.z * value.y),
				2.0F * (rotation.z * value.x - rotation.x * value.z),
				2.0F * (rotation.x * value.y - rotation.y * value.x)};
			return {
				value.x + rotation.w * twiceCross.x + rotation.y * twiceCross.z - rotation.z * twiceCross.y,
				value.y + rotation.w * twiceCross.y + rotation.z * twiceCross.x - rotation.x * twiceCross.z,
				value.z + rotation.w * twiceCross.z + rotation.x * twiceCross.y - rotation.y * twiceCross.x};
		}

		float Length(const AnimationFloat3& value) noexcept {
			return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
		}

		RagdollRecoveryResult Blocked(RagdollRecoveryBlockReason reason, AnimationStatus status) noexcept {
			RagdollRecoveryResult result;
			result.status = status;
			result.blocked = true;
			result.blockReason = reason;
			return result;
		}
	}

	AnimationStatus BuildRagdollRecoveryRequest(
		const RagdollProfileAsset& profile,
		const PhysicsPoseSnapshot& physicsPose,
		const RagdollGenerationStamp& expected,
		const RagdollRecoveryObservation& observation,
		RagdollRecoveryRequest& output) noexcept {
		if(ValidatePhysicsPoseSnapshot(physicsPose, expected) != AnimationStatus::Ok) {
			return AnimationStatus::StaleHandle;
		}
		RagdollActivationPlan animationBodies;
		const AnimationStatus animationStatus = PrepareRagdollActivation(
			observation.completeAnimationPose, profile, expected, animationBodies);
		if(animationStatus != AnimationStatus::Ok) return animationStatus;
		const auto rootPhysics = std::find_if(
			physicsPose.bodies.begin(), physicsPose.bodies.end(), [&profile](const auto& value) {
				return value.bodyId == profile.rootBodyId;
			});
		const auto rootAnimation = std::find_if(
			animationBodies.bodies.begin(), animationBodies.bodies.end(), [&profile](const auto& value) {
				return value.bodyId == profile.rootBodyId;
			});
		if(rootPhysics == physicsPose.bodies.end()
			|| rootAnimation == animationBodies.bodies.end()) {
			return AnimationStatus::IncompatibleAsset;
		}
		try {
			RagdollRecoveryRequest staged;
			staged.observedContactMask = observation.observedContactMask;
			staged.observedPosture = observation.observedPosture;
			staged.rootPosition = rootPhysics->modelPose.translation;
			staged.rootLinearSpeed = Length(rootPhysics->linearVelocity);
			staged.rootAngularSpeed = Length(rootPhysics->angularVelocity);
			staged.up = Rotate(rootPhysics->modelPose.rotation, {0.0F, 1.0F, 0.0F});
			staged.desiredFacing = Rotate(rootAnimation->worldPose.rotation, {0.0F, 0.0F, 1.0F});
			if(staged.observedPosture == RagdollRecoveryPosture::Any) {
				staged.observedPosture = Dot(staged.up, profile.referenceUpAxis) >= 0.0F
					? RagdollRecoveryPosture::FaceUp : RagdollRecoveryPosture::FaceDown;
			}
			float squaredError = 0.0F;
			for(const RagdollActivationBody& animationBody : animationBodies.bodies) {
				const auto physicsBody = std::find_if(
					physicsPose.bodies.begin(), physicsPose.bodies.end(),
					[&animationBody](const auto& value) {
						return value.bodyId == animationBody.bodyId
							&& value.stableJointId == animationBody.stableJointId;
					});
				if(physicsBody == physicsPose.bodies.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const float x = physicsBody->modelPose.translation.x - animationBody.worldPose.translation.x;
				const float y = physicsBody->modelPose.translation.y - animationBody.worldPose.translation.y;
				const float z = physicsBody->modelPose.translation.z - animationBody.worldPose.translation.z;
				squaredError += x * x + y * y + z * z;
			}
			staged.poseErrorMeters = std::sqrt(
				squaredError / static_cast<float>(animationBodies.bodies.size()));
			float radius = 0.0F;
			float height = 0.0F;
			for(const RagdollRecoveryRecord& candidate : profile.recovery) {
				radius = std::max(radius, candidate.clearanceRadiusMeters);
				height = std::max(height, candidate.clearanceHeightMeters);
			}
			staged.spaceClear = observation.clearanceConfirmed;
			staged.clearance = {expected.worldGeneration, expected.fixedStep,
				staged.rootPosition, radius, height, observation.clearanceConfirmed};
			staged.onMovingSupport = observation.onMovingSupport;
			staged.supportVelocity = observation.supportVelocity;
			staged.supportGeneration = observation.supportGeneration;
			if(!std::isfinite(staged.rootLinearSpeed) || !std::isfinite(staged.rootAngularSpeed)
				|| !std::isfinite(staged.poseErrorMeters) || !IsFinite(staged.up)
				|| !IsFinite(staged.desiredFacing)) {
				return AnimationStatus::InvalidArgument;
			}
			output = staged;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	RagdollRecoveryResult SelectRagdollRecovery(
		const RagdollProfileAsset& profile,
		const PhysicsPoseSnapshot& snapshot,
		const RagdollGenerationStamp& expected,
		const RagdollRecoveryRequest& request) noexcept {
		if(ValidatePhysicsPoseSnapshot(snapshot, expected) != AnimationStatus::Ok) {
			return Blocked(RagdollRecoveryBlockReason::StaleSnapshot, AnimationStatus::StaleHandle);
		}
		if(profile.runtimeGeneration != expected.profileGeneration
			|| ValidateRagdollProfileAssetStructure(profile) != AnimationStatus::Ok
			|| profile.skeletonFingerprint == 0U || profile.referencePoseHash == 0U) {
			return Blocked(RagdollRecoveryBlockReason::InvalidProfile, AnimationStatus::IncompatibleAsset);
		}
		AnimationFloat3 normalizedUp;
		AnimationFloat3 normalizedFacing;
		if(!Normalize(request.up, normalizedUp) || !Normalize(request.desiredFacing, normalizedFacing)
			|| !std::isfinite(request.rootLinearSpeed) || request.rootLinearSpeed < 0.0F
			|| !std::isfinite(request.rootAngularSpeed) || request.rootAngularSpeed < 0.0F
			|| !std::isfinite(request.poseErrorMeters) || request.poseErrorMeters < 0.0F
			|| !IsFinite(request.rootPosition) || !IsFinite(request.supportVelocity)) {
			return Blocked(RagdollRecoveryBlockReason::NoCompatibleCandidate, AnimationStatus::InvalidArgument);
		}

		const bool hasVersionedClearance = request.clearance.worldGeneration != 0U
			|| request.clearance.fixedStep != 0;
		if(hasVersionedClearance
			&& (request.clearance.worldGeneration != expected.worldGeneration
				|| request.clearance.fixedStep != expected.fixedStep
				|| !IsFinite(request.clearance.center)
				|| !std::isfinite(request.clearance.radiusMeters)
				|| !std::isfinite(request.clearance.heightMeters))) {
			return Blocked(RagdollRecoveryBlockReason::StaleClearance, AnimationStatus::StaleHandle);
		}
		const bool clearanceSafe = hasVersionedClearance ? request.clearance.clear : request.spaceClear;
		if(!clearanceSafe) {
			return Blocked(RagdollRecoveryBlockReason::SpaceBlocked, AnimationStatus::Rejected);
		}

		const RagdollRecoveryRecord* selected = nullptr;
		float selectedScore = std::numeric_limits<float>::infinity();
		bool sawPoseFailure = false;
		bool sawVelocityFailure = false;
		for(const RagdollRecoveryRecord& candidate : profile.recovery) {
			if(request.requestedCandidateId != 0U && candidate.candidateId != request.requestedCandidateId) continue;
			if(candidate.clipSkeletonFingerprint != profile.skeletonFingerprint
				|| candidate.requiredReferencePoseHash != profile.referencePoseHash
				|| (candidate.requiredContactMask & request.observedContactMask) != candidate.requiredContactMask
				|| (candidate.posture != RagdollRecoveryPosture::Any
					&& request.observedPosture != RagdollRecoveryPosture::Any
					&& candidate.posture != request.observedPosture)) {
				continue;
			}
			AnimationFloat3 candidateFacing;
			if(!Normalize(candidate.facing, candidateFacing)
				|| Dot(normalizedUp, profile.referenceUpAxis) < candidate.minimumUprightDot) continue;
			if(request.poseErrorMeters > candidate.maximumPoseErrorMeters) {
				sawPoseFailure = true;
				continue;
			}
			if(request.rootLinearSpeed > candidate.maximumLinearSpeed
				|| request.rootAngularSpeed > candidate.maximumAngularSpeed) {
				sawVelocityFailure = true;
				continue;
			}
			if(request.onMovingSupport && (!candidate.allowMovingSupport || request.supportGeneration == 0U)) {
				continue;
			}
			if(hasVersionedClearance
				&& (request.clearance.radiusMeters + 0.0001F < candidate.clearanceRadiusMeters
					|| request.clearance.heightMeters + 0.0001F < candidate.clearanceHeightMeters)) {
				continue;
			}
			// Lower pose error is the primary match; stable candidate identity is the
			// deterministic tie-break shared by rollback, replay, and networking.
			const float score = request.poseErrorMeters;
			if(selected == nullptr || score < selectedScore
				|| (score == selectedScore && candidate.candidateId < selected->candidateId)) {
				selected = &candidate;
				selectedScore = score;
			}
		}

		if(selected == nullptr) {
			if(sawPoseFailure) {
				return Blocked(RagdollRecoveryBlockReason::PoseErrorExceeded, AnimationStatus::NotReady);
			}
			if(sawVelocityFailure) {
				return Blocked(RagdollRecoveryBlockReason::VelocityExceeded, AnimationStatus::NotReady);
			}
			return Blocked(request.onMovingSupport
				? RagdollRecoveryBlockReason::MovingSupportNotAllowed
				: RagdollRecoveryBlockReason::NoCompatibleCandidate,
				AnimationStatus::NotReady);
		}

		RagdollRecoveryResult result;
		result.status = AnimationStatus::Ok;
		result.candidateId = selected->candidateId;
		result.alignedFacing = normalizedFacing;
		result.alignedRootPosition = {
			request.rootPosition.x + selected->rootAlignmentOffset.x,
			request.rootPosition.y + selected->rootAlignmentOffset.y,
			request.rootPosition.z + selected->rootAlignmentOffset.z};
		if(request.onMovingSupport) {
			result.inheritedSupportVelocity = request.supportVelocity;
			// Predict support displacement only across the declared blend handoff.
			// Physics remains authoritative after that transition begins.
			result.alignedRootPosition.x += request.supportVelocity.x * selected->blendDurationSeconds;
			result.alignedRootPosition.y += request.supportVelocity.y * selected->blendDurationSeconds;
			result.alignedRootPosition.z += request.supportVelocity.z * selected->blendDurationSeconds;
		}
		result.transitionStartTick = expected.fixedStep;
		result.transitionDurationSeconds = selected->blendDurationSeconds;
		return result;
	}
}
