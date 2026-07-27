// AnimationConstraints.cpp - Deterministic aim, copy, limit, and twist operations.

#include "Animation/Procedural/AnimationConstraints.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		constexpr float kPi = 3.14159265358979323846F;

		bool ValidTransform(const AnimationTransform& transform) noexcept {
			return IsFinite(transform) && IsNormalized(transform.rotation, 0.01F);
		}

		ProceduralDiagnosticRecord MakeConstraintDiagnostic(
			const ProceduralGenerationStamp& observed,
			const ProceduralGenerationStamp& expected,
			ProceduralSolverKind solver,
			std::uint64_t goalId,
			float inputWeight) noexcept {
			ProceduralDiagnosticRecord diagnostic;
			diagnostic.requestedGeneration = expected;
			diagnostic.observedGeneration = observed;
			diagnostic.solver = solver;
			diagnostic.state = ProceduralSolverState::Solved;
			diagnostic.status = AnimationStatus::Ok;
			diagnostic.goalId = goalId;
			diagnostic.inputWeight = inputWeight;
			return diagnostic;
		}

		bool PositionTarget(const ProceduralGoal& goal) noexcept {
			return goal.kind == ProceduralGoalKind::Position
				|| goal.kind == ProceduralGoalKind::PositionAndRotation
				|| goal.kind == ProceduralGoalKind::Aim
				|| goal.kind == ProceduralGoalKind::Look
				|| goal.kind == ProceduralGoalKind::FootPlant;
		}

		AnimationQuaternion ClampRotationDelta(
			AnimationQuaternion current,
			AnimationQuaternion desired,
			float maximumAngle,
			bool& clamped) noexcept {
			using namespace ProceduralMath;
			const AnimationQuaternion relative = Multiply(desired, Conjugate(current));
			const float angle = 2.0F * std::acos(std::clamp(relative.w, -1.0F, 1.0F));
			if(angle <= maximumAngle + kProceduralVectorEpsilon) {
				return desired;
			}
			AnimationFloat3 axis{relative.x, relative.y, relative.z};
			axis = NormalizeOr(axis, {1.0F, 0.0F, 0.0F});
			clamped = true;
			return Multiply(AxisAngle(axis, maximumAngle), current);
		}

		AnimationStatus AimAtResolvedTarget(
			const AimConstraintRequest& request,
			AnimationFloat3 targetPosition,
			float targetContribution,
			ProceduralSolverKind solver,
			bool preClamped,
			TransformConstraintResult& output) noexcept {
			using namespace ProceduralMath;
			TransformConstraintResult candidate;
			candidate.header = {request.expectedGeneration, AnimationStatus::Ok, ProceduralSolverState::Solved};
			candidate.transform = request.transform;
			candidate.diagnostic = MakeConstraintDiagnostic(
				request.generation, request.expectedGeneration, solver, 0U, request.weight);
			const AnimationFloat3 targetOffset = Subtract(targetPosition, request.transform.translation);
			AnimationFloat3 targetDirection;
			if(!TryNormalize(targetOffset, targetDirection)) {
				candidate.header.state = ProceduralSolverState::Singular;
				candidate.diagnostic.state = ProceduralSolverState::Singular;
				candidate.diagnostic.singular = true;
				output = candidate;
				return AnimationStatus::Ok;
			}

			const AnimationFloat3 currentAim = NormalizeOr(
				Rotate(request.transform.rotation, request.localAimAxis), targetDirection);
			AnimationQuaternion desired = Multiply(
				FromTo(currentAim, targetDirection), request.transform.rotation);
			// Roll correction projects both up vectors onto the plane orthogonal to
			// the aim direction, so it cannot disturb the target alignment.
			const AnimationFloat3 aimedUp = Rotate(desired, request.localUpAxis);
			const AnimationFloat3 projectedAimUp = Subtract(
				aimedUp, Scale(targetDirection, Dot(aimedUp, targetDirection)));
			const AnimationFloat3 projectedWorldUp = Subtract(
				request.worldUp, Scale(targetDirection, Dot(request.worldUp, targetDirection)));
			AnimationFloat3 normalizedAimUp;
			AnimationFloat3 normalizedWorldUp;
			if(TryNormalize(projectedAimUp, normalizedAimUp)
				&& TryNormalize(projectedWorldUp, normalizedWorldUp)) {
				desired = Multiply(FromTo(normalizedAimUp, normalizedWorldUp), desired);
			}

			bool clamped = preClamped;
			desired = ClampRotationDelta(
				request.transform.rotation, desired, request.maximumAngleRadians, clamped);
			const float dampingContribution = request.dampingRate > 0.0F
				? 1.0F - std::exp(-request.dampingRate * request.deltaSeconds)
				: 1.0F;
			const float contribution = std::clamp(
				request.weight * targetContribution * dampingContribution, 0.0F, 1.0F);
			candidate.transform.rotation = Nlerp(
				request.transform.rotation, desired, contribution);
			candidate.diagnostic.finalContribution = contribution;
			candidate.diagnostic.clamped = clamped;
			candidate.diagnostic.converged = Dot(
				NormalizeOr(Rotate(candidate.transform.rotation, request.localAimAxis), targetDirection),
				targetDirection) >= 0.9999F;
			candidate.diagnostic.targetErrorRadians = std::acos(std::clamp(Dot(
				NormalizeOr(Rotate(candidate.transform.rotation, request.localAimAxis), targetDirection),
				targetDirection), -1.0F, 1.0F));
			candidate.header.state = contribution == 0.0F
				? ProceduralSolverState::NoContribution
				: (clamped ? ProceduralSolverState::Clamped
					: (candidate.diagnostic.converged
						? ProceduralSolverState::Converged : ProceduralSolverState::Solved));
			candidate.diagnostic.state = candidate.header.state;
			output = candidate;
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus BlendProceduralTargets(
		const ProceduralGoalSnapshot& snapshot,
		const ProceduralGenerationStamp& expectedGeneration,
		const AnimationTransform& fallback,
		WeightedProceduralTargetResult& output) noexcept {
		if(!expectedGeneration.IsValid() || !ValidTransform(fallback)
			|| snapshot.goals.size() > kProceduralGoalHardMaximum) {
			return snapshot.goals.size() > kProceduralGoalHardMaximum
				? AnimationStatus::HardLimitExceeded : AnimationStatus::InvalidArgument;
		}

		try {
			WeightedProceduralTargetResult candidate;
			candidate.header.generation = expectedGeneration;
			candidate.transform = fallback;
			if(!IsCurrentProceduralGeneration(snapshot.generation, expectedGeneration)) {
				candidate.header.status = AnimationStatus::StaleHandle;
				candidate.header.state = ProceduralSolverState::StaleInput;
				output = std::move(candidate);
				return AnimationStatus::StaleHandle;
			}

			const ProceduralGoal* first = nullptr;
			for(const ProceduralGoal& goal : snapshot.goals) {
				if(goal.weight > 0.0F && PositionTarget(goal)) {
					first = &goal;
					break;
				}
			}
			if(first == nullptr) {
				candidate.header.status = AnimationStatus::Ok;
				candidate.header.state = ProceduralSolverState::NoContribution;
				output = std::move(candidate);
				return AnimationStatus::Ok;
			}

			const std::int32_t priority = first->priority;
			const ProceduralCoordinateSpace space = first->space;
			double positionX = 0.0;
			double positionY = 0.0;
			double positionZ = 0.0;
			double scaleX = 0.0;
			double scaleY = 0.0;
			double scaleZ = 0.0;
			double rotationX = 0.0;
			double rotationY = 0.0;
			double rotationZ = 0.0;
			double rotationW = 0.0;
			double totalWeight = 0.0;
			AnimationQuaternion anchor{};
			bool hasAnchor = false;
			for(const ProceduralGoal& goal : snapshot.goals) {
				if(goal.priority != priority || goal.weight <= 0.0F || !PositionTarget(goal)) {
					continue;
				}
				if(goal.space != space || !IsCurrentProceduralGeneration(goal.generation, expectedGeneration)) {
					return goal.space != space ? AnimationStatus::InvalidArgument : AnimationStatus::StaleHandle;
				}
				AnimationQuaternion rotation;
				if(!ProceduralMath::TryNormalize(goal.rotation, rotation)) {
					return AnimationStatus::InvalidArgument;
				}
				if(!hasAnchor) {
					anchor = rotation;
					hasAnchor = true;
				} else if(ProceduralMath::Dot(anchor, rotation) < 0.0F) {
					rotation = {-rotation.x, -rotation.y, -rotation.z, -rotation.w};
				}
				totalWeight += goal.weight;
				positionX += static_cast<double>(goal.position.x) * goal.weight;
				positionY += static_cast<double>(goal.position.y) * goal.weight;
				positionZ += static_cast<double>(goal.position.z) * goal.weight;
				scaleX += static_cast<double>(fallback.scale.x) * goal.weight;
				scaleY += static_cast<double>(fallback.scale.y) * goal.weight;
				scaleZ += static_cast<double>(fallback.scale.z) * goal.weight;
				rotationX += static_cast<double>(rotation.x) * goal.weight;
				rotationY += static_cast<double>(rotation.y) * goal.weight;
				rotationZ += static_cast<double>(rotation.z) * goal.weight;
				rotationW += static_cast<double>(rotation.w) * goal.weight;
			}
			if(!std::isfinite(totalWeight) || totalWeight <= 0.0) {
				return AnimationStatus::RangeOverflow;
			}

			AnimationTransform blendedTarget;
			blendedTarget.translation = {
				static_cast<float>(positionX / totalWeight),
				static_cast<float>(positionY / totalWeight),
				static_cast<float>(positionZ / totalWeight)};
			blendedTarget.scale = {
				static_cast<float>(scaleX / totalWeight),
				static_cast<float>(scaleY / totalWeight),
				static_cast<float>(scaleZ / totalWeight)};
			if(!ProceduralMath::TryNormalize({
				static_cast<float>(rotationX / totalWeight),
				static_cast<float>(rotationY / totalWeight),
				static_cast<float>(rotationZ / totalWeight),
				static_cast<float>(rotationW / totalWeight)}, blendedTarget.rotation)) {
				return AnimationStatus::InvalidArgument;
			}
			candidate.finalContribution = static_cast<float>(std::clamp(totalWeight, 0.0, 1.0));
			candidate.transform = ProceduralMath::BlendTransform(
				fallback, blendedTarget, candidate.finalContribution,
				candidate.finalContribution, candidate.finalContribution);
			candidate.header.status = AnimationStatus::Ok;
			candidate.header.state = ProceduralSolverState::Solved;

			for(const ProceduralGoal& goal : snapshot.goals) {
				ProceduralDiagnosticRecord diagnostic = MakeConstraintDiagnostic(
					goal.generation, expectedGeneration, ProceduralSolverKind::WeightedTargets,
					goal.goalId, goal.weight);
				diagnostic.priority = goal.priority;
				diagnostic.producerOrder = goal.producerOrder;
				if(goal.priority == priority && goal.weight > 0.0F && PositionTarget(goal)) {
					diagnostic.finalContribution = candidate.finalContribution
						* static_cast<float>(goal.weight / totalWeight);
				} else {
					diagnostic.state = ProceduralSolverState::NoContribution;
				}
				candidate.diagnostics.push_back(diagnostic);
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus SolveAimConstraint(
		const AimConstraintRequest& request,
		TransformConstraintResult& output) noexcept {
		if(!request.generation.IsValid() || !request.expectedGeneration.IsValid()
			|| request.goalSnapshot == nullptr || !ValidTransform(request.transform)
			|| !IsFinite(request.localAimAxis) || !IsFinite(request.localUpAxis)
			|| !IsFinite(request.worldUp) || !std::isfinite(request.maximumAngleRadians)
			|| request.maximumAngleRadians < 0.0F || request.maximumAngleRadians > kPi
			|| !std::isfinite(request.dampingRate) || request.dampingRate < 0.0F
			|| !std::isfinite(request.deltaSeconds) || request.deltaSeconds < 0.0F
			|| !std::isfinite(request.weight) || request.weight < 0.0F || request.weight > 1.0F) {
			return AnimationStatus::InvalidArgument;
		}
		if(!IsCurrentProceduralGeneration(request.generation, request.expectedGeneration)) {
			TransformConstraintResult candidate;
			candidate.header = {request.expectedGeneration, AnimationStatus::StaleHandle,
				ProceduralSolverState::StaleInput};
			candidate.transform = request.transform;
			candidate.diagnostic = MakeConstraintDiagnostic(
				request.generation, request.expectedGeneration, ProceduralSolverKind::Aim, 0U, request.weight);
			candidate.diagnostic.status = AnimationStatus::StaleHandle;
			candidate.diagnostic.state = ProceduralSolverState::StaleInput;
			candidate.diagnostic.staleInput = true;
			output = candidate;
			return AnimationStatus::StaleHandle;
		}
		AnimationFloat3 aimAxis;
		AnimationFloat3 upAxis;
		AnimationFloat3 worldUp;
		if(!ProceduralMath::TryNormalize(request.localAimAxis, aimAxis)
			|| !ProceduralMath::TryNormalize(request.localUpAxis, upAxis)
			|| !ProceduralMath::TryNormalize(request.worldUp, worldUp)
			|| std::abs(ProceduralMath::Dot(aimAxis, upAxis)) > 0.999F) {
			return AnimationStatus::InvalidArgument;
		}

		WeightedProceduralTargetResult target;
		const AnimationStatus targetStatus = BlendProceduralTargets(
			*request.goalSnapshot, request.expectedGeneration, request.transform, target);
		if(targetStatus != AnimationStatus::Ok) {
			return targetStatus;
		}
		return AimAtResolvedTarget(
			request, target.transform.translation, target.finalContribution,
			ProceduralSolverKind::Aim, false, output);
	}

	AnimationStatus SolveLookConstraint(
		const LookConstraintRequest& request,
		TransformConstraintResult& output) noexcept {
		if(!std::isfinite(request.maximumYawRadians) || request.maximumYawRadians < 0.0F
			|| request.maximumYawRadians > kPi
			|| !std::isfinite(request.maximumPitchRadians)
			|| request.maximumPitchRadians < 0.0F || request.maximumPitchRadians > kPi * 0.5F
			|| request.aim.goalSnapshot == nullptr) {
			return AnimationStatus::InvalidArgument;
		}
		// Reuse aim validation and target weighting, but clamp yaw and pitch around
		// the current orthonormal look basis before applying roll stabilization.
		if(!IsCurrentProceduralGeneration(
			request.aim.generation, request.aim.expectedGeneration)) {
			AimConstraintRequest stale = request.aim;
			return SolveAimConstraint(stale, output);
		}
		WeightedProceduralTargetResult target;
		const AnimationStatus targetStatus = BlendProceduralTargets(
			*request.aim.goalSnapshot, request.aim.expectedGeneration,
			request.aim.transform, target);
		if(targetStatus != AnimationStatus::Ok) {
			return targetStatus;
		}

		using namespace ProceduralMath;
		const AnimationFloat3 forward = NormalizeOr(
			Rotate(request.aim.transform.rotation, request.aim.localAimAxis), {0.0F, 0.0F, 1.0F});
		AnimationFloat3 up = NormalizeOr(
			Rotate(request.aim.transform.rotation, request.aim.localUpAxis), {0.0F, 1.0F, 0.0F});
		AnimationFloat3 right = NormalizeOr(Cross(up, forward), DeterministicPerpendicular(forward));
		up = NormalizeOr(Cross(forward, right), up);
		const AnimationFloat3 offset = Subtract(
			target.transform.translation, request.aim.transform.translation);
		const float distance = Length(offset);
		if(distance <= kProceduralVectorEpsilon) {
			return AimAtResolvedTarget(request.aim, target.transform.translation,
				target.finalContribution, ProceduralSolverKind::Look, false, output);
		}
		const AnimationFloat3 desired = Scale(offset, 1.0F / distance);
		const float yaw = std::atan2(Dot(desired, right), Dot(desired, forward));
		const float pitch = std::asin(std::clamp(Dot(desired, up), -1.0F, 1.0F));
		const float limitedYaw = std::clamp(
			yaw, -request.maximumYawRadians, request.maximumYawRadians);
		const float limitedPitch = std::clamp(
			pitch, -request.maximumPitchRadians, request.maximumPitchRadians);
		const bool clamped = std::abs(limitedYaw - yaw) > kProceduralVectorEpsilon
			|| std::abs(limitedPitch - pitch) > kProceduralVectorEpsilon;
		const float cosinePitch = std::cos(limitedPitch);
		const AnimationFloat3 limitedDirection = NormalizeOr(Add(
			Add(Scale(forward, cosinePitch * std::cos(limitedYaw)),
				Scale(right, cosinePitch * std::sin(limitedYaw))),
			Scale(up, std::sin(limitedPitch))), forward);
		AimConstraintRequest unrestricted = request.aim;
		unrestricted.maximumAngleRadians = kPi;
		return AimAtResolvedTarget(unrestricted,
			Add(unrestricted.transform.translation, Scale(limitedDirection, distance)),
			target.finalContribution, ProceduralSolverKind::Look, clamped, output);
	}

	AnimationStatus ApplyCopyTransformConstraint(
		const CopyTransformConstraintRequest& request,
		TransformConstraintResult& output) noexcept {
		if(!request.generation.IsValid() || !request.expectedGeneration.IsValid()
			|| !ValidTransform(request.currentLocalTransform) || !ValidTransform(request.targetTransform)
			|| !ValidTransform(request.parentWorldTransform)
			|| !IsSupportedProceduralCoordinateSpace(request.targetSpace)
			|| !std::isfinite(request.translationWeight) || request.translationWeight < 0.0F
			|| request.translationWeight > 1.0F || !std::isfinite(request.rotationWeight)
			|| request.rotationWeight < 0.0F || request.rotationWeight > 1.0F
			|| !std::isfinite(request.scaleWeight) || request.scaleWeight < 0.0F
			|| request.scaleWeight > 1.0F) {
			return AnimationStatus::InvalidArgument;
		}
		TransformConstraintResult candidate;
		candidate.header.generation = request.expectedGeneration;
		candidate.transform = request.currentLocalTransform;
		candidate.diagnostic = MakeConstraintDiagnostic(
			request.generation, request.expectedGeneration,
			ProceduralSolverKind::CopyTransform, request.goalId,
			std::max({request.translationWeight, request.rotationWeight, request.scaleWeight}));
		if(!IsCurrentProceduralGeneration(request.generation, request.expectedGeneration)) {
			candidate.header.status = AnimationStatus::StaleHandle;
			candidate.header.state = ProceduralSolverState::StaleInput;
			candidate.diagnostic.status = AnimationStatus::StaleHandle;
			candidate.diagnostic.state = ProceduralSolverState::StaleInput;
			candidate.diagnostic.staleInput = true;
			output = candidate;
			return AnimationStatus::StaleHandle;
		}

		AnimationTransform localTarget = request.targetTransform;
		if(request.targetSpace == ProceduralCoordinateSpace::World
			|| request.targetSpace == ProceduralCoordinateSpace::Model) {
			if(!ProceduralMath::TryRelativeTransform(
				request.parentWorldTransform, request.targetTransform, localTarget)) {
				return AnimationStatus::InvalidArgument;
			}
		}
		candidate.transform = ProceduralMath::BlendTransform(
			request.currentLocalTransform, localTarget,
			request.translationWeight, request.rotationWeight, request.scaleWeight);
		candidate.header.status = AnimationStatus::Ok;
		candidate.header.state = ProceduralSolverState::Solved;
		candidate.diagnostic.finalContribution = std::max({
			request.translationWeight, request.rotationWeight, request.scaleWeight});
		candidate.diagnostic.state = candidate.diagnostic.finalContribution == 0.0F
			? ProceduralSolverState::NoContribution : ProceduralSolverState::Solved;
		candidate.header.state = candidate.diagnostic.state;
		output = candidate;
		return AnimationStatus::Ok;
	}

	AnimationStatus ApplyJointRotationConstraint(
		const JointRotationConstraintRequest& request,
		JointRotationConstraintResult& output) noexcept {
		if(!request.generation.IsValid() || !request.expectedGeneration.IsValid()
			|| !IsFinite(request.referenceRotation) || !IsFinite(request.candidateRotation)
			|| !IsNormalized(request.referenceRotation, 0.01F)
			|| !IsNormalized(request.candidateRotation, 0.01F)
			|| !IsFinite(request.localTwistAxis) || !IsFinite(request.limit)
			|| !std::isfinite(request.weight) || request.weight < 0.0F || request.weight > 1.0F) {
			return AnimationStatus::InvalidArgument;
		}

		JointRotationConstraintResult candidate;
		candidate.header.generation = request.expectedGeneration;
		candidate.rotation = request.candidateRotation;
		candidate.diagnostic = MakeConstraintDiagnostic(
			request.generation, request.expectedGeneration,
			ProceduralSolverKind::JointConstraint, request.goalId, request.weight);
		if(!IsCurrentProceduralGeneration(request.generation, request.expectedGeneration)) {
			candidate.header.status = AnimationStatus::StaleHandle;
			candidate.header.state = ProceduralSolverState::StaleInput;
			candidate.diagnostic.status = AnimationStatus::StaleHandle;
			candidate.diagnostic.state = ProceduralSolverState::StaleInput;
			candidate.diagnostic.staleInput = true;
			output = candidate;
			return AnimationStatus::StaleHandle;
		}

		using namespace ProceduralMath;
		AnimationFloat3 twistAxis;
		if(!TryNormalize(request.localTwistAxis, twistAxis)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationQuaternion relative = Multiply(
			Conjugate(request.referenceRotation), request.candidateRotation);
		const AnimationFloat3 relativeVector{relative.x, relative.y, relative.z};
		const float projection = Dot(relativeVector, twistAxis);
		AnimationQuaternion twist;
		if(!TryNormalize({twistAxis.x * projection, twistAxis.y * projection,
			twistAxis.z * projection, relative.w}, twist)) {
			twist = {};
		}
		AnimationQuaternion swing = Multiply(relative, Conjugate(twist));
		float twistAngle = 2.0F * std::atan2(
			Dot(AnimationFloat3{twist.x, twist.y, twist.z}, twistAxis), twist.w);
		float swingAngle = 2.0F * std::acos(std::clamp(swing.w, -1.0F, 1.0F));
		bool clamped = false;
		if(request.limit.enabled) {
			const float limitedTwist = std::clamp(
				twistAngle, -request.limit.maximumTwistRadians, request.limit.maximumTwistRadians);
			const float limitedSwing = std::min(swingAngle, request.limit.maximumSwingRadians);
			clamped = std::abs(limitedTwist - twistAngle) > kProceduralVectorEpsilon
				|| std::abs(limitedSwing - swingAngle) > kProceduralVectorEpsilon;
			twistAngle = limitedTwist;
			swingAngle = limitedSwing;
			AnimationFloat3 swingAxis{swing.x, swing.y, swing.z};
			swingAxis = NormalizeOr(swingAxis, DeterministicPerpendicular(twistAxis));
			swing = AxisAngle(swingAxis, swingAngle);
			twist = AxisAngle(twistAxis, twistAngle);
		}
		const AnimationQuaternion limitedRotation = Multiply(
			request.referenceRotation, Multiply(swing, twist));
		candidate.rotation = Nlerp(request.candidateRotation, limitedRotation, request.weight);
		candidate.swingRadians = swingAngle;
		candidate.twistRadians = twistAngle;
		candidate.header.status = AnimationStatus::Ok;
		candidate.header.state = request.weight == 0.0F
			? ProceduralSolverState::NoContribution
			: (clamped ? ProceduralSolverState::Clamped : ProceduralSolverState::Solved);
		candidate.diagnostic.state = candidate.header.state;
		candidate.diagnostic.clamped = clamped;
		candidate.diagnostic.finalContribution = request.weight;
		output = candidate;
		return AnimationStatus::Ok;
	}

	AnimationStatus ApplyTwistDistribution(
		const TwistDistributionRequest& request,
		TwistDistributionResult& output) noexcept {
		if(!request.generation.IsValid() || !request.expectedGeneration.IsValid()
			|| request.transforms.empty()
			|| request.transforms.size() != request.distributionWeights.size()
			|| !IsFinite(request.localTwistAxis) || !std::isfinite(request.twistRadians)
			|| !std::isfinite(request.weight) || request.weight < 0.0F || request.weight > 1.0F) {
			return AnimationStatus::InvalidArgument;
		}
		if(request.transforms.size() > static_cast<std::size_t>(
			GetAnimationRuntimeLimit(AnimationLimit::SkeletonJoints).hardMaximum)) {
			return AnimationStatus::HardLimitExceeded;
		}

		try {
			TwistDistributionResult candidate;
			candidate.header.generation = request.expectedGeneration;
			candidate.transforms.assign(request.transforms.begin(), request.transforms.end());
			if(!IsCurrentProceduralGeneration(request.generation, request.expectedGeneration)) {
				candidate.header.status = AnimationStatus::StaleHandle;
				candidate.header.state = ProceduralSolverState::StaleInput;
				output = std::move(candidate);
				return AnimationStatus::StaleHandle;
			}
			AnimationFloat3 axis;
			if(!ProceduralMath::TryNormalize(request.localTwistAxis, axis)) {
				return AnimationStatus::InvalidArgument;
			}
			double totalWeight = 0.0;
			for(std::size_t index = 0U; index < request.transforms.size(); ++index) {
				if(!ValidTransform(request.transforms[index])
					|| !std::isfinite(request.distributionWeights[index])
					|| request.distributionWeights[index] < 0.0F) {
					return AnimationStatus::InvalidArgument;
				}
				totalWeight += request.distributionWeights[index];
			}
			if(!std::isfinite(totalWeight)) {
				return AnimationStatus::RangeOverflow;
			}
			for(std::size_t index = 0U; index < candidate.transforms.size(); ++index) {
				const float normalizedWeight = totalWeight > 0.0
					? static_cast<float>(request.distributionWeights[index] / totalWeight) : 0.0F;
				const AnimationQuaternion twisted = ProceduralMath::Multiply(
					request.transforms[index].rotation,
					ProceduralMath::AxisAngle(axis, request.twistRadians * normalizedWeight));
				candidate.transforms[index].rotation = ProceduralMath::Nlerp(
					request.transforms[index].rotation, twisted, request.weight);
				ProceduralDiagnosticRecord diagnostic = MakeConstraintDiagnostic(
					request.generation, request.expectedGeneration,
					ProceduralSolverKind::TwistDistribution,
					request.goalId + index, request.distributionWeights[index]);
				diagnostic.finalContribution = normalizedWeight * request.weight;
				diagnostic.state = diagnostic.finalContribution > 0.0F
					? ProceduralSolverState::Solved : ProceduralSolverState::NoContribution;
				candidate.diagnostics.push_back(diagnostic);
			}
			candidate.header.status = AnimationStatus::Ok;
			candidate.header.state = totalWeight > 0.0 && request.weight > 0.0F
				? ProceduralSolverState::Solved : ProceduralSolverState::NoContribution;
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
