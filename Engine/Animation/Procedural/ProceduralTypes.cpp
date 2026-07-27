// ProceduralTypes.cpp - Validation, bounded copying, and shared procedural math.

#include "Animation/Procedural/ProceduralTypes.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <tuple>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		constexpr double kLengthSquaredEpsilon = 1.0e-20;
		constexpr float kPi = 3.14159265358979323846F;

		bool ValidGoal(const ProceduralGoal& goal) noexcept {
			return goal.generation.IsValid() && goal.target.IsValid() && goal.goalId != 0U
				&& IsSupportedProceduralGoalKind(goal.kind)
				&& IsSupportedProceduralCoordinateSpace(goal.space)
				&& IsFinite(goal.position) && IsFinite(goal.rotation) && IsFinite(goal.pole)
				&& IsNormalized(goal.rotation, 0.01F) && std::isfinite(goal.weight)
				&& goal.weight >= 0.0F && goal.weight <= 1.0F;
		}

		bool GoalLess(const ProceduralGoal& left, const ProceduralGoal& right) noexcept {
			// Higher priority wins. Every remaining key is ascending so producer
			// discovery order never changes a floating-point accumulation order.
			return std::tie(right.priority, left.producerOrder, left.goalId,
				left.target.stableTargetId, left.targetJointIndex)
				< std::tie(left.priority, right.producerOrder, right.goalId,
					right.target.stableTargetId, right.targetJointIndex);
		}

		bool SameGoalOrder(const ProceduralGoal& left, const ProceduralGoal& right) noexcept {
			return left.priority == right.priority && left.producerOrder == right.producerOrder
				&& left.goalId == right.goalId
				&& left.target.stableTargetId == right.target.stableTargetId
				&& left.targetJointIndex == right.targetJointIndex;
		}
	}

	bool IsSupportedProceduralCoordinateSpace(ProceduralCoordinateSpace space) noexcept {
		switch(space) {
		case ProceduralCoordinateSpace::Local:
		case ProceduralCoordinateSpace::Parent:
		case ProceduralCoordinateSpace::Model:
		case ProceduralCoordinateSpace::World:
			return true;
		}
		return false;
	}

	bool IsSupportedProceduralGoalKind(ProceduralGoalKind kind) noexcept {
		switch(kind) {
		case ProceduralGoalKind::Position:
		case ProceduralGoalKind::Rotation:
		case ProceduralGoalKind::PositionAndRotation:
		case ProceduralGoalKind::Aim:
		case ProceduralGoalKind::Look:
		case ProceduralGoalKind::FootPlant:
		case ProceduralGoalKind::CopyTransform:
		case ProceduralGoalKind::Twist:
			return true;
		}
		return false;
	}

	bool IsCurrentProceduralGeneration(
		const ProceduralGenerationStamp& observed,
		const ProceduralGenerationStamp& expected) noexcept {
		return observed.IsValid() && expected.IsValid() && observed == expected;
	}

	bool IsFinite(const ProceduralJointLimit& limit) noexcept {
		return std::isfinite(limit.minimumBendRadians)
			&& std::isfinite(limit.maximumBendRadians)
			&& std::isfinite(limit.maximumSwingRadians)
			&& std::isfinite(limit.maximumTwistRadians)
			&& limit.minimumBendRadians >= 0.0F
			&& limit.maximumBendRadians >= limit.minimumBendRadians
			&& limit.maximumSwingRadians >= 0.0F
			&& limit.maximumTwistRadians >= 0.0F;
	}

	bool IsFinite(const ProceduralDiagnosticRecord& diagnostic) noexcept {
		return std::isfinite(diagnostic.targetErrorMeters)
			&& std::isfinite(diagnostic.targetErrorRadians)
			&& std::isfinite(diagnostic.inputWeight)
			&& std::isfinite(diagnostic.finalContribution);
	}

	AnimationStatus BuildProceduralGoalSnapshot(
		const ProceduralGenerationStamp& expectedGeneration,
		std::span<const ProceduralGoal> goals,
		ProceduralGoalSnapshot& output) noexcept {
		if(!expectedGeneration.IsValid()) {
			return AnimationStatus::InvalidArgument;
		}
		if(goals.size() > kProceduralGoalHardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}

		try {
			ProceduralGoalSnapshot candidate;
			candidate.generation = expectedGeneration;
			candidate.goals.assign(goals.begin(), goals.end());
			for(const ProceduralGoal& goal : candidate.goals) {
				if(!ValidGoal(goal)) {
					return AnimationStatus::InvalidArgument;
				}
				if(!IsCurrentProceduralGeneration(goal.generation, expectedGeneration)) {
					return AnimationStatus::StaleHandle;
				}
			}
			std::sort(candidate.goals.begin(), candidate.goals.end(), GoalLess);
			for(std::size_t index = 1U; index < candidate.goals.size(); ++index) {
				if(SameGoalOrder(candidate.goals[index - 1U], candidate.goals[index])) {
					return AnimationStatus::InvalidArgument;
				}
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus BuildProceduralQuerySnapshot(
		const ProceduralGenerationStamp& generation,
		AnimationFixedTick capturedTick,
		std::span<const ProceduralQueryHit> hits,
		ProceduralQuerySnapshot& output) noexcept {
		if(!generation.IsValid() || generation.queryGeneration == 0U || capturedTick < 0) {
			return AnimationStatus::InvalidArgument;
		}
		if(hits.size() > kProceduralGoalHardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}

		try {
			ProceduralQuerySnapshot candidate;
			candidate.generation = generation;
			candidate.capturedTick = capturedTick;
			candidate.hits.assign(hits.begin(), hits.end());
			for(const ProceduralQueryHit& hit : candidate.hits) {
				if(hit.queryId == 0U || !hit.target.IsValid()
					|| !IsFinite(hit.point) || !IsFinite(hit.normal)
					|| !std::isfinite(hit.distance) || hit.distance < 0.0F
					|| (hit.hasTargetTransform && (!IsFinite(hit.targetTransform)
						|| !IsNormalized(hit.targetTransform.rotation, 0.01F)))) {
					return AnimationStatus::InvalidArgument;
				}
				AnimationFloat3 normalized;
				if(!ProceduralMath::TryNormalize(hit.normal, normalized)) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::sort(candidate.hits.begin(), candidate.hits.end(),
				[](const ProceduralQueryHit& left, const ProceduralQueryHit& right) {
					return std::tie(left.queryId, left.distance, left.target.stableTargetId,
						left.target.objectGeneration)
						< std::tie(right.queryId, right.distance, right.target.stableTargetId,
							right.target.objectGeneration);
				});
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	namespace ProceduralMath {
		AnimationFloat3 Add(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			return {left.x + right.x, left.y + right.y, left.z + right.z};
		}

		AnimationFloat3 Subtract(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			return {left.x - right.x, left.y - right.y, left.z - right.z};
		}

		AnimationFloat3 Scale(AnimationFloat3 value, float scale) noexcept {
			return {value.x * scale, value.y * scale, value.z * scale};
		}

		float Dot(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		AnimationFloat3 Cross(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			return {
				left.y * right.z - left.z * right.y,
				left.z * right.x - left.x * right.z,
				left.x * right.y - left.y * right.x};
		}

		float LengthSquared(AnimationFloat3 value) noexcept {
			return Dot(value, value);
		}

		float Length(AnimationFloat3 value) noexcept {
			const float squared = LengthSquared(value);
			return squared > 0.0F && std::isfinite(squared) ? std::sqrt(squared) : 0.0F;
		}

		bool TryNormalize(AnimationFloat3 value, AnimationFloat3& output) noexcept {
			if(!IsFinite(value)) {
				return false;
			}
			const double squared = static_cast<double>(value.x) * value.x
				+ static_cast<double>(value.y) * value.y
				+ static_cast<double>(value.z) * value.z;
			if(!std::isfinite(squared) || squared <= kLengthSquaredEpsilon) {
				return false;
			}
			const double inverseLength = 1.0 / std::sqrt(squared);
			output = {
				static_cast<float>(value.x * inverseLength),
				static_cast<float>(value.y * inverseLength),
				static_cast<float>(value.z * inverseLength)};
			return IsFinite(output);
		}

		AnimationFloat3 NormalizeOr(AnimationFloat3 value, AnimationFloat3 fallback) noexcept {
			AnimationFloat3 normalized;
			if(TryNormalize(value, normalized)) {
				return normalized;
			}
			if(TryNormalize(fallback, normalized)) {
				return normalized;
			}
			return {1.0F, 0.0F, 0.0F};
		}

		AnimationFloat3 Lerp(AnimationFloat3 from, AnimationFloat3 to, float weight) noexcept {
			const float clamped = std::clamp(weight, 0.0F, 1.0F);
			return Add(from, Scale(Subtract(to, from), clamped));
		}

		AnimationFloat3 DeterministicPerpendicular(AnimationFloat3 axis) noexcept {
			const AnimationFloat3 normalized = NormalizeOr(axis, {1.0F, 0.0F, 0.0F});
			// Cross with the least-aligned cardinal axis to avoid a near-zero result.
			const AnimationFloat3 cardinal = std::abs(normalized.x) <= std::abs(normalized.y)
				&& std::abs(normalized.x) <= std::abs(normalized.z)
				? AnimationFloat3{1.0F, 0.0F, 0.0F}
				: (std::abs(normalized.y) <= std::abs(normalized.z)
					? AnimationFloat3{0.0F, 1.0F, 0.0F}
					: AnimationFloat3{0.0F, 0.0F, 1.0F});
			return NormalizeOr(Cross(normalized, cardinal), {0.0F, 1.0F, 0.0F});
		}

		float Dot(AnimationQuaternion left, AnimationQuaternion right) noexcept {
			return left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
		}

		bool TryNormalize(AnimationQuaternion value, AnimationQuaternion& output) noexcept {
			if(!IsFinite(value)) {
				return false;
			}
			const double squared = static_cast<double>(value.x) * value.x
				+ static_cast<double>(value.y) * value.y
				+ static_cast<double>(value.z) * value.z
				+ static_cast<double>(value.w) * value.w;
			if(!std::isfinite(squared) || squared <= kLengthSquaredEpsilon) {
				return false;
			}
			const double inverseLength = 1.0 / std::sqrt(squared);
			output = Canonicalize({
				static_cast<float>(value.x * inverseLength),
				static_cast<float>(value.y * inverseLength),
				static_cast<float>(value.z * inverseLength),
				static_cast<float>(value.w * inverseLength)});
			return IsFinite(output);
		}

		AnimationQuaternion Canonicalize(AnimationQuaternion value) noexcept {
			const bool negate = value.w < 0.0F
				|| (value.w == 0.0F && value.x < 0.0F)
				|| (value.w == 0.0F && value.x == 0.0F && value.y < 0.0F)
				|| (value.w == 0.0F && value.x == 0.0F && value.y == 0.0F && value.z < 0.0F);
			return negate
				? AnimationQuaternion{-value.x, -value.y, -value.z, -value.w}
				: value;
		}

		AnimationQuaternion Conjugate(AnimationQuaternion value) noexcept {
			return {-value.x, -value.y, -value.z, value.w};
		}

		AnimationQuaternion Multiply(AnimationQuaternion left, AnimationQuaternion right) noexcept {
			AnimationQuaternion result{
				left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
				left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
				left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
				left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
			AnimationQuaternion normalized;
			return TryNormalize(result, normalized) ? normalized : AnimationQuaternion{};
		}

		AnimationQuaternion AxisAngle(AnimationFloat3 axis, float radians) noexcept {
			if(!std::isfinite(radians)) {
				return {};
			}
			const AnimationFloat3 normalized = NormalizeOr(axis, {1.0F, 0.0F, 0.0F});
			const float half = radians * 0.5F;
			return Canonicalize({
				normalized.x * std::sin(half),
				normalized.y * std::sin(half),
				normalized.z * std::sin(half),
				std::cos(half)});
		}

		AnimationQuaternion FromTo(AnimationFloat3 from, AnimationFloat3 to) noexcept {
			const AnimationFloat3 source = NormalizeOr(from, {1.0F, 0.0F, 0.0F});
			const AnimationFloat3 destination = NormalizeOr(to, source);
			const float dot = std::clamp(Dot(source, destination), -1.0F, 1.0F);
			if(dot >= 1.0F - kProceduralVectorEpsilon) {
				return {};
			}
			if(dot <= -1.0F + kProceduralVectorEpsilon) {
				return AxisAngle(DeterministicPerpendicular(source), kPi);
			}
			AnimationQuaternion value{
				Cross(source, destination).x,
				Cross(source, destination).y,
				Cross(source, destination).z,
				1.0F + dot};
			AnimationQuaternion normalized;
			return TryNormalize(value, normalized) ? normalized : AnimationQuaternion{};
		}

		AnimationQuaternion Nlerp(AnimationQuaternion from, AnimationQuaternion to, float weight) noexcept {
			AnimationQuaternion normalizedFrom;
			AnimationQuaternion normalizedTo;
			if(!TryNormalize(from, normalizedFrom) || !TryNormalize(to, normalizedTo)) {
				return {};
			}
			if(Dot(normalizedFrom, normalizedTo) < 0.0F) {
				normalizedTo = {-normalizedTo.x, -normalizedTo.y, -normalizedTo.z, -normalizedTo.w};
			}
			const float clamped = std::clamp(weight, 0.0F, 1.0F);
			AnimationQuaternion mixed{
				normalizedFrom.x + (normalizedTo.x - normalizedFrom.x) * clamped,
				normalizedFrom.y + (normalizedTo.y - normalizedFrom.y) * clamped,
				normalizedFrom.z + (normalizedTo.z - normalizedFrom.z) * clamped,
				normalizedFrom.w + (normalizedTo.w - normalizedFrom.w) * clamped};
			AnimationQuaternion output;
			return TryNormalize(mixed, output) ? output : normalizedFrom;
		}

		AnimationFloat3 Rotate(AnimationQuaternion rotation, AnimationFloat3 value) noexcept {
			AnimationQuaternion normalized;
			if(!TryNormalize(rotation, normalized)) {
				return value;
			}
			const AnimationFloat3 vectorPart{normalized.x, normalized.y, normalized.z};
			const AnimationFloat3 twiceCross = Scale(Cross(vectorPart, value), 2.0F);
			return Add(value, Add(Scale(twiceCross, normalized.w), Cross(vectorPart, twiceCross)));
		}

		AnimationFloat3 SlerpDirection(AnimationFloat3 from, AnimationFloat3 to, float weight) noexcept {
			const AnimationFloat3 source = NormalizeOr(from, {1.0F, 0.0F, 0.0F});
			const AnimationFloat3 destination = NormalizeOr(to, source);
			return NormalizeOr(Rotate(Nlerp({}, FromTo(source, destination), weight), source), source);
		}

		AnimationTransform Combine(
			const AnimationTransform& parent,
			const AnimationTransform& local) noexcept {
			AnimationTransform output;
			const AnimationFloat3 scaled{
				local.translation.x * parent.scale.x,
				local.translation.y * parent.scale.y,
				local.translation.z * parent.scale.z};
			output.translation = Add(parent.translation, Rotate(parent.rotation, scaled));
			output.rotation = Multiply(parent.rotation, local.rotation);
			output.scale = {
				parent.scale.x * local.scale.x,
				parent.scale.y * local.scale.y,
				parent.scale.z * local.scale.z};
			return output;
		}

		bool TryRelativeTransform(
			const AnimationTransform& parent,
			const AnimationTransform& world,
			AnimationTransform& output) noexcept {
			if(!IsFinite(parent) || !IsFinite(world) || !IsNormalized(parent.rotation, 0.01F)
				|| !IsNormalized(world.rotation, 0.01F)
				|| std::abs(parent.scale.x) <= kProceduralVectorEpsilon
				|| std::abs(parent.scale.y) <= kProceduralVectorEpsilon
				|| std::abs(parent.scale.z) <= kProceduralVectorEpsilon) {
				return false;
			}
			const AnimationQuaternion inverseRotation = Conjugate(parent.rotation);
			const AnimationFloat3 unrotated = Rotate(
				inverseRotation, Subtract(world.translation, parent.translation));
			AnimationTransform candidate;
			candidate.translation = {
				unrotated.x / parent.scale.x,
				unrotated.y / parent.scale.y,
				unrotated.z / parent.scale.z};
			candidate.rotation = Multiply(inverseRotation, world.rotation);
			candidate.scale = {
				world.scale.x / parent.scale.x,
				world.scale.y / parent.scale.y,
				world.scale.z / parent.scale.z};
			if(!IsFinite(candidate)) {
				return false;
			}
			output = candidate;
			return true;
		}

		AnimationTransform BlendTransform(
			const AnimationTransform& from,
			const AnimationTransform& to,
			float translationWeight,
			float rotationWeight,
			float scaleWeight) noexcept {
			AnimationTransform output;
			output.translation = Lerp(from.translation, to.translation, translationWeight);
			output.rotation = Nlerp(from.rotation, to.rotation, rotationWeight);
			output.scale = Lerp(from.scale, to.scale, scaleWeight);
			return output;
		}
	}
}
