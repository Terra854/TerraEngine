// ChainIk.cpp - Bounded FABRIK passes with priority-resolved copied goals.

#include "Animation/Procedural/ChainIk.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		constexpr float kPi = 3.14159265358979323846F;

		bool ValidJoint(const ChainIkJoint& joint) noexcept {
			return joint.stableJointId != 0U && IsFinite(joint.transform)
				&& IsNormalized(joint.transform.rotation, 0.01F)
				&& IsFinite(joint.limit);
		}

		bool PositionalGoal(const ProceduralGoal& goal) noexcept {
			return goal.kind == ProceduralGoalKind::Position
				|| goal.kind == ProceduralGoalKind::PositionAndRotation
				|| goal.kind == ProceduralGoalKind::FootPlant;
		}

		AnimationFloat3 LimitedDirection(
			AnimationFloat3 previousDirection,
			AnimationFloat3 candidateDirection,
			const ProceduralJointLimit& limit,
			bool& clamped,
			bool& singular) noexcept {
			using namespace ProceduralMath;
			const AnimationFloat3 previous = NormalizeOr(previousDirection, {1.0F, 0.0F, 0.0F});
			const AnimationFloat3 candidate = NormalizeOr(candidateDirection, previous);
			if(!limit.enabled) {
				return candidate;
			}
			const float bend = std::acos(std::clamp(Dot(previous, candidate), -1.0F, 1.0F));
			const float limited = std::clamp(
				bend, limit.minimumBendRadians, std::min(limit.maximumBendRadians, kPi));
			if(std::abs(limited - bend) <= kProceduralVectorEpsilon) {
				return candidate;
			}
			clamped = true;
			if(bend > kProceduralVectorEpsilon) {
				return SlerpDirection(previous, candidate, limited / bend);
			}
			AnimationFloat3 axis;
			if(!TryNormalize(Cross(previous, candidate), axis)) {
				axis = DeterministicPerpendicular(previous);
				singular = true;
			}
			return NormalizeOr(Rotate(AxisAngle(axis, limited), previous), previous);
		}

		ProceduralDiagnosticRecord GoalDiagnostic(
			const ChainIkRequest& request,
			const ProceduralGoal& goal,
			ProceduralSolverState state,
			AnimationStatus status) noexcept {
			ProceduralDiagnosticRecord diagnostic;
			diagnostic.requestedGeneration = request.expectedGeneration;
			diagnostic.observedGeneration = goal.generation;
			diagnostic.solver = ProceduralSolverKind::ChainIk;
			diagnostic.state = state;
			diagnostic.status = status;
			diagnostic.goalId = goal.goalId;
			diagnostic.producerOrder = goal.producerOrder;
			diagnostic.priority = goal.priority;
			diagnostic.inputWeight = goal.weight;
			diagnostic.iterationLimit = request.maximumIterations;
			return diagnostic;
		}
	}

	AnimationStatus SolveChainIk(
		const ChainIkRequest& request,
		ChainIkResult& output) noexcept {
		if(!request.generation.IsValid() || !request.expectedGeneration.IsValid()
			|| request.goalSnapshot == nullptr || request.joints.size() < 2U
			|| !std::isfinite(request.toleranceMeters) || request.toleranceMeters < 0.0F
			|| request.maximumIterations == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(request.joints.size() > static_cast<std::size_t>(
			GetAnimationRuntimeLimit(AnimationLimit::SkeletonJoints).hardMaximum)
			|| request.goalSnapshot->goals.size() > kProceduralGoalHardMaximum
			|| request.maximumIterations > kChainIkIterationHardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}

		try {
			ChainIkResult candidate;
			candidate.header.generation = request.expectedGeneration;
			candidate.joints.assign(request.joints.begin(), request.joints.end());
			for(const ChainIkJoint& joint : candidate.joints) {
				if(!ValidJoint(joint)) {
					return AnimationStatus::InvalidArgument;
				}
			}

			const bool requestStale = !IsCurrentProceduralGeneration(
				request.generation, request.expectedGeneration);
			const bool goalsStale = !IsCurrentProceduralGeneration(
				request.goalSnapshot->generation, request.expectedGeneration);
			if(requestStale || goalsStale) {
				candidate.header.status = AnimationStatus::StaleHandle;
				candidate.header.state = ProceduralSolverState::StaleInput;
				for(const ProceduralGoal& goal : request.goalSnapshot->goals) {
					ProceduralDiagnosticRecord diagnostic = GoalDiagnostic(
						request, goal, ProceduralSolverState::StaleInput, AnimationStatus::StaleHandle);
					diagnostic.staleInput = true;
					candidate.diagnostics.push_back(diagnostic);
				}
				output = std::move(candidate);
				return AnimationStatus::StaleHandle;
			}

			const std::uint32_t endJointIndex = static_cast<std::uint32_t>(request.joints.size() - 1U);
			const ProceduralGoal* firstContributingGoal = nullptr;
			for(const ProceduralGoal& goal : request.goalSnapshot->goals) {
				if(!IsCurrentProceduralGeneration(goal.generation, request.expectedGeneration)) {
					candidate.header.status = AnimationStatus::StaleHandle;
					candidate.header.state = ProceduralSolverState::StaleInput;
					ProceduralDiagnosticRecord diagnostic = GoalDiagnostic(
						request, goal, ProceduralSolverState::StaleInput, AnimationStatus::StaleHandle);
					diagnostic.staleInput = true;
					candidate.diagnostics.push_back(diagnostic);
					output = std::move(candidate);
					return AnimationStatus::StaleHandle;
				}
				if(PositionalGoal(goal) && goal.weight > 0.0F
					&& (goal.targetJointIndex == kInvalidProceduralJointIndex
						|| goal.targetJointIndex == endJointIndex)) {
					firstContributingGoal = &goal;
					break;
				}
			}

			if(firstContributingGoal == nullptr) {
				candidate.header.status = AnimationStatus::Ok;
				candidate.header.state = ProceduralSolverState::NoContribution;
				candidate.resolvedTarget = request.joints.back().transform.translation;
				for(const ProceduralGoal& goal : request.goalSnapshot->goals) {
					candidate.diagnostics.push_back(GoalDiagnostic(
						request, goal, ProceduralSolverState::NoContribution, AnimationStatus::Ok));
				}
				output = std::move(candidate);
				return AnimationStatus::Ok;
			}

			const std::int32_t selectedPriority = firstContributingGoal->priority;
			const ProceduralCoordinateSpace selectedSpace = firstContributingGoal->space;
			double targetX = 0.0;
			double targetY = 0.0;
			double targetZ = 0.0;
			double totalWeight = 0.0;
			for(const ProceduralGoal& goal : request.goalSnapshot->goals) {
				if(goal.priority != selectedPriority || goal.weight <= 0.0F || !PositionalGoal(goal)
					|| (goal.targetJointIndex != kInvalidProceduralJointIndex
						&& goal.targetJointIndex != endJointIndex)) {
					continue;
				}
				if(goal.space != selectedSpace) {
					return AnimationStatus::InvalidArgument;
				}
				totalWeight += goal.weight;
				targetX += static_cast<double>(goal.position.x) * goal.weight;
				targetY += static_cast<double>(goal.position.y) * goal.weight;
				targetZ += static_cast<double>(goal.position.z) * goal.weight;
			}
			if(!std::isfinite(totalWeight) || totalWeight <= 0.0) {
				return AnimationStatus::RangeOverflow;
			}
			candidate.resolvedTarget = {
				static_cast<float>(targetX / totalWeight),
				static_cast<float>(targetY / totalWeight),
				static_cast<float>(targetZ / totalWeight)};
			candidate.finalContribution = static_cast<float>(std::clamp(totalWeight, 0.0, 1.0));

			using namespace ProceduralMath;
			std::vector<AnimationFloat3> positions(request.joints.size());
			std::vector<AnimationFloat3> originalPositions(request.joints.size());
			std::vector<float> lengths(request.joints.size() - 1U, 0.0F);
			float totalLength = 0.0F;
			for(std::size_t index = 0U; index < request.joints.size(); ++index) {
				positions[index] = request.joints[index].transform.translation;
				originalPositions[index] = positions[index];
				if(index > 0U) {
					lengths[index - 1U] = Length(Subtract(positions[index], positions[index - 1U]));
					totalLength += lengths[index - 1U];
					candidate.encounteredZeroLength = candidate.encounteredZeroLength
						|| lengths[index - 1U] <= kProceduralVectorEpsilon;
				}
			}

			bool singular = candidate.encounteredZeroLength;
			bool clamped = false;
			bool unreachable = false;
			const AnimationFloat3 root = positions.front();
			const float rootToTarget = Length(Subtract(candidate.resolvedTarget, root));
			if(totalLength <= kProceduralVectorEpsilon) {
				singular = true;
			} else if(rootToTarget > totalLength + request.toleranceMeters) {
				unreachable = true;
				const AnimationFloat3 direction = NormalizeOr(
					Subtract(candidate.resolvedTarget, root), {1.0F, 0.0F, 0.0F});
				for(std::size_t index = 1U; index < positions.size(); ++index) {
					positions[index] = Add(positions[index - 1U], Scale(direction, lengths[index - 1U]));
				}
				candidate.iterationCount = 1U;
			} else {
				for(std::uint32_t iteration = 0U; iteration < request.maximumIterations; ++iteration) {
					positions.back() = candidate.resolvedTarget;
					for(std::size_t index = positions.size() - 1U; index > 0U; --index) {
						const float length = lengths[index - 1U];
						if(length <= kProceduralVectorEpsilon) {
							positions[index - 1U] = positions[index];
							continue;
						}
						const AnimationFloat3 fallback = Subtract(
							originalPositions[index - 1U], originalPositions[index]);
						const AnimationFloat3 direction = NormalizeOr(
							Subtract(positions[index - 1U], positions[index]), fallback);
						positions[index - 1U] = Add(positions[index], Scale(direction, length));
					}

					positions.front() = root;
					for(std::size_t index = 0U; index + 1U < positions.size(); ++index) {
						const float length = lengths[index];
						if(length <= kProceduralVectorEpsilon) {
							positions[index + 1U] = positions[index];
							continue;
						}
						const AnimationFloat3 fallback = Subtract(
							originalPositions[index + 1U], originalPositions[index]);
						AnimationFloat3 direction = NormalizeOr(
							Subtract(positions[index + 1U], positions[index]), fallback);
						if(index > 0U) {
							direction = LimitedDirection(
								Subtract(positions[index], positions[index - 1U]),
								direction, request.joints[index].limit, clamped, singular);
						}
						positions[index + 1U] = Add(positions[index], Scale(direction, length));
					}

					candidate.iterationCount = iteration + 1U;
					if(Length(Subtract(positions.back(), candidate.resolvedTarget))
						<= request.toleranceMeters) {
						break;
					}
				}
			}

			for(std::size_t index = 0U; index < candidate.joints.size(); ++index) {
				candidate.joints[index].transform.translation = Lerp(
					originalPositions[index], positions[index], candidate.finalContribution);
				if(index + 1U < candidate.joints.size()) {
					const AnimationFloat3 originalDirection = Subtract(
						originalPositions[index + 1U], originalPositions[index]);
					const AnimationFloat3 solvedDirection = Subtract(
						positions[index + 1U], positions[index]);
					const AnimationQuaternion solvedRotation = Multiply(
						FromTo(originalDirection, solvedDirection), request.joints[index].transform.rotation);
					candidate.joints[index].transform.rotation = Nlerp(
						request.joints[index].transform.rotation,
						solvedRotation, candidate.finalContribution);
				}
			}

			candidate.targetErrorMeters = Length(Subtract(
				candidate.joints.back().transform.translation, candidate.resolvedTarget));
			const bool converged = candidate.targetErrorMeters <= request.toleranceMeters;
			candidate.header.status = AnimationStatus::Ok;
			candidate.header.state = singular
				? ProceduralSolverState::Singular
				: (unreachable ? ProceduralSolverState::Unreachable
					: (clamped ? ProceduralSolverState::Clamped
						: (converged ? ProceduralSolverState::Converged
							: ProceduralSolverState::MaxIterations)));

			for(const ProceduralGoal& goal : request.goalSnapshot->goals) {
				ProceduralDiagnosticRecord diagnostic = GoalDiagnostic(
					request, goal, candidate.header.state, AnimationStatus::Ok);
				diagnostic.targetErrorMeters = Length(Subtract(
					candidate.joints.back().transform.translation, goal.position));
				diagnostic.iterationCount = candidate.iterationCount;
				diagnostic.converged = converged;
				diagnostic.clamped = clamped;
				diagnostic.singular = singular;
				diagnostic.unreachable = unreachable;
				if(goal.priority == selectedPriority && goal.weight > 0.0F && PositionalGoal(goal)
					&& (goal.targetJointIndex == kInvalidProceduralJointIndex
						|| goal.targetJointIndex == endJointIndex)) {
					diagnostic.finalContribution = candidate.finalContribution
						* static_cast<float>(goal.weight / totalWeight);
				} else {
					diagnostic.finalContribution = 0.0F;
					diagnostic.state = ProceduralSolverState::NoContribution;
				}
				candidate.diagnostics.push_back(diagnostic);
			}

			for(const ChainIkJoint& joint : candidate.joints) {
				if(!IsFinite(joint.transform) || !IsNormalized(joint.transform.rotation, 0.01F)) {
					return AnimationStatus::InternalError;
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
}
