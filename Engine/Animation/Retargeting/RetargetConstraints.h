// RetargetConstraints.h - Immutable root, trajectory, contact, and effector pass.

#pragma once

#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	// The stamp labels every immutable input that can invalidate a worker result.
	// It contains no scene object, physics body, pointer, or renderer address.
	struct RetargetGenerationStamp {
		AnimationAssetHandle profile{};
		AnimationAssetHandle sourceSkeleton{};
		AnimationAssetHandle targetSkeleton{};
		AnimationGeneration sourcePoseGeneration = 0U;
		AnimationGeneration targetReferencePoseGeneration = 0U;
		AnimationGeneration publicationGeneration = 0U;
		AnimationFixedTick tick = 0;

		constexpr bool IsValid() const noexcept {
			return profile.IsValid() && sourceSkeleton.IsValid()
				&& targetSkeleton.IsValid() && sourcePoseGeneration != 0U
				&& targetReferencePoseGeneration != 0U
				&& publicationGeneration != 0U;
		}

		auto operator<=>(const RetargetGenerationStamp&) const = default;
	};

	struct RetargetContactTarget {
		std::uint64_t contactId = 0U;
		AnimationFloat3 positionModel{};
		AnimationFloat3 normalModel{0.0F, 1.0F, 0.0F};
		bool active = true;
		bool hasExplicitPosition = false;
		bool hasExplicitNormal = false;

		auto operator<=>(const RetargetContactTarget&) const = default;
	};

	struct RetargetEndEffectorTarget {
		std::uint64_t chainId = 0U;
		std::uint64_t targetJointId = 0U;
		AnimationFloat3 positionModel{};
		bool active = true;

		auto operator<=>(const RetargetEndEffectorTarget&) const = default;
	};

	// Producers copy contact/trajectory goals before animation workers begin.
	// Empty target arrays mean "derive from source motion" rather than query live
	// scene or physics state from this constraint pass.
	struct RetargetConstraintSnapshot {
		RetargetGenerationStamp generation{};
		AnimationFixedTick sourceTick = 0;
		float normalizedTime = 0.0F;
		std::vector<RetargetContactTarget> contacts;
		std::vector<RetargetEndEffectorTarget> endEffectors;

		auto operator<=>(const RetargetConstraintSnapshot&) const = default;
	};

	struct RetargetConstraintRequest {
		const RetargetProfileAsset* profile = nullptr;
		const SkeletonAsset* sourceSkeleton = nullptr;
		const SkeletonAsset* targetSkeleton = nullptr;
		RetargetGenerationStamp generation{};
		RetargetGenerationStamp expectedGeneration{};
		std::span<const AnimationTransform> sourceReferenceModelTransforms;
		std::span<const AnimationTransform> sourceModelTransforms;
		std::span<const AnimationTransform> targetReferenceModelTransforms;
		std::span<const AnimationTransform> inputTargetLocalTransforms;
		std::span<const AnimationTransform> inputTargetModelTransforms;
		const RetargetConstraintSnapshot* snapshot = nullptr;
	};

	struct RetargetConstraintMetrics {
		float maximumContactErrorMeters = 0.0F;
		float maximumEndEffectorErrorMeters = 0.0F;
		float facingErrorRadians = 0.0F;
		float rootDisplacementErrorMeters = 0.0F;
		std::uint32_t appliedContactCount = 0U;
		std::uint32_t appliedEndEffectorCount = 0U;
		std::uint32_t fallbackCount = 0U;
		bool timingPreserved = false;
		bool facingPreserved = false;
		bool rootDisplacementPreserved = false;
		bool contactsPreserved = false;
		bool reachableEndEffectorsPreserved = false;

		auto operator<=>(const RetargetConstraintMetrics&) const = default;
	};

	struct RetargetConstraintResult {
		AnimationStatus status = AnimationStatus::NotReady;
		RetargetGenerationStamp generation{};
		AnimationFixedTick sourceTick = 0;
		float normalizedTime = 0.0F;
		std::vector<AnimationTransform> localTransforms;
		std::vector<AnimationTransform> modelTransforms;
		RetargetConstraintMetrics metrics{};
		bool reducedFidelity = false;

		auto operator<=>(const RetargetConstraintResult&) const = default;
	};

	TERRAENGINE_API AnimationStatus ApplyRetargetConstraints(
		const RetargetConstraintRequest& request,
		RetargetConstraintResult& output) noexcept;
}
