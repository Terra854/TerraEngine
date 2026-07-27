// RagdollBlend.h - CPU reference blending from copied physics snapshots.

#pragma once

#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	struct RagdollBlendInput {
		std::uint64_t stableJointId = 0U;
		float weight = 1.0F;
		bool isBoundary = false;
		RagdollBoundaryPolicy boundaryPolicy = RagdollBoundaryPolicy::MatchAnimation;
		// These independent factors preserve the authored composition order:
		// per-joint -> inherited branch -> layer -> mask -> local time ramp ->
		// caller-wide activation/recovery time. Each defaults to a neutral factor.
		float branchWeight = 1.0F;
		float layerWeight = 1.0F;
		float maskWeight = 1.0F;
		float timeStartWeight = 1.0F;
		float timeTargetWeight = 1.0F;
		float elapsedSeconds = 0.0F;
		float durationSeconds = 0.0F;
		float motorStrength = 0.0F;
		bool includeDescendants = false;
		bool animationDrivenKinematic = false;
	};

	struct RagdollBlendResult {
		std::vector<AnimationTransform> modelPose;
		std::vector<float> effectiveWeights;
		// Motor weights are copied outputs for the pre-physics drive packet. The
		// visual blend never changes Jolt authority or writes provider-owned state.
		std::vector<float> effectiveMotorStrengths;
	};

	// parentIndices uses -1 for a root. The hierarchy overload implements branch
	// inheritance and animated/simulated boundary continuity. The compatibility
	// overload treats every joint as an independent root.
	TERRAENGINE_API AnimationStatus BlendRagdollPose(
		std::span<const std::uint64_t> jointIds,
		std::span<const std::int32_t> parentIndices,
		std::span<const AnimationTransform> animationPose,
		const PhysicsPoseSnapshot& physicsPose,
		const RagdollGenerationStamp& expected,
		std::span<const RagdollBlendInput> inputs,
		float timeWeight,
		RagdollBlendResult& output) noexcept;

	TERRAENGINE_API AnimationStatus BlendRagdollPose(
		std::span<const std::uint64_t> jointIds,
		std::span<const AnimationTransform> animationPose,
		const PhysicsPoseSnapshot& physicsPose,
		const RagdollGenerationStamp& expected,
		std::span<const RagdollBlendInput> inputs,
		float timeWeight,
		RagdollBlendResult& output) noexcept;
}
