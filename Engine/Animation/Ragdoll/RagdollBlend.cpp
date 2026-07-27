// RagdollBlend.cpp - Deterministic full and partial ragdoll pose composition.

#include "Animation/Ragdoll/RagdollBlend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_map>

namespace TerraEngine::Animation {
	namespace {
		bool FiniteUnitWeight(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
		}

		AnimationQuaternion NormalizedShortestNlerp(
			const AnimationQuaternion& animation,
			AnimationQuaternion physics,
			float weight) noexcept {
			const float dot = animation.x * physics.x + animation.y * physics.y
				+ animation.z * physics.z + animation.w * physics.w;
			if(dot < 0.0F) {
				physics.x = -physics.x;
				physics.y = -physics.y;
				physics.z = -physics.z;
				physics.w = -physics.w;
			}
			AnimationQuaternion result{
				animation.x + (physics.x - animation.x) * weight,
				animation.y + (physics.y - animation.y) * weight,
				animation.z + (physics.z - animation.z) * weight,
				animation.w + (physics.w - animation.w) * weight};
			const float lengthSquared = result.x * result.x + result.y * result.y
				+ result.z * result.z + result.w * result.w;
			if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) return {};
			const float inverseLength = 1.0F / std::sqrt(lengthSquared);
			result.x *= inverseLength;
			result.y *= inverseLength;
			result.z *= inverseLength;
			result.w *= inverseLength;
			return result;
		}

		AnimationTransform BlendTransform(
			const AnimationTransform& animation,
			const AnimationTransform& physics,
			float weight) noexcept {
			AnimationTransform result;
			result.translation = {
				animation.translation.x + (physics.translation.x - animation.translation.x) * weight,
				animation.translation.y + (physics.translation.y - animation.translation.y) * weight,
				animation.translation.z + (physics.translation.z - animation.translation.z) * weight};
			result.rotation = NormalizedShortestNlerp(animation.rotation, physics.rotation, weight);
			result.scale = {
				animation.scale.x + (physics.scale.x - animation.scale.x) * weight,
				animation.scale.y + (physics.scale.y - animation.scale.y) * weight,
				animation.scale.z + (physics.scale.z - animation.scale.z) * weight};
			return result;
		}

