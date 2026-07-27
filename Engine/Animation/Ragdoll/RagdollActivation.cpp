// RagdollActivation.cpp - Converts one sealed pose into an atomic activation plan.
#include "Animation/Ragdoll/RagdollActivation.h"
#include <algorithm>
#include <cmath>
#include <new>
namespace TerraEngine::Animation {
	namespace {
		AnimationQuaternion Multiply(
			const AnimationQuaternion& left,
			const AnimationQuaternion& right) noexcept {
			return {
				left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
				left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
				left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
				left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
		}

		AnimationFloat3 Rotate(
			const AnimationQuaternion& rotation,
			const AnimationFloat3& value) noexcept {
			// q * v * conjugate(q), expanded to avoid introducing a math-library
			// dependency into the provider-neutral animation handoff.
			const AnimationFloat3 twiceCross{
				2.0F * (rotation.y * value.z - rotation.z * value.y),
				2.0F * (rotation.z * value.x - rotation.x * value.z),
				2.0F * (rotation.x * value.y - rotation.y * value.x)};
			return {
				value.x + rotation.w * twiceCross.x + rotation.y * twiceCross.z - rotation.z * twiceCross.y,
				value.y + rotation.w * twiceCross.y + rotation.z * twiceCross.x - rotation.x * twiceCross.z,
				value.z + rotation.w * twiceCross.z + rotation.x * twiceCross.y - rotation.y * twiceCross.x};
		}

		AnimationTransform MapJointToBody(
			const AnimationTransform& joint,
			const RagdollBodyRecord& body) noexcept {
			const AnimationFloat3 scaledOffset{
				body.jointToBodyOffset.x * joint.scale.x,
				body.jointToBodyOffset.y * joint.scale.y,
				body.jointToBodyOffset.z * joint.scale.z};
			const AnimationFloat3 rotatedOffset = Rotate(joint.rotation, scaledOffset);
			return {
				{joint.translation.x + rotatedOffset.x,
					joint.translation.y + rotatedOffset.y,
					joint.translation.z + rotatedOffset.z},
				Multiply(joint.rotation, body.jointToBodyOrientation),
				joint.scale};
		}

		AnimationQuaternion Conjugate(AnimationQuaternion value) noexcept {
			value.x = -value.x;
			value.y = -value.y;
			value.z = -value.z;
			return value;
		}

		bool DeriveAngularVelocity(
			const AnimationQuaternion& previous,
			const AnimationQuaternion& current,
			float inverseElapsedSeconds,
			AnimationFloat3& output) noexcept {
			AnimationQuaternion delta = Multiply(current, Conjugate(previous));
			const float lengthSquared = delta.x * delta.x + delta.y * delta.y
				+ delta.z * delta.z + delta.w * delta.w;
			if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) return false;
			const float inverseLength = 1.0F / std::sqrt(lengthSquared);
			delta.x *= inverseLength;
			delta.y *= inverseLength;
			delta.z *= inverseLength;
			delta.w *= inverseLength;
			// q and -q encode the same orientation. Select the shortest arc so an
			// animation key crossing quaternion sign does not create a velocity spike.
			if(delta.w < 0.0F) {
				delta.x = -delta.x;
				delta.y = -delta.y;
				delta.z = -delta.z;
				delta.w = -delta.w;
			}
			const float sineHalfAngle = std::sqrt(
				delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			if(sineHalfAngle <= 1.0e-6F) {
				output = {};
				return true;
			}
			const float angle = 2.0F * std::atan2(sineHalfAngle, std::clamp(delta.w, 0.0F, 1.0F));
			const float scale = angle * inverseElapsedSeconds / sineHalfAngle;
			output = {delta.x * scale, delta.y * scale, delta.z * scale};
			return IsFinite(output);
		}
	}

