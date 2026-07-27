// AnimationConstraints.h - Reusable copied-input procedural pose constraints.

#pragma once

#include "Animation/Procedural/ProceduralTypes.h"

#include <span>
#include <vector>

namespace TerraEngine::Animation {
	struct WeightedProceduralTargetResult {
		ProceduralResultHeader header;
		AnimationTransform transform{};
		float finalContribution = 0.0F;
		std::vector<ProceduralDiagnosticRecord> diagnostics;
	};

	// Resolves only the highest active priority; equal-priority targets are
	// normalized by weight in stable snapshot order. Lower priorities remain in
	// diagnostics with zero final contribution so conflicts are visible.
	TERRAENGINE_API AnimationStatus BlendProceduralTargets(
		const ProceduralGoalSnapshot& snapshot,
		const ProceduralGenerationStamp& expectedGeneration,
		const AnimationTransform& fallback,
		WeightedProceduralTargetResult& output) noexcept;

	struct AimConstraintRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		const ProceduralGoalSnapshot* goalSnapshot = nullptr;
		AnimationTransform transform{};
		AnimationFloat3 localAimAxis{0.0F, 0.0F, 1.0F};
		AnimationFloat3 localUpAxis{0.0F, 1.0F, 0.0F};
		AnimationFloat3 worldUp{0.0F, 1.0F, 0.0F};
		float maximumAngleRadians = 3.14159265358979323846F;
		float dampingRate = 0.0F;
		float deltaSeconds = 0.0F;
		float weight = 1.0F;
	};

	struct LookConstraintRequest {
		AimConstraintRequest aim;
		float maximumYawRadians = 3.14159265358979323846F;
		float maximumPitchRadians = 1.57079632679489661923F;
	};

	struct TransformConstraintResult {
		ProceduralResultHeader header;
		AnimationTransform transform{};
		ProceduralDiagnosticRecord diagnostic;
	};

	TERRAENGINE_API AnimationStatus SolveAimConstraint(
		const AimConstraintRequest& request,
		TransformConstraintResult& output) noexcept;
	TERRAENGINE_API AnimationStatus SolveLookConstraint(
		const LookConstraintRequest& request,
		TransformConstraintResult& output) noexcept;

	struct CopyTransformConstraintRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		AnimationTransform currentLocalTransform{};
		AnimationTransform targetTransform{};
		AnimationTransform parentWorldTransform{};
		ProceduralCoordinateSpace targetSpace = ProceduralCoordinateSpace::Parent;
		float translationWeight = 1.0F;
		float rotationWeight = 1.0F;
		float scaleWeight = 1.0F;
		std::uint64_t goalId = 0U;
	};

	TERRAENGINE_API AnimationStatus ApplyCopyTransformConstraint(
		const CopyTransformConstraintRequest& request,
		TransformConstraintResult& output) noexcept;

	struct JointRotationConstraintRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		AnimationQuaternion referenceRotation{};
		AnimationQuaternion candidateRotation{};
		AnimationFloat3 localTwistAxis{1.0F, 0.0F, 0.0F};
		ProceduralJointLimit limit{};
		float weight = 1.0F;
		std::uint64_t goalId = 0U;
	};

	struct JointRotationConstraintResult {
		ProceduralResultHeader header;
		AnimationQuaternion rotation{};
		float swingRadians = 0.0F;
		float twistRadians = 0.0F;
		ProceduralDiagnosticRecord diagnostic;
	};

	TERRAENGINE_API AnimationStatus ApplyJointRotationConstraint(
		const JointRotationConstraintRequest& request,
		JointRotationConstraintResult& output) noexcept;

	struct TwistDistributionRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		std::span<const AnimationTransform> transforms;
		std::span<const float> distributionWeights;
		AnimationFloat3 localTwistAxis{1.0F, 0.0F, 0.0F};
		float twistRadians = 0.0F;
		float weight = 1.0F;
		std::uint64_t goalId = 0U;
	};

	struct TwistDistributionResult {
		ProceduralResultHeader header;
		std::vector<AnimationTransform> transforms;
		std::vector<ProceduralDiagnosticRecord> diagnostics;
	};

	TERRAENGINE_API AnimationStatus ApplyTwistDistribution(
		const TwistDistributionRequest& request,
		TwistDistributionResult& output) noexcept;
}
