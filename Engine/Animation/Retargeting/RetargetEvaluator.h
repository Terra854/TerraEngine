// RetargetEvaluator.h - Deterministic CPU reference evaluation for rig transfer.

#pragma once

#include "Animation/Retargeting/RetargetConstraints.h"
#include "Animation/Retargeting/RetargetValidator.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace TerraEngine::Animation {
	// A pose snapshot owns its local/model arrays. Copying the shared pointer into
	// a worker packet therefore cannot expose mutable animator or scene storage.
	struct RetargetPoseSnapshot {
		AnimationAssetHandle skeleton{};
		AnimationGeneration poseGeneration = 0U;
		AnimationFixedTick tick = 0;
		std::vector<AnimationTransform> localTransforms;
		std::vector<AnimationTransform> modelTransforms;

		auto operator<=>(const RetargetPoseSnapshot&) const = default;
	};

	struct RetargetEvaluationRequest {
		std::shared_ptr<const RetargetCompiledProfile> profile;
		std::shared_ptr<const SkeletonAsset> sourceSkeleton;
		std::shared_ptr<const SkeletonAsset> targetSkeleton;
		std::shared_ptr<const RetargetPoseSnapshot> sourcePose;
		std::shared_ptr<const RetargetPoseSnapshot> targetReferencePose;
		std::shared_ptr<const RetargetConstraintSnapshot> constraints;
		RetargetGenerationStamp generation{};
		RetargetGenerationStamp expectedGeneration{};
		float normalizedTime = 0.0F;
	};

	struct RetargetEvaluationMetrics {
		std::uint32_t evaluatedMappingCount = 0U;
		std::uint32_t twistJointCount = 0U;
		std::uint32_t fallbackCount = 0U;
		float appliedTranslationScale = 1.0F;
		RetargetConstraintMetrics constraints{};

		auto operator<=>(const RetargetEvaluationMetrics&) const = default;
	};

	struct RetargetEvaluationResult {
		AnimationStatus status = AnimationStatus::NotReady;
		RetargetGenerationStamp generation{};
		AnimationFixedTick sourceTick = 0;
		float normalizedTime = 0.0F;
		std::vector<AnimationTransform> localTransforms;
		std::vector<AnimationTransform> modelTransforms;
		RetargetEvaluationMetrics metrics{};
		std::uint64_t deterministicHash = 0U;
		bool reducedFidelity = false;
		bool finite = false;

		auto operator<=>(const RetargetEvaluationResult&) const = default;
	};

	TERRAENGINE_API AnimationStatus EvaluateRetargetPose(
		const RetargetEvaluationRequest& request,
		RetargetEvaluationResult& output) noexcept;
}