	AnimationStatus PrepareRagdollActivation(const PhysicsPoseSnapshot& source, const RagdollGenerationStamp& expected, RagdollActivationPlan& output) noexcept {
		if(ValidatePhysicsPoseSnapshot(source, expected) != AnimationStatus::Ok) return AnimationStatus::StaleHandle;
		try {
			RagdollActivationPlan staged;
			staged.generation = source.generation;
			staged.bodies.reserve(source.bodies.size());
			for(const PhysicsPoseBodySnapshot& body : source.bodies) {
				staged.bodies.push_back({body.bodyId, body.stableJointId, body.modelPose,
					body.linearVelocity, body.angularVelocity});
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus PrepareRagdollActivation(
		const PhysicsPoseSnapshot& source,
		const RagdollProfileAsset& profile,
		const RagdollGenerationStamp& expected,
		RagdollActivationPlan& output) noexcept {
		if(ValidatePhysicsPoseSnapshot(source, expected) != AnimationStatus::Ok) return AnimationStatus::StaleHandle;
		if(profile.runtimeGeneration != expected.profileGeneration
			|| ValidateRagdollProfileAssetStructure(profile) != AnimationStatus::Ok
			|| profile.bodies.size() != source.bodies.size()
			|| profile.bodies.size() > kRagdollSnapshotMaximumBodies) {
			return AnimationStatus::IncompatibleAsset;
		}
		try {
			RagdollActivationPlan staged;
			staged.generation = source.generation;
			staged.bodies.reserve(profile.bodies.size());
			for(const RagdollBodyRecord& profileBody : profile.bodies) {
				const auto sourceBody = std::find_if(source.bodies.begin(), source.bodies.end(),
					[&profileBody](const PhysicsPoseBodySnapshot& candidate) {
						return candidate.bodyId == profileBody.bodyId
							&& candidate.stableJointId == profileBody.stableJointId;
					});
				if(sourceBody == source.bodies.end()) return AnimationStatus::IncompatibleAsset;
				AnimationTransform worldPose = MapJointToBody(sourceBody->modelPose, profileBody);
				if(!IsFinite(worldPose)) return AnimationStatus::InvalidArgument;
				staged.bodies.push_back({profileBody.bodyId, profileBody.stableJointId,
					worldPose, sourceBody->linearVelocity, sourceBody->angularVelocity});
			}
			// Publish only after every body was mapped. A missing final joint therefore
			// cannot leak a prefix of the new activation plan to the physics boundary.
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus PrepareRagdollActivation(
		const PhysicsPoseSnapshot& source,
		const PhysicsPoseSnapshot& previous,
		float elapsedSeconds,
		const RagdollProfileAsset& profile,
		const RagdollGenerationStamp& expected,
		RagdollActivationPlan& output) noexcept {
		if(ValidatePhysicsPoseSnapshot(source, expected) != AnimationStatus::Ok) {
			return AnimationStatus::StaleHandle;
		}
		if(ValidatePhysicsPoseSnapshot(previous, previous.generation) != AnimationStatus::Ok
			|| previous.generation.worldGeneration != expected.worldGeneration
			|| previous.generation.profileGeneration != expected.profileGeneration
			|| previous.generation.skeletonGeneration != expected.skeletonGeneration
			|| previous.generation.fixedStep >= expected.fixedStep
			|| !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F) {
			return AnimationStatus::StaleHandle;
		}
		if(profile.runtimeGeneration != expected.profileGeneration
			|| ValidateRagdollProfileAssetStructure(profile) != AnimationStatus::Ok
			|| profile.bodies.size() != source.bodies.size()
			|| profile.bodies.size() != previous.bodies.size()
			|| profile.bodies.size() > kRagdollSnapshotMaximumBodies) {
			return AnimationStatus::IncompatibleAsset;
		}
		try {
			RagdollActivationPlan staged;
			staged.generation = source.generation;
			staged.bodies.reserve(profile.bodies.size());
			const float inverseElapsedSeconds = 1.0F / elapsedSeconds;
			for(const RagdollBodyRecord& profileBody : profile.bodies) {
				const auto currentJoint = std::find_if(source.bodies.begin(), source.bodies.end(),
					[&profileBody](const PhysicsPoseBodySnapshot& candidate) {
						return candidate.bodyId == profileBody.bodyId
							&& candidate.stableJointId == profileBody.stableJointId;
					});
				const auto previousJoint = std::find_if(previous.bodies.begin(), previous.bodies.end(),
					[&profileBody](const PhysicsPoseBodySnapshot& candidate) {
						return candidate.bodyId == profileBody.bodyId
							&& candidate.stableJointId == profileBody.stableJointId;
					});
				if(currentJoint == source.bodies.end() || previousJoint == previous.bodies.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const AnimationTransform worldPose = MapJointToBody(currentJoint->modelPose, profileBody);
				const AnimationTransform previousWorldPose = MapJointToBody(
					previousJoint->modelPose, profileBody);
				AnimationFloat3 linearVelocity{
					(worldPose.translation.x - previousWorldPose.translation.x) * inverseElapsedSeconds,
					(worldPose.translation.y - previousWorldPose.translation.y) * inverseElapsedSeconds,
					(worldPose.translation.z - previousWorldPose.translation.z) * inverseElapsedSeconds};
				AnimationFloat3 angularVelocity;
				if(!IsFinite(worldPose) || !IsFinite(previousWorldPose) || !IsFinite(linearVelocity)
					|| !DeriveAngularVelocity(previousWorldPose.rotation, worldPose.rotation,
						inverseElapsedSeconds, angularVelocity)) {
					return AnimationStatus::InvalidArgument;
				}
				staged.bodies.push_back({profileBody.bodyId, profileBody.stableJointId,
					worldPose, linearVelocity, angularVelocity});
			}
			// Do not replace a usable activation plan until every mapped body and both
			// velocity channels have passed validation.
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}
}
