// FootPlacement.cpp - Bounded contact locking from immutable query snapshots.

#include "Animation/Procedural/FootPlacement.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		bool ValidTransform(const AnimationTransform& transform) noexcept {
			return IsFinite(transform) && IsNormalized(transform.rotation, 0.01F);
		}

		bool ValidContact(const FootContactState& contact, std::uint64_t stableFootId) noexcept {
			if(static_cast<std::uint8_t>(contact.phase)
				> static_cast<std::uint8_t>(FootContactPhase::Releasing)) {
				return false;
			}
			if(contact.stableFootId != 0U && contact.stableFootId != stableFootId) {
				return false;
			}
			if(!ValidTransform(contact.lockedTransform) || !IsFinite(contact.surfaceNormal)
				|| !std::isfinite(contact.contribution)
				|| contact.contribution < 0.0F || contact.contribution > 1.0F) {
				return false;
			}
			if(contact.phase == FootContactPhase::Released) {
				return contact.contribution == 0.0F;
			}
			AnimationFloat3 normalizedNormal;
			return contact.target.IsValid() && contact.contribution > 0.0F
				&& ProceduralMath::TryNormalize(contact.surfaceNormal, normalizedNormal);
		}

		bool ValidHit(const ProceduralQueryHit& hit) noexcept {
			AnimationFloat3 normalizedNormal;
			return hit.queryId != 0U && hit.target.IsValid()
				&& IsFinite(hit.point) && IsFinite(hit.normal)
				&& ProceduralMath::TryNormalize(hit.normal, normalizedNormal)
				&& std::isfinite(hit.distance) && hit.distance >= 0.0F
				&& (!hit.hasTargetTransform
					|| (ValidTransform(hit.targetTransform)));
		}

		bool ValidFoot(const FootPlacementFootInput& foot) noexcept {
			return foot.stableFootId != 0U && foot.queryId != 0U
				&& ValidTransform(foot.animatedTransform) && IsFinite(foot.hipPosition)
				&& ValidContact(foot.previousContact, foot.stableFootId)
				&& std::isfinite(foot.maximumReachMeters) && foot.maximumReachMeters >= 0.0F
				&& std::isfinite(foot.soleOffsetMeters)
				&& std::isfinite(foot.positionWeight) && foot.positionWeight >= 0.0F
				&& foot.positionWeight <= 1.0F
				&& std::isfinite(foot.orientationWeight) && foot.orientationWeight >= 0.0F
				&& foot.orientationWeight <= 1.0F;
		}

		const ProceduralQueryHit* FindNearestBlockingHit(
			const ProceduralQuerySnapshot& snapshot,
			std::uint64_t queryId) noexcept {
			// Snapshot construction sorts by query id and distance. The first blocking
			// match is therefore deterministic even when the producer returned ties.
			for(const ProceduralQueryHit& hit : snapshot.hits) {
				if(hit.queryId > queryId) {
					break;
				}
				if(hit.queryId == queryId && hit.blocking) {
					return &hit;
				}
			}
			return nullptr;
		}

		float SmoothingAlpha(float deltaSeconds, float durationSeconds) noexcept {
			if(durationSeconds <= 0.0F) {
				return 1.0F;
			}
			return std::clamp(deltaSeconds / durationSeconds, 0.0F, 1.0F);
		}

		ProceduralDiagnosticRecord MakeDiagnostic(
			const FootPlacementRequest& request,
			ProceduralSolverKind solver,
			std::uint64_t goalId,
			float inputWeight) noexcept {
			ProceduralDiagnosticRecord diagnostic;
			diagnostic.requestedGeneration = request.expectedGeneration;
			diagnostic.observedGeneration = request.generation != request.expectedGeneration
				? request.generation
				: (request.querySnapshot != nullptr
					? request.querySnapshot->generation : request.generation);
			diagnostic.solver = solver;
			diagnostic.state = ProceduralSolverState::Solved;
			diagnostic.status = AnimationStatus::Ok;
			diagnostic.goalId = goalId;
			diagnostic.inputWeight = inputWeight;
			return diagnostic;
		}

		FootPlacementFootResult MakePreservedFoot(
			const FootPlacementRequest& request,
			const FootPlacementFootInput& foot,
			AnimationStatus status,
			ProceduralSolverState state) noexcept {
			FootPlacementFootResult result;
			result.stableFootId = foot.stableFootId;
			result.transform = foot.animatedTransform;
			result.contact = foot.previousContact;
			result.diagnostic = MakeDiagnostic(
				request, ProceduralSolverKind::FootPlacement, foot.stableFootId,
				std::max(foot.positionWeight, foot.orientationWeight));
			result.diagnostic.status = status;
			result.diagnostic.state = state;
			result.diagnostic.staleInput = state == ProceduralSolverState::StaleInput;
			return result;
		}
	}

	AnimationStatus SolveFootPlacement(
		const FootPlacementRequest& request,
		FootPlacementResult& output) noexcept {
		if(!request.generation.IsValid() || !request.expectedGeneration.IsValid()
			|| request.querySnapshot == nullptr || !ValidTransform(request.pelvisTransform)
			|| request.feet.empty() || request.feet.size() > kProceduralGoalHardMaximum
			|| !std::isfinite(request.previousPelvisOffsetMeters)
			|| !std::isfinite(request.maximumPelvisDropMeters)
			|| request.maximumPelvisDropMeters < 0.0F
			|| !std::isfinite(request.maximumPelvisRaiseMeters)
			|| request.maximumPelvisRaiseMeters < 0.0F
			|| !std::isfinite(request.pelvisWeight) || request.pelvisWeight < 0.0F
			|| request.pelvisWeight > 1.0F
			|| !std::isfinite(request.deltaSeconds) || request.deltaSeconds < 0.0F
			|| !std::isfinite(request.releaseSmoothingSeconds)
			|| request.releaseSmoothingSeconds < 0.0F
			|| !std::isfinite(request.pelvisSmoothingSeconds)
			|| request.pelvisSmoothingSeconds < 0.0F
			|| request.maximumQueryAgeTicks < 0) {
			return request.feet.size() > kProceduralGoalHardMaximum
				? AnimationStatus::HardLimitExceeded : AnimationStatus::InvalidArgument;
		}
		for(const FootPlacementFootInput& foot : request.feet) {
			if(!ValidFoot(foot)) {
				return AnimationStatus::InvalidArgument;
			}
		}
		if(request.querySnapshot->hits.size() > kProceduralGoalHardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}
		for(const ProceduralQueryHit& hit : request.querySnapshot->hits) {
			if(!ValidHit(hit)) {
				return AnimationStatus::InvalidArgument;
			}
		}
		for(std::size_t left = 0U; left < request.feet.size(); ++left) {
			for(std::size_t right = left + 1U; right < request.feet.size(); ++right) {
				if(request.feet[left].stableFootId == request.feet[right].stableFootId) {
					return AnimationStatus::InvalidArgument;
				}
			}
		}

		try {
			FootPlacementResult candidate;
			candidate.header.generation = request.expectedGeneration;
			candidate.pelvisTransform = request.pelvisTransform;
			candidate.feet.reserve(request.feet.size());
			candidate.diagnostics.reserve(request.feet.size() + 1U);

			const bool generationStale = !IsCurrentProceduralGeneration(
				request.generation, request.expectedGeneration)
				|| !IsCurrentProceduralGeneration(
					request.querySnapshot->generation, request.expectedGeneration);
			const AnimationFixedTick queryAge = request.expectedGeneration.tick
				- request.querySnapshot->capturedTick;
			const bool ageStale = queryAge < 0 || queryAge > request.maximumQueryAgeTicks;
			if(generationStale || ageStale) {
				candidate.header.status = AnimationStatus::StaleHandle;
				candidate.header.state = ProceduralSolverState::StaleInput;
				for(const FootPlacementFootInput& foot : request.feet) {
					FootPlacementFootResult result = MakePreservedFoot(
						request, foot, AnimationStatus::StaleHandle,
						ProceduralSolverState::StaleInput);
					candidate.diagnostics.push_back(result.diagnostic);
					candidate.feet.push_back(std::move(result));
				}
				output = std::move(candidate);
				return AnimationStatus::StaleHandle;
			}

			using namespace ProceduralMath;
			double pelvisOffsetSum = 0.0;
			std::size_t contributingFeet = 0U;
			bool anyClamped = false;
			for(const FootPlacementFootInput& foot : request.feet) {
				FootPlacementFootResult result = MakePreservedFoot(
					request, foot, AnimationStatus::Ok, ProceduralSolverState::NoContribution);
				const ProceduralQueryHit* hit = FindNearestBlockingHit(
					*request.querySnapshot, foot.queryId);
				AnimationTransform target = foot.animatedTransform;
				float contactContribution = 0.0F;
				if(hit != nullptr) {
					AnimationFloat3 normal = NormalizeOr(hit->normal, {0.0F, 1.0F, 0.0F});
					const bool retainLock = foot.lockOnContact
						&& foot.previousContact.phase == FootContactPhase::Locked
						&& foot.previousContact.target == hit->target;
					if(retainLock) {
						target = foot.previousContact.lockedTransform;
						normal = foot.previousContact.surfaceNormal;
					} else {
						target.translation = Add(hit->point, Scale(normal, foot.soleOffsetMeters));
						const AnimationFloat3 currentUp = NormalizeOr(
							Rotate(foot.animatedTransform.rotation, {0.0F, 1.0F, 0.0F}),
							{0.0F, 1.0F, 0.0F});
						target.rotation = Multiply(
							FromTo(currentUp, normal), foot.animatedTransform.rotation);
					}

					const AnimationFloat3 hipToTarget = Subtract(
						target.translation, foot.hipPosition);
					const float requestedReach = Length(hipToTarget);
					if(requestedReach > foot.maximumReachMeters + kProceduralVectorEpsilon) {
						const AnimationFloat3 direction = NormalizeOr(
							hipToTarget, {0.0F, -1.0F, 0.0F});
						target.translation = Add(
							foot.hipPosition, Scale(direction, foot.maximumReachMeters));
						result.targetErrorMeters = requestedReach - foot.maximumReachMeters;
						result.reachClamped = true;
						anyClamped = true;
					}

					result.transform = BlendTransform(
						foot.animatedTransform, target,
						foot.positionWeight, foot.orientationWeight, 0.0F);
					contactContribution = std::max(
						foot.positionWeight, foot.orientationWeight);
					result.contact.stableFootId = foot.stableFootId;
					result.contact.target = hit->target;
					result.contact.lockedTransform = target;
					result.contact.surfaceNormal = normal;
					result.contact.contribution = contactContribution;
					// Locked also means "active contact state". lockOnContact controls
					// whether the next frame retains this transform or follows a new hit.
					result.contact.phase = contactContribution > 0.0F
						? FootContactPhase::Locked : FootContactPhase::Released;
					result.diagnostic.state = result.reachClamped
						? ProceduralSolverState::Clamped
						: (contactContribution > 0.0F
							? ProceduralSolverState::Solved
							: ProceduralSolverState::NoContribution);
					result.diagnostic.clamped = result.reachClamped;
					result.diagnostic.targetErrorMeters = result.targetErrorMeters;
					result.diagnostic.finalContribution = contactContribution;
				} else if(foot.previousContact.phase != FootContactPhase::Released
					&& foot.previousContact.contribution > 0.0F) {
					const float releaseAlpha = SmoothingAlpha(
						request.deltaSeconds, request.releaseSmoothingSeconds);
					contactContribution = foot.previousContact.contribution * (1.0F - releaseAlpha);
					result.transform = BlendTransform(
						foot.animatedTransform, foot.previousContact.lockedTransform,
						contactContribution * foot.positionWeight,
						contactContribution * foot.orientationWeight, 0.0F);
					result.contact = foot.previousContact;
					result.contact.contribution = contactContribution;
					result.contact.phase = contactContribution > kProceduralVectorEpsilon
						? FootContactPhase::Releasing : FootContactPhase::Released;
					if(result.contact.phase == FootContactPhase::Released) {
						result.contact.target = {};
						result.contact.contribution = 0.0F;
					}
					result.diagnostic.state = contactContribution > 0.0F
						? ProceduralSolverState::Solved : ProceduralSolverState::NoContribution;
					result.diagnostic.finalContribution = contactContribution;
				} else {
					result.contact = {};
					result.contact.stableFootId = foot.stableFootId;
				}

				if(contactContribution > 0.0F) {
					pelvisOffsetSum += static_cast<double>(
						result.transform.translation.y - foot.animatedTransform.translation.y);
					++contributingFeet;
				}
				if(!ValidTransform(result.transform) || !IsFinite(result.diagnostic)) {
					return AnimationStatus::InternalError;
				}
				candidate.diagnostics.push_back(result.diagnostic);
				candidate.feet.push_back(std::move(result));
			}

			const float rawPelvisOffset = contributingFeet > 0U
				? static_cast<float>(pelvisOffsetSum / static_cast<double>(contributingFeet))
					* request.pelvisWeight
				: 0.0F;
			const float boundedPelvisOffset = std::clamp(
				rawPelvisOffset,
				-request.maximumPelvisDropMeters,
				request.maximumPelvisRaiseMeters);
			const bool pelvisClamped = std::abs(
				boundedPelvisOffset - rawPelvisOffset) > kProceduralVectorEpsilon;
			candidate.pelvisOffsetMeters = request.previousPelvisOffsetMeters
				+ (boundedPelvisOffset - request.previousPelvisOffsetMeters)
					* SmoothingAlpha(request.deltaSeconds, request.pelvisSmoothingSeconds);
			candidate.pelvisTransform.translation.y += candidate.pelvisOffsetMeters;

			ProceduralDiagnosticRecord pelvisDiagnostic = MakeDiagnostic(
				request, ProceduralSolverKind::PelvisCompensation, 0U, request.pelvisWeight);
			pelvisDiagnostic.finalContribution = contributingFeet > 0U
				? request.pelvisWeight : 0.0F;
			pelvisDiagnostic.clamped = pelvisClamped;
			pelvisDiagnostic.state = contributingFeet == 0U
				? ProceduralSolverState::NoContribution
				: (pelvisClamped ? ProceduralSolverState::Clamped
					: ProceduralSolverState::Solved);
			candidate.diagnostics.push_back(pelvisDiagnostic);
			candidate.header.status = AnimationStatus::Ok;
			candidate.header.state = anyClamped || pelvisClamped
				? ProceduralSolverState::Clamped
				: (contributingFeet > 0U ? ProceduralSolverState::Solved
					: ProceduralSolverState::NoContribution);

			if(!ValidTransform(candidate.pelvisTransform)
				|| !std::isfinite(candidate.pelvisOffsetMeters)) {
				return AnimationStatus::InternalError;
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
