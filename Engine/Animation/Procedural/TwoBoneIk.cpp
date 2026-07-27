// TwoBoneIk.cpp - Law-of-cosines IK with deterministic singular fallbacks.

#include "Animation/Procedural/TwoBoneIk.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Animation {
	namespace {
		constexpr float kPi = 3.14159265358979323846F;

		ProceduralDiagnosticRecord MakeDiagnostic(
			const TwoBoneIkRequest& request,
			ProceduralSolverState state,
			AnimationStatus status) noexcept {
			ProceduralDiagnosticRecord diagnostic;
			diagnostic.requestedGeneration = request.expectedGeneration;
			diagnostic.observedGeneration = request.generation;
			diagnostic.solver = ProceduralSolverKind::TwoBoneIk;
			diagnostic.state = state;
			diagnostic.status = status;
			diagnostic.goalId = request.goalId;
			diagnostic.producerOrder = request.producerOrder;
			diagnostic.priority = request.priority;
			diagnostic.inputWeight = request.weight;
			diagnostic.iterationLimit = 1U;
			return diagnostic;
		}

		bool ValidTransform(const AnimationTransform& transform) noexcept {
			return IsFinite(transform) && IsNormalized(transform.rotation, 0.01F);
		}

		TwoBoneIkResult MakePreservedResult(
			const TwoBoneIkRequest& request,
			ProceduralSolverState state,
			AnimationStatus status) noexcept {
			TwoBoneIkResult result;
			result.header = {request.expectedGeneration, status, state};
			result.rootTransform = request.rootTransform;
			result.middleTransform = request.middleTransform;
			result.endTransform = request.endTransform;
			result.upperLength = ProceduralMath::Length(ProceduralMath::Subtract(
				request.middleTransform.translation, request.rootTransform.translation));
			result.lowerLength = ProceduralMath::Length(ProceduralMath::Subtract(
				request.endTransform.translation, request.middleTransform.translation));
			result.targetErrorMeters = ProceduralMath::Length(ProceduralMath::Subtract(
				request.endTransform.translation, request.targetPosition));
			result.diagnostic = MakeDiagnostic(request, state, status);
			result.diagnostic.targetErrorMeters = result.targetErrorMeters;
			result.diagnostic.staleInput = state == ProceduralSolverState::StaleInput;
			result.diagnostic.singular = state == ProceduralSolverState::Singular;
			return result;
		}
	}

	AnimationStatus SolveTwoBoneIk(
		const TwoBoneIkRequest& request,
		TwoBoneIkResult& output) noexcept {
		if(!request.expectedGeneration.IsValid() || !request.generation.IsValid()) {
			return AnimationStatus::InvalidArgument;
		}
		if(!IsCurrentProceduralGeneration(request.generation, request.expectedGeneration)) {
			output = MakePreservedResult(
				request, ProceduralSolverState::StaleInput, AnimationStatus::StaleHandle);
			return AnimationStatus::StaleHandle;
		}
		if(!ValidTransform(request.rootTransform) || !ValidTransform(request.middleTransform)
			|| !ValidTransform(request.endTransform) || !IsFinite(request.targetPosition)
			|| !IsFinite(request.polePosition) || !IsFinite(request.middleLimit)
			|| !IsSupportedProceduralCoordinateSpace(request.space)
			|| !std::isfinite(request.weight) || request.weight < 0.0F || request.weight > 1.0F
			|| !std::isfinite(request.maximumStretchScale) || request.maximumStretchScale < 1.0F
			|| !std::isfinite(request.twistRadians)
			|| !std::isfinite(request.toleranceMeters) || request.toleranceMeters < 0.0F) {
			return AnimationStatus::InvalidArgument;
		}

		using namespace ProceduralMath;
		TwoBoneIkResult candidate = MakePreservedResult(
			request, ProceduralSolverState::Solved, AnimationStatus::Ok);
		const AnimationFloat3 originalUpper = Subtract(
			request.middleTransform.translation, request.rootTransform.translation);
		const AnimationFloat3 originalLower = Subtract(
			request.endTransform.translation, request.middleTransform.translation);
		const float originalUpperLength = Length(originalUpper);
		const float originalLowerLength = Length(originalLower);
		candidate.upperLength = originalUpperLength;
		candidate.lowerLength = originalLowerLength;

		if(originalUpperLength <= kProceduralVectorEpsilon
			|| originalLowerLength <= kProceduralVectorEpsilon) {
			candidate.header.state = ProceduralSolverState::Singular;
			candidate.diagnostic.state = ProceduralSolverState::Singular;
			candidate.diagnostic.singular = true;
			candidate.diagnostic.finalContribution = 0.0F;
			output = candidate;
			return AnimationStatus::Ok;
		}

		const AnimationFloat3 root = request.rootTransform.translation;
		const AnimationFloat3 targetOffset = Subtract(request.targetPosition, root);
		const float requestedDistance = Length(targetOffset);
		bool singular = requestedDistance <= kProceduralVectorEpsilon;
		const AnimationFloat3 originalReach = Subtract(
			request.endTransform.translation, request.rootTransform.translation);
		const AnimationFloat3 targetDirection = NormalizeOr(
			targetOffset, NormalizeOr(originalReach, originalUpper));

		const float baseReach = originalUpperLength + originalLowerLength;
		const float maximumStretch = request.allowStretch ? request.maximumStretchScale : 1.0F;
		const float desiredStretch = requestedDistance > baseReach
			? requestedDistance / baseReach : 1.0F;
		candidate.stretchScale = std::clamp(desiredStretch, 1.0F, maximumStretch);
		const float upperLength = originalUpperLength * candidate.stretchScale;
		const float lowerLength = originalLowerLength * candidate.stretchScale;
		const float maximumReach = upperLength + lowerLength;
		const float minimumReach = std::abs(upperLength - lowerLength);
		bool unreachable = requestedDistance > maximumReach + request.toleranceMeters
			|| requestedDistance < minimumReach - request.toleranceMeters;
		float solvedDistance = std::clamp(
			requestedDistance, minimumReach, maximumReach);
		if(solvedDistance <= kProceduralVectorEpsilon) {
			// Equal segments may fold exactly onto the root. A tiny deterministic
			// distance keeps the law-of-cosines denominator finite.
			solvedDistance = std::min(maximumReach, kProceduralVectorEpsilon);
			singular = true;
		}

		const float bendNumerator = upperLength * upperLength
			+ lowerLength * lowerLength - solvedDistance * solvedDistance;
		const float bendDenominator = 2.0F * upperLength * lowerLength;
		const float interiorRadians = std::acos(std::clamp(
			bendNumerator / bendDenominator, -1.0F, 1.0F));
		float bendRadians = kPi - interiorRadians;
		bool clamped = false;
		if(request.middleLimit.enabled) {
			const float limitedBend = std::clamp(
				bendRadians,
				request.middleLimit.minimumBendRadians,
				request.middleLimit.maximumBendRadians);
			clamped = std::abs(limitedBend - bendRadians) > kProceduralVectorEpsilon;
			bendRadians = limitedBend;
			const float distanceSquared = upperLength * upperLength
				+ lowerLength * lowerLength
				+ 2.0F * upperLength * lowerLength * std::cos(bendRadians);
			solvedDistance = std::sqrt(std::max(0.0F, distanceSquared));
		}
		candidate.bendRadians = bendRadians;

		AnimationFloat3 bendDirection;
		const AnimationFloat3 poleOffset = Subtract(request.polePosition, root);
		const AnimationFloat3 projectedPole = Subtract(
			poleOffset, Scale(targetDirection, Dot(poleOffset, targetDirection)));
		if(!TryNormalize(projectedPole, bendDirection)) {
			const AnimationFloat3 projectedOriginal = Subtract(
				originalUpper, Scale(targetDirection, Dot(originalUpper, targetDirection)));
			if(!TryNormalize(projectedOriginal, bendDirection)) {
				bendDirection = DeterministicPerpendicular(targetDirection);
			}
			candidate.usedPoleFallback = true;
			singular = true;
		}

		const float maximumTwist = request.middleLimit.enabled
			? request.middleLimit.maximumTwistRadians : kPi;
		const float appliedTwist = std::clamp(request.twistRadians, -maximumTwist, maximumTwist);
		clamped = clamped || std::abs(appliedTwist - request.twistRadians) > kProceduralVectorEpsilon;
		bendDirection = NormalizeOr(
			Rotate(AxisAngle(targetDirection, appliedTwist), bendDirection), bendDirection);

		const float rootAngleCosine = solvedDistance <= kProceduralVectorEpsilon
			? 1.0F
			: std::clamp((upperLength * upperLength + solvedDistance * solvedDistance
				- lowerLength * lowerLength) / (2.0F * upperLength * solvedDistance),
				-1.0F, 1.0F);
		const float rootAngleSine = std::sqrt(std::max(
			0.0F, 1.0F - rootAngleCosine * rootAngleCosine));
		const AnimationFloat3 solvedMiddle = Add(root, Add(
			Scale(targetDirection, upperLength * rootAngleCosine),
			Scale(bendDirection, upperLength * rootAngleSine)));
		const AnimationFloat3 solvedEnd = Add(root, Scale(targetDirection, solvedDistance));

		candidate.middleTransform.translation = Lerp(
			request.middleTransform.translation, solvedMiddle, request.weight);
		candidate.endTransform.translation = Lerp(
			request.endTransform.translation, solvedEnd, request.weight);
		const AnimationQuaternion rootDelta = FromTo(
			originalUpper, Subtract(solvedMiddle, root));
		const AnimationQuaternion middleDelta = FromTo(
			originalLower, Subtract(solvedEnd, solvedMiddle));
		candidate.rootTransform.rotation = Nlerp(
			request.rootTransform.rotation,
			Multiply(rootDelta, request.rootTransform.rotation), request.weight);
		candidate.middleTransform.rotation = Nlerp(
			request.middleTransform.rotation,
			Multiply(middleDelta, request.middleTransform.rotation), request.weight);

		candidate.targetErrorMeters = Length(Subtract(
			candidate.endTransform.translation, request.targetPosition));
		candidate.header.state = singular
			? ProceduralSolverState::Singular
			: (unreachable ? ProceduralSolverState::Unreachable
				: (clamped ? ProceduralSolverState::Clamped
					: (candidate.targetErrorMeters <= request.toleranceMeters
						? ProceduralSolverState::Converged : ProceduralSolverState::Solved)));
		candidate.diagnostic.state = candidate.header.state;
		candidate.diagnostic.targetErrorMeters = candidate.targetErrorMeters;
		candidate.diagnostic.finalContribution = request.weight;
		candidate.diagnostic.iterationCount = 1U;
		candidate.diagnostic.converged = candidate.targetErrorMeters <= request.toleranceMeters;
		candidate.diagnostic.clamped = clamped;
		candidate.diagnostic.singular = singular;
		candidate.diagnostic.unreachable = unreachable;
		if(request.weight == 0.0F) {
			candidate.header.state = ProceduralSolverState::NoContribution;
			candidate.diagnostic.state = ProceduralSolverState::NoContribution;
		}

		if(!IsFinite(candidate.rootTransform) || !IsFinite(candidate.middleTransform)
			|| !IsFinite(candidate.endTransform) || !IsFinite(candidate.diagnostic)) {
			return AnimationStatus::InternalError;
		}
		output = candidate;
		return AnimationStatus::Ok;
	}
}