		float ResolveLocalTimeWeight(const RagdollBlendInput& input) noexcept {
			if(input.durationSeconds <= 0.0F) return input.timeTargetWeight;
			const float progress = std::clamp(input.elapsedSeconds / input.durationSeconds, 0.0F, 1.0F);
			return input.timeStartWeight
				+ (input.timeTargetWeight - input.timeStartWeight) * progress;
		}
	}

	AnimationStatus BlendRagdollPose(
		std::span<const std::uint64_t> jointIds,
		std::span<const std::int32_t> parentIndices,
		std::span<const AnimationTransform> animationPose,
		const PhysicsPoseSnapshot& snapshot,
		const RagdollGenerationStamp& expected,
		std::span<const RagdollBlendInput> inputs,
		float timeWeight,
		RagdollBlendResult& output) noexcept {
		if(jointIds.empty() || jointIds.size() != animationPose.size()
			|| jointIds.size() != parentIndices.size() || !FiniteUnitWeight(timeWeight)) {
			return AnimationStatus::InvalidArgument;
		}
		if(const AnimationStatus status = ValidatePhysicsPoseSnapshot(snapshot, expected);
			status != AnimationStatus::Ok) return status;
		try {
			std::unordered_map<std::uint64_t, std::size_t> jointIndex;
			jointIndex.reserve(jointIds.size());
			for(std::size_t index = 0U; index < jointIds.size(); ++index) {
				if(jointIds[index] == 0U || !jointIndex.emplace(jointIds[index], index).second
					|| parentIndices[index] >= static_cast<std::int32_t>(index)
					|| parentIndices[index] < -1 || !IsFinite(animationPose[index])) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::unordered_map<std::uint64_t, const PhysicsPoseBodySnapshot*> physicsByJoint;
			physicsByJoint.reserve(snapshot.bodies.size());
			for(const PhysicsPoseBodySnapshot& body : snapshot.bodies) {
				if(!physicsByJoint.emplace(body.stableJointId, &body).second) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::unordered_map<std::uint64_t, RagdollBlendInput> configured;
			configured.reserve(inputs.size());
			for(const RagdollBlendInput& input : inputs) {
				if(input.stableJointId == 0U || !jointIndex.contains(input.stableJointId)
					|| !configured.emplace(input.stableJointId, input).second
					|| !FiniteUnitWeight(input.weight) || !FiniteUnitWeight(input.branchWeight)
					|| !FiniteUnitWeight(input.layerWeight) || !FiniteUnitWeight(input.maskWeight)
					|| !FiniteUnitWeight(input.timeStartWeight) || !FiniteUnitWeight(input.timeTargetWeight)
					|| !std::isfinite(input.elapsedSeconds) || input.elapsedSeconds < 0.0F
					|| !std::isfinite(input.durationSeconds) || input.durationSeconds < 0.0F
					|| !std::isfinite(input.motorStrength) || input.motorStrength < 0.0F) {
					return AnimationStatus::InvalidArgument;
				}
			}

			RagdollBlendResult staged;
			staged.modelPose.assign(animationPose.begin(), animationPose.end());
			staged.effectiveWeights.assign(jointIds.size(), 0.0F);
			staged.effectiveMotorStrengths.assign(jointIds.size(), 0.0F);
			std::vector<float> inheritedBranch(jointIds.size(), 1.0F);
			for(std::size_t index = 0U; index < jointIds.size(); ++index) {
				const std::int32_t parent = parentIndices[index];
				if(parent >= 0) inheritedBranch[index] = inheritedBranch[static_cast<std::size_t>(parent)];
				const auto rule = configured.find(jointIds[index]);
				RagdollBlendInput input;
				input.stableJointId = jointIds[index];
				if(rule != configured.end()) input = rule->second;
				if(rule != configured.end() && input.includeDescendants) {
					inheritedBranch[index] *= input.branchWeight;
				}
				const float explicitBranch = rule != configured.end() && !input.includeDescendants
					? input.branchWeight : 1.0F;
				float weight = input.weight * inheritedBranch[index] * explicitBranch
					* input.layerWeight * input.maskWeight * ResolveLocalTimeWeight(input) * timeWeight;
				weight = std::clamp(weight, 0.0F, 1.0F);
				if(input.animationDrivenKinematic
					|| (input.isBoundary && input.boundaryPolicy == RagdollBoundaryPolicy::KinematicAnchor)) {
					weight = 0.0F;
				} else if(input.isBoundary && parent >= 0
					&& input.boundaryPolicy == RagdollBoundaryPolicy::MatchAnimation) {
					// A boundary cannot become more simulated than its parent in this
					// composition pass. This monotonic rule prevents a one-joint hard seam
					// while still allowing descendants to ramp farther into simulation.
					weight = std::min(weight, staged.effectiveWeights[static_cast<std::size_t>(parent)]);
				}
				staged.effectiveWeights[index] = weight;
				if(input.boundaryPolicy == RagdollBoundaryPolicy::MotorToAnimation) {
					const float motorStrength = input.motorStrength * (1.0F - weight);
					if(!std::isfinite(motorStrength)) return AnimationStatus::InvalidArgument;
					staged.effectiveMotorStrengths[index] = motorStrength;
				}
				const auto physics = physicsByJoint.find(jointIds[index]);
				if(physics != physicsByJoint.end()) {
					staged.modelPose[index] = BlendTransform(
						animationPose[index], physics->second->modelPose, weight);
					if(!IsFinite(staged.modelPose[index])) return AnimationStatus::InvalidArgument;
				}
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus BlendRagdollPose(
		std::span<const std::uint64_t> jointIds,
		std::span<const AnimationTransform> animationPose,
		const PhysicsPoseSnapshot& physicsPose,
		const RagdollGenerationStamp& expected,
		std::span<const RagdollBlendInput> inputs,
		float timeWeight,
		RagdollBlendResult& output) noexcept {
		try {
			std::vector<std::int32_t> roots(jointIds.size(), -1);
			return BlendRagdollPose(jointIds, roots, animationPose, physicsPose,
				expected, inputs, timeWeight, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}
}
