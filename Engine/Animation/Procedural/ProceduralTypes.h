// ProceduralTypes.h - Copied, generation-safe inputs and outputs for procedural animation.
//
// Worker jobs consume these records without consulting live scene, object, or
// physics storage. Every variable-sized snapshot is copied once behind a shared
// hard limit so malformed producers cannot create unbounded animation work.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kProceduralGoalHardMaximum = static_cast<std::size_t>(
		GetAnimationRuntimeLimit(AnimationLimit::AnimatorProceduralGoals).hardMaximum);
	inline constexpr std::uint32_t kChainIkIterationHardMaximum = static_cast<std::uint32_t>(
		GetAnimationRuntimeLimit(AnimationLimit::ChainIkIterations).hardMaximum);
	inline constexpr std::uint32_t kChainIkIterationDefault = static_cast<std::uint32_t>(
		GetAnimationRuntimeLimit(AnimationLimit::ChainIkIterations).operationalDefault);
	inline constexpr std::uint32_t kInvalidProceduralJointIndex =
		std::numeric_limits<std::uint32_t>::max();
	inline constexpr float kProceduralPositionToleranceMeters = 0.01F;
	inline constexpr float kProceduralVectorEpsilon = 1.0e-6F;

	// All target values inside one goal use the same declared coordinate space.
	// A graph integration layer is responsible for converting them before a
	// model-space solver runs; the solver never reaches back into a hierarchy.
	enum class ProceduralCoordinateSpace : std::uint8_t {
		Local = 0U,
		Parent = 1U,
		Model = 2U,
		World = 3U
	};

	enum class ProceduralGoalKind : std::uint8_t {
		Position = 0U,
		Rotation = 1U,
		PositionAndRotation = 2U,
		Aim = 3U,
		Look = 4U,
		FootPlant = 5U,
		CopyTransform = 6U,
		Twist = 7U
	};

	enum class ProceduralSolverKind : std::uint8_t {
		TwoBoneIk = 0U,
		ChainIk = 1U,
		Aim = 2U,
		Look = 3U,
		JointConstraint = 4U,
		CopyTransform = 5U,
		TwistDistribution = 6U,
		WeightedTargets = 7U,
		FootPlacement = 8U,
		PelvisCompensation = 9U
	};

	// Solver state is intentionally separate from AnimationStatus. A singular or
	// unreachable goal is a finite, publishable result rather than an API error.
	enum class ProceduralSolverState : std::uint8_t {
		Solved = 0U,
		Converged = 1U,
		MaxIterations = 2U,
		Unreachable = 3U,
		Singular = 4U,
		Clamped = 5U,
		StaleInput = 6U,
		NoContribution = 7U,
		InvalidInput = 8U
	};

	// The full stamp prevents an old worker result from aliasing a reused animator,
	// skeleton, controller program, pose publication, or query publication.
	struct ProceduralGenerationStamp {
		AnimatorInstanceHandle animator;
		AnimationGeneration componentGeneration = 0U;
		AnimationGeneration skeletonGeneration = 0U;
		AnimationGeneration programGeneration = 0U;
		AnimationGeneration poseGeneration = 0U;
		AnimationGeneration publicationGeneration = 0U;
		AnimationGeneration queryGeneration = 0U;
		AnimationFixedTick tick = 0;

		constexpr bool IsValid() const noexcept {
			return animator.IsValid() && componentGeneration != 0U
				&& skeletonGeneration != 0U && programGeneration != 0U
				&& poseGeneration != 0U && publicationGeneration != 0U;
		}

		auto operator<=>(const ProceduralGenerationStamp&) const = default;
	};

	// A stable joint id is sufficient for pose-local targets. Object-backed goals
	// additionally carry the captured object lifetime generation, never a pointer.
	struct ProceduralTargetIdentity {
		UUID objectId = EMPTY_UUID;
		AnimationGeneration objectGeneration = 0U;
		std::uint64_t stableTargetId = 0U;

		constexpr bool IsValid() const noexcept {
			return stableTargetId != 0U || (objectId != EMPTY_UUID && objectGeneration != 0U);
		}

		auto operator<=>(const ProceduralTargetIdentity&) const = default;
	};

	// One goal is a complete copied producer record. priority is compared first;
	// producerOrder and goalId then provide deterministic ties across worker counts.
	struct ProceduralGoal {
		ProceduralGenerationStamp generation;
		ProceduralTargetIdentity target;
		std::uint64_t goalId = 0U;
		std::uint64_t producerOrder = 0U;
		std::int32_t priority = 0;
		std::uint32_t targetJointIndex = kInvalidProceduralJointIndex;
		ProceduralGoalKind kind = ProceduralGoalKind::Position;
		ProceduralCoordinateSpace space = ProceduralCoordinateSpace::Model;
		AnimationFloat3 position{};
		AnimationQuaternion rotation{};
		AnimationFloat3 pole{0.0F, 1.0F, 0.0F};
		float weight = 1.0F;
		AnimationFixedTick effectiveTick = 0;
	};

	// GoalSnapshot owns its vector, so solvers can safely retain a const reference
	// for one task without depending on a producer's temporary command buffer.
	struct ProceduralGoalSnapshot {
		ProceduralGenerationStamp generation;
		std::vector<ProceduralGoal> goals;
	};

	// Physics and scene-query producers copy hits into this provider-neutral form.
	// targetTransform is meaningful only when hasTargetTransform is true.
	struct ProceduralQueryHit {
		std::uint64_t queryId = 0U;
		ProceduralTargetIdentity target;
		AnimationFloat3 point{};
		AnimationFloat3 normal{0.0F, 1.0F, 0.0F};
		AnimationTransform targetTransform{};
		float distance = 0.0F;
		bool hasTargetTransform = false;
		bool blocking = true;
	};

	struct ProceduralQuerySnapshot {
		ProceduralGenerationStamp generation;
		AnimationFixedTick capturedTick = 0;
		std::vector<ProceduralQueryHit> hits;
	};

	// Positional IK uses bend limits, while rotational constraints also consume
	// swing and twist bounds. Disabled limits leave the corresponding solver free.
	struct ProceduralJointLimit {
		float minimumBendRadians = 0.0F;
		float maximumBendRadians = 3.14159265358979323846F;
		float maximumSwingRadians = 3.14159265358979323846F;
		float maximumTwistRadians = 3.14159265358979323846F;
		bool enabled = false;
	};

	// Every solver emits the same diagnostic payload. requestedGeneration records
	// what was current, while observedGeneration makes stale input inspectable.
	struct ProceduralDiagnosticRecord {
		ProceduralGenerationStamp requestedGeneration;
		ProceduralGenerationStamp observedGeneration;
		ProceduralSolverKind solver = ProceduralSolverKind::TwoBoneIk;
		ProceduralSolverState state = ProceduralSolverState::InvalidInput;
		AnimationStatus status = AnimationStatus::NotReady;
		std::uint64_t goalId = 0U;
		std::uint64_t producerOrder = 0U;
		std::int32_t priority = 0;
		float targetErrorMeters = 0.0F;
		float targetErrorRadians = 0.0F;
		float inputWeight = 0.0F;
		float finalContribution = 0.0F;
		std::uint32_t iterationCount = 0U;
		std::uint32_t iterationLimit = 0U;
		bool converged = false;
		bool clamped = false;
		bool singular = false;
		bool unreachable = false;
		bool staleInput = false;
	};

	// The header is copied into specialized results, allowing publication code to
	// validate identity and status without knowing which solver produced it.
	struct ProceduralResultHeader {
		ProceduralGenerationStamp generation;
		AnimationStatus status = AnimationStatus::NotReady;
		ProceduralSolverState state = ProceduralSolverState::InvalidInput;
	};

	TERRAENGINE_API bool IsSupportedProceduralCoordinateSpace(
		ProceduralCoordinateSpace space) noexcept;
	TERRAENGINE_API bool IsSupportedProceduralGoalKind(ProceduralGoalKind kind) noexcept;
	TERRAENGINE_API bool IsCurrentProceduralGeneration(
		const ProceduralGenerationStamp& observed,
		const ProceduralGenerationStamp& expected) noexcept;
	TERRAENGINE_API bool IsFinite(const ProceduralJointLimit& limit) noexcept;
	TERRAENGINE_API bool IsFinite(const ProceduralDiagnosticRecord& diagnostic) noexcept;

	// Both builders validate and stage the complete candidate before replacing
	// output. A rejected batch therefore leaves the last published snapshot intact.
	TERRAENGINE_API AnimationStatus BuildProceduralGoalSnapshot(
		const ProceduralGenerationStamp& expectedGeneration,
		std::span<const ProceduralGoal> goals,
		ProceduralGoalSnapshot& output) noexcept;
	TERRAENGINE_API AnimationStatus BuildProceduralQuerySnapshot(
		const ProceduralGenerationStamp& generation,
		AnimationFixedTick capturedTick,
		std::span<const ProceduralQueryHit> hits,
		ProceduralQuerySnapshot& output) noexcept;

	// Shared scalar math keeps every CPU procedural path on exactly the same
	// normalization, quaternion hemisphere, and transform-composition semantics.
	namespace ProceduralMath {
		TERRAENGINE_API AnimationFloat3 Add(AnimationFloat3 left, AnimationFloat3 right) noexcept;
		TERRAENGINE_API AnimationFloat3 Subtract(AnimationFloat3 left, AnimationFloat3 right) noexcept;
		TERRAENGINE_API AnimationFloat3 Scale(AnimationFloat3 value, float scale) noexcept;
		TERRAENGINE_API float Dot(AnimationFloat3 left, AnimationFloat3 right) noexcept;
		TERRAENGINE_API AnimationFloat3 Cross(AnimationFloat3 left, AnimationFloat3 right) noexcept;
		TERRAENGINE_API float LengthSquared(AnimationFloat3 value) noexcept;
		TERRAENGINE_API float Length(AnimationFloat3 value) noexcept;
		TERRAENGINE_API bool TryNormalize(AnimationFloat3 value, AnimationFloat3& output) noexcept;
		TERRAENGINE_API AnimationFloat3 NormalizeOr(
			AnimationFloat3 value,
			AnimationFloat3 fallback) noexcept;
		TERRAENGINE_API AnimationFloat3 Lerp(
			AnimationFloat3 from,
			AnimationFloat3 to,
			float weight) noexcept;
		TERRAENGINE_API AnimationFloat3 DeterministicPerpendicular(AnimationFloat3 axis) noexcept;

		TERRAENGINE_API float Dot(AnimationQuaternion left, AnimationQuaternion right) noexcept;
		TERRAENGINE_API bool TryNormalize(
			AnimationQuaternion value,
			AnimationQuaternion& output) noexcept;
		TERRAENGINE_API AnimationQuaternion Canonicalize(AnimationQuaternion value) noexcept;
		TERRAENGINE_API AnimationQuaternion Conjugate(AnimationQuaternion value) noexcept;
		TERRAENGINE_API AnimationQuaternion Multiply(
			AnimationQuaternion left,
			AnimationQuaternion right) noexcept;
		TERRAENGINE_API AnimationQuaternion AxisAngle(
			AnimationFloat3 axis,
			float radians) noexcept;
		TERRAENGINE_API AnimationQuaternion FromTo(
			AnimationFloat3 from,
			AnimationFloat3 to) noexcept;
		TERRAENGINE_API AnimationQuaternion Nlerp(
			AnimationQuaternion from,
			AnimationQuaternion to,
			float weight) noexcept;
		TERRAENGINE_API AnimationFloat3 Rotate(
			AnimationQuaternion rotation,
			AnimationFloat3 value) noexcept;
		TERRAENGINE_API AnimationFloat3 SlerpDirection(
			AnimationFloat3 from,
			AnimationFloat3 to,
			float weight) noexcept;

		TERRAENGINE_API AnimationTransform Combine(
			const AnimationTransform& parent,
			const AnimationTransform& local) noexcept;
		TERRAENGINE_API bool TryRelativeTransform(
			const AnimationTransform& parent,
			const AnimationTransform& world,
			AnimationTransform& output) noexcept;
		TERRAENGINE_API AnimationTransform BlendTransform(
			const AnimationTransform& from,
			const AnimationTransform& to,
			float translationWeight,
			float rotationWeight,
			float scaleWeight) noexcept;
	}
}
