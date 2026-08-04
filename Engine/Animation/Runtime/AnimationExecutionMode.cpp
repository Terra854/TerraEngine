// AnimationExecutionMode.cpp - Pure requested-to-effective animation policy.

#include "Animation/Runtime/AnimationExecutionMode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		// FNV-1a gives diagnostics and hysteresis a stable identity without relying
		// on implementation-defined std::hash results or native byte order.
		constexpr std::uint64_t kFingerprintOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFingerprintPrime = 1099511628211ULL;

		void HashByte(std::uint64_t& hash, std::uint8_t value) noexcept
		{
			hash ^= value;
			hash *= kFingerprintPrime;
		}

		void HashU32(std::uint64_t& hash, std::uint32_t value) noexcept
		{
			for(std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
				HashByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
			}
		}

		void HashU64(std::uint64_t& hash, std::uint64_t value) noexcept
		{
			for(std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
				HashByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffULL));
			}
		}

		void CopyConsumerFlags(
			const AnimationConsumerRequirements& source,
			AnimationConsumerClosure& destination) noexcept
		{
			destination.curves = source.curves;
			destination.morphs = source.morphs;
			destination.events = source.events;
			destination.rootMotion = source.rootMotion;
			destination.contacts = source.contacts;
			destination.proceduralResults = source.proceduralResults;
			destination.physics = source.physics;
			destination.attachments = source.attachments;
			destination.networking = source.networking;
			destination.rollback = source.rollback;
			destination.replay = source.replay;
			destination.recording = source.recording;
			destination.automation = source.automation;
			destination.diagnostics = source.diagnostics;
			destination.completePose = source.completePose;
		}

		void FinalizeConsumerFingerprint(AnimationConsumerClosure& closure) noexcept
		{
			std::uint64_t hash = kFingerprintOffset;
			for(const std::uint32_t joint : closure.joints) {
				HashU32(hash, joint);
			}
			HashByte(hash, closure.curves ? 1U : 0U);
			HashByte(hash, closure.morphs ? 1U : 0U);
			HashByte(hash, closure.events ? 1U : 0U);
			HashByte(hash, closure.rootMotion ? 1U : 0U);
			HashByte(hash, closure.contacts ? 1U : 0U);
			HashByte(hash, closure.proceduralResults ? 1U : 0U);
			HashByte(hash, closure.physics ? 1U : 0U);
			HashByte(hash, closure.attachments ? 1U : 0U);
			HashByte(hash, closure.networking ? 1U : 0U);
			HashByte(hash, closure.rollback ? 1U : 0U);
			HashByte(hash, closure.replay ? 1U : 0U);
			HashByte(hash, closure.recording ? 1U : 0U);
			HashByte(hash, closure.automation ? 1U : 0U);
			HashByte(hash, closure.diagnostics ? 1U : 0U);
			HashByte(hash, closure.completePose ? 1U : 0U);
			closure.fingerprint = hash;
		}

		AnimationConsumerClosure ResolveConsumerClosure(const AnimationConsumerRequirements& requirements)
		{
			AnimationConsumerClosure closure;
			CopyConsumerFlags(requirements, closure);

			const std::size_t jointCount = requirements.jointParents.size();
			if(jointCount > kMaximumAnimationExecutionJoints
				|| requirements.requestedJoints.size() > kMaximumAnimationExecutionJoints) {
				closure.status = AnimationStatus::HardLimitExceeded;
				return closure;
			}
			if(requirements.requestedJoints.empty()) {
				FinalizeConsumerFingerprint(closure);
				return closure;
			}
			if(jointCount == 0U) {
				closure.status = AnimationStatus::InvalidArgument;
				return closure;
			}

			// Every node has at most one parent, so an iterative ancestor walk can
			// prove the requested forest without recursion. Fixed-capacity scratch
			// arrays make the hard bound explicit and prevent a malformed cycle from
			// growing temporary storage indefinitely.
			std::array<std::uint32_t, kMaximumAnimationExecutionJoints> visitedEpoch{};
			std::array<std::uint8_t, kMaximumAnimationExecutionJoints> resolved{};
			std::array<std::uint32_t, kMaximumAnimationExecutionJoints> path{};
			std::uint32_t epoch = 0U;

			try {
				closure.joints.reserve(jointCount);
				for(const std::uint32_t requestedJoint : requirements.requestedJoints) {
					if(requestedJoint >= jointCount) {
						closure.joints.clear();
						closure.status = AnimationStatus::InvalidArgument;
						return closure;
					}

					++epoch;
					std::size_t pathCount = 0U;
					std::uint32_t currentJoint = requestedJoint;
					while(resolved[currentJoint] == 0U) {
						if(visitedEpoch[currentJoint] == epoch || pathCount >= jointCount) {
							closure.joints.clear();
							closure.status = AnimationStatus::InvalidArgument;
							return closure;
						}
						visitedEpoch[currentJoint] = epoch;
						path[pathCount++] = currentJoint;

						const std::int32_t parent = requirements.jointParents[currentJoint];
						if(parent == -1) {
							break;
						}
						if(parent < 0 || static_cast<std::size_t>(parent) >= jointCount) {
							closure.joints.clear();
							closure.status = AnimationStatus::InvalidArgument;
							return closure;
						}
						currentJoint = static_cast<std::uint32_t>(parent);
					}

					for(std::size_t index = 0U; index < pathCount; ++index) {
						const std::uint32_t joint = path[index];
						if(resolved[joint] == 0U) {
							resolved[joint] = 1U;
							closure.joints.push_back(joint);
						}
					}
				}
			} catch(const std::bad_alloc&) {
				closure.joints.clear();
				closure.status = AnimationStatus::InternalError;
				return closure;
			}

			std::sort(closure.joints.begin(), closure.joints.end());
			FinalizeConsumerFingerprint(closure);
			return closure;
		}

		bool IsKnownMode(AnimationEffectiveExecutionMode mode) noexcept
		{
			switch(mode) {
			case AnimationEffectiveExecutionMode::GpuVisual:
			case AnimationEffectiveExecutionMode::HybridAuthoritative:
			case AnimationEffectiveExecutionMode::CpuDeterministicFallback:
				return true;
			default:
				return false;
			}
		}

		bool IsKnownReason(AnimationExecutionModeReason reason) noexcept
		{
			return reason >= AnimationExecutionModeReason::Uninitialized
				&& reason <= AnimationExecutionModeReason::InvalidPreviousState;
		}

		bool IsKnownPolicy(AnimationExecutionPolicy policy) noexcept
		{
			switch(policy) {
			case AnimationExecutionPolicy::Auto:
			case AnimationExecutionPolicy::ForceGpuVisual:
			case AnimationExecutionPolicy::ForceHybridAuthoritative:
			case AnimationExecutionPolicy::ForceCpuDeterministicFallback:
				return true;
			default:
				return false;
			}
		}

		int AuthorityRank(AnimationEffectiveExecutionMode mode) noexcept
		{
			return static_cast<int>(mode);
		}

		bool IsValidRequestTimeline(const AnimationExecutionModeRequest& request) noexcept
		{
			return request.fixedTick >= 0
				&& request.stability.promotionHysteresisTicks >= 0
				&& request.stability.minimumResidenceTicks >= 0;
		}

		bool IsValidPreviousState(
			const AnimationExecutionModeState& state,
			AnimationFixedTick currentTick) noexcept
		{
			if(!state.initialized) {
				return true;
			}
			if(!IsKnownMode(state.effectiveMode)
				|| !IsKnownReason(state.effectiveReason)
				|| state.effectiveReason == AnimationExecutionModeReason::Uninitialized
				|| state.modeEnteredTick < 0
				|| state.modeEnteredTick > currentTick) {
				return false;
			}
			if(!state.promotionPending) {
				return true;
			}
			if(!IsKnownMode(state.promotionMode)
				|| !IsKnownReason(state.promotionReason)
				|| state.promotionReason == AnimationExecutionModeReason::Uninitialized
				|| state.promotionSinceTick < state.modeEnteredTick
				|| state.promotionSinceTick > currentTick
				|| AuthorityRank(state.promotionMode) >= AuthorityRank(state.effectiveMode)) {
				return false;
			}

			// A pending record is written only by the promotion path below. Restricting
			// its reason-to-mode combinations rejects torn or manually corrupted state
			// without inventing a transition to a guessed evaluator.
			switch(state.promotionMode) {
			case AnimationEffectiveExecutionMode::GpuVisual:
				return state.promotionReason == AnimationExecutionModeReason::ForceGpuRequested
					|| state.promotionReason == AnimationExecutionModeReason::GpuVisualEligible;
			case AnimationEffectiveExecutionMode::HybridAuthoritative:
				return state.promotionReason == AnimationExecutionModeReason::ForceHybridRequested
					|| state.promotionReason == AnimationExecutionModeReason::AuthoritativeSubsetRequired;
			case AnimationEffectiveExecutionMode::CpuDeterministicFallback:
			default:
				return false;
			}
		}

		bool HasElapsed(
			AnimationFixedTick currentTick,
			AnimationFixedTick startTick,
			AnimationFixedTick requiredTicks) noexcept
		{
			if(requiredTicks <= 0) {
				return true;
			}
			if(currentTick < startTick) {
				return false;
			}
			const std::uint64_t elapsed = static_cast<std::uint64_t>(currentTick)
				- static_cast<std::uint64_t>(startTick);
			return elapsed >= static_cast<std::uint64_t>(requiredTicks);
		}

		std::uint64_t MakePromotionFingerprint(
			const AnimationExecutionModeDecision& decision,
			AnimationExecutionPolicy policy) noexcept
		{
			std::uint64_t hash = kFingerprintOffset;
			HashU64(hash, decision.consumerClosure.fingerprint);
			HashByte(hash, static_cast<std::uint8_t>(decision.eligibleMode));
			HashByte(hash, static_cast<std::uint8_t>(decision.eligibilityReason));
			HashByte(hash, static_cast<std::uint8_t>(policy));
			return hash;
		}

		AnimationExecutionModeState MakeState(
			AnimationEffectiveExecutionMode mode,
			AnimationExecutionModeReason reason,
			AnimationFixedTick fixedTick,
			const AnimationModeContinuityToken& continuity) noexcept
		{
			AnimationExecutionModeState state;
			state.initialized = true;
			state.effectiveMode = mode;
			state.effectiveReason = reason;
			state.modeEnteredTick = fixedTick;
			state.continuity = continuity;
			return state;
		}

		void SelectEligibleMode(
			const AnimationExecutionModeRequest& request,
			AnimationExecutionModeDecision& decision) noexcept
		{
			const auto select = [&decision](
				AnimationEffectiveExecutionMode mode,
				AnimationExecutionModeReason reason) {
				decision.eligibleMode = mode;
				decision.eligibilityReason = reason;
			};

			if(decision.consumerClosure.status != AnimationStatus::Ok) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::InvalidConsumerClosure);
				return;
			}
			if(request.deterministicRequired || request.consumers.replay) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::DeterministicRequired);
				return;
			}
			if(request.rollbackOrResimulation || request.consumers.rollback) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::RollbackRequired);
				return;
			}
			if(request.headless) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::HeadlessRequired);
				return;
			}
			if(request.validationReference) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::ValidationRequired);
				return;
			}
			if(!IsKnownPolicy(request.requestedPolicy) || !IsValidRequestTimeline(request)) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::InvalidRequestedPolicy);
				return;
			}
			if(!request.gpu.rendererAvailable) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::RendererUnavailable);
				return;
			}
			if(!request.gpu.animationCapabilityAvailable) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::GpuCapabilityUnavailable);
				return;
			}
			if(!request.gpu.resourcesResident) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::GpuResourcesUnavailable);
				return;
			}
			if(!request.gpu.evaluationHealthy) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::GpuEvaluationFailure);
				return;
			}
			if(!std::isfinite(request.gpu.requiredBudget)
				|| !std::isfinite(request.gpu.availableBudget)
				|| request.gpu.requiredBudget < 0.0
				|| request.gpu.availableBudget < 0.0
				|| request.gpu.requiredBudget > request.gpu.availableBudget) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::GpuBudgetUnavailable);
				return;
			}
			if(request.requestedPolicy == AnimationExecutionPolicy::ForceCpuDeterministicFallback) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::ForceCpuRequested);
				return;
			}
			if(decision.consumerClosure.completePose) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::CompletePoseRequired);
				return;
			}
			if(request.requestedPolicy == AnimationExecutionPolicy::ForceGpuVisual
				&& decision.consumerClosure.HasAuthoritativeSubset()) {
				select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
					AnimationExecutionModeReason::ForceGpuUnsafe);
				return;
			}
			if(request.requestedPolicy == AnimationExecutionPolicy::ForceHybridAuthoritative) {
				if(!decision.consumerClosure.HasAuthoritativeSubset()) {
					select(AnimationEffectiveExecutionMode::CpuDeterministicFallback,
						AnimationExecutionModeReason::ForceHybridMissingSubset);
					return;
				}
				select(AnimationEffectiveExecutionMode::HybridAuthoritative,
					AnimationExecutionModeReason::ForceHybridRequested);
				return;
			}
			if(decision.consumerClosure.HasAuthoritativeSubset()) {
				select(AnimationEffectiveExecutionMode::HybridAuthoritative,
					AnimationExecutionModeReason::AuthoritativeSubsetRequired);
				return;
			}
			if(request.requestedPolicy == AnimationExecutionPolicy::ForceGpuVisual) {
				select(AnimationEffectiveExecutionMode::GpuVisual,
					AnimationExecutionModeReason::ForceGpuRequested);
				return;
			}
			select(AnimationEffectiveExecutionMode::GpuVisual,
				AnimationExecutionModeReason::GpuVisualEligible);
		}
	}

	bool AnimationConsumerClosure::HasAuthoritativeSubset() const noexcept
	{
		return !joints.empty()
			|| curves
			|| morphs
			|| events
			|| rootMotion
			|| contacts
			|| proceduralResults
			|| physics
			|| attachments
			|| networking
			|| rollback
			|| replay
			|| recording
			|| automation
			|| diagnostics
			|| completePose;
	}

	bool RequiresCpuRootMotionAuthority(
		AnimatorRootMotionPolicy policy,
		bool currentClipHasRootMotion,
		bool interruptedTransitionHasRootMotion,
		bool destinationTransitionHasRootMotion,
		bool explicitlyConsumed) noexcept
	{
		// An explicit reader remains authoritative even when the current authored
		// clip is in-place. Otherwise Ignore suppresses automatic extraction and the
		// remaining policies require CPU work only when a real delta can exist.
		return explicitlyConsumed
			|| (policy != AnimatorRootMotionPolicy::Ignore
				&& (currentClipHasRootMotion
					|| interruptedTransitionHasRootMotion
					|| destinationTransitionHasRootMotion));
	}

	std::string_view ToString(AnimationEffectiveExecutionMode mode) noexcept
	{
		switch(mode) {
		case AnimationEffectiveExecutionMode::GpuVisual:
			return "GpuVisual";
		case AnimationEffectiveExecutionMode::HybridAuthoritative:
			return "HybridAuthoritative";
		case AnimationEffectiveExecutionMode::CpuDeterministicFallback:
			return "CpuDeterministicFallback";
		default:
			return "Unknown";
		}
	}

	std::string_view ToString(AnimationExecutionModeReason reason) noexcept
	{
		switch(reason) {
		case AnimationExecutionModeReason::Uninitialized:
			return "Uninitialized";
		case AnimationExecutionModeReason::InvalidConsumerClosure:
			return "InvalidConsumerClosure";
		case AnimationExecutionModeReason::DeterministicRequired:
			return "DeterministicRequired";
		case AnimationExecutionModeReason::RollbackRequired:
			return "RollbackRequired";
		case AnimationExecutionModeReason::HeadlessRequired:
			return "HeadlessRequired";
		case AnimationExecutionModeReason::ValidationRequired:
			return "ValidationRequired";
		case AnimationExecutionModeReason::RendererUnavailable:
			return "RendererUnavailable";
		case AnimationExecutionModeReason::GpuCapabilityUnavailable:
			return "GpuCapabilityUnavailable";
		case AnimationExecutionModeReason::GpuResourcesUnavailable:
			return "GpuResourcesUnavailable";
		case AnimationExecutionModeReason::GpuEvaluationFailure:
			return "GpuEvaluationFailure";
		case AnimationExecutionModeReason::GpuBudgetUnavailable:
			return "GpuBudgetUnavailable";
		case AnimationExecutionModeReason::ForceCpuRequested:
			return "ForceCpuRequested";
		case AnimationExecutionModeReason::CompletePoseRequired:
			return "CompletePoseRequired";
		case AnimationExecutionModeReason::ForceGpuUnsafe:
			return "ForceGpuUnsafe";
		case AnimationExecutionModeReason::ForceHybridMissingSubset:
			return "ForceHybridMissingSubset";
		case AnimationExecutionModeReason::ForceHybridRequested:
			return "ForceHybridRequested";
		case AnimationExecutionModeReason::AuthoritativeSubsetRequired:
			return "AuthoritativeSubsetRequired";
		case AnimationExecutionModeReason::ForceGpuRequested:
			return "ForceGpuRequested";
		case AnimationExecutionModeReason::GpuVisualEligible:
			return "GpuVisualEligible";
		case AnimationExecutionModeReason::PromotionHysteresis:
			return "PromotionHysteresis";
		case AnimationExecutionModeReason::MinimumResidence:
			return "MinimumResidence";
		case AnimationExecutionModeReason::InvalidRequestedPolicy:
			return "InvalidRequestedPolicy";
		case AnimationExecutionModeReason::InvalidPreviousState:
			return "InvalidPreviousState";
		default:
			return "Unknown";
		}
	}

	AnimationExecutionModeDecision ResolveAnimationExecutionMode(
		const AnimationExecutionModeRequest& request,
		const AnimationExecutionModeState& previousState)
	{
		AnimationExecutionModeDecision decision;
		decision.consumerClosure = ResolveConsumerClosure(request.consumers);
		SelectEligibleMode(request, decision);

		// Current request errors take precedence over a prior-state time comparison.
		// In particular, a negative current tick must keep its InvalidRequestedPolicy
		// diagnostic instead of being mislabeled as corruption of an older record.
		if(!IsValidRequestTimeline(request)) {
			decision.effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			decision.reason = AnimationExecutionModeReason::InvalidRequestedPolicy;
			decision.nextState = MakeState(
				decision.effectiveMode,
				decision.reason,
				std::max<AnimationFixedTick>(request.fixedTick, 0),
				request.continuity);
			decision.transition.from = decision.effectiveMode;
			decision.transition.to = decision.effectiveMode;
			decision.transition.continuity = request.continuity;
			return decision;
		}

		// An invalid prior record cannot safely identify a transition source. Start
		// a new CPU record and expose the error instead of guessing across possibly
		// corrupt mode/tick data.
		if(!IsValidPreviousState(previousState, request.fixedTick)) {
			decision.effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			decision.reason = AnimationExecutionModeReason::InvalidPreviousState;
			decision.nextState = MakeState(
				decision.effectiveMode,
				decision.reason,
				std::max<AnimationFixedTick>(request.fixedTick, 0),
				request.continuity);
			decision.transition.from = decision.effectiveMode;
			decision.transition.to = decision.effectiveMode;
			decision.transition.continuity = request.continuity;
			return decision;
		}

		if(!previousState.initialized) {
			decision.effectiveMode = decision.eligibleMode;
			decision.reason = decision.eligibilityReason;
			decision.nextState = MakeState(
				decision.effectiveMode,
				decision.reason,
				std::max<AnimationFixedTick>(request.fixedTick, 0),
				request.continuity);
			decision.transition.from = decision.effectiveMode;
			decision.transition.to = decision.effectiveMode;
			decision.transition.continuity = request.continuity;
			return decision;
		}

		decision.nextState = previousState;
		decision.nextState.continuity = request.continuity;
		decision.transition.from = previousState.effectiveMode;
		decision.transition.to = previousState.effectiveMode;
		decision.transition.continuity = request.continuity;

		if(decision.eligibleMode == previousState.effectiveMode) {
			decision.effectiveMode = previousState.effectiveMode;
			decision.reason = decision.eligibilityReason;
			decision.nextState.effectiveReason = decision.reason;
			decision.nextState.promotionPending = false;
			decision.nextState.promotionMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			decision.nextState.promotionReason = AnimationExecutionModeReason::Uninitialized;
			decision.nextState.promotionSinceTick = 0;
			decision.nextState.promotionFingerprint = 0U;
			return decision;
		}

		const bool isHardDowngrade = AuthorityRank(decision.eligibleMode)
			> AuthorityRank(previousState.effectiveMode);
		if(isHardDowngrade) {
			decision.effectiveMode = decision.eligibleMode;
			decision.reason = decision.eligibilityReason;
			decision.modeChanged = true;
			decision.transition.occurred = true;
			decision.transition.to = decision.effectiveMode;
			decision.nextState = MakeState(
				decision.effectiveMode,
				decision.reason,
				request.fixedTick,
				request.continuity);
			return decision;
		}

		// Moving toward less CPU authority is a promotion. A candidate must remain
		// identical for the hysteresis window, and the current mode must satisfy its
		// residence window. Both comparisons use caller-owned fixed ticks only.
		const std::uint64_t promotionFingerprint = MakePromotionFingerprint(decision, request.requestedPolicy);
		const bool sameCandidate = previousState.promotionPending
			&& previousState.promotionMode == decision.eligibleMode
			&& previousState.promotionReason == decision.eligibilityReason
			&& previousState.promotionFingerprint == promotionFingerprint;
		const AnimationFixedTick promotionSinceTick = sameCandidate
			? previousState.promotionSinceTick
			: request.fixedTick;
		const bool hysteresisPassed = HasElapsed(
			request.fixedTick,
			promotionSinceTick,
			request.stability.promotionHysteresisTicks);
		const bool residencePassed = HasElapsed(
			request.fixedTick,
			previousState.modeEnteredTick,
			request.stability.minimumResidenceTicks);

		if(!hysteresisPassed || !residencePassed) {
			decision.effectiveMode = previousState.effectiveMode;
			decision.reason = !hysteresisPassed
				? AnimationExecutionModeReason::PromotionHysteresis
				: AnimationExecutionModeReason::MinimumResidence;
			decision.nextState.effectiveReason = decision.reason;
			decision.nextState.promotionPending = true;
			decision.nextState.promotionMode = decision.eligibleMode;
			decision.nextState.promotionReason = decision.eligibilityReason;
			decision.nextState.promotionSinceTick = promotionSinceTick;
			decision.nextState.promotionFingerprint = promotionFingerprint;
			return decision;
		}

		decision.effectiveMode = decision.eligibleMode;
		decision.reason = decision.eligibilityReason;
		decision.modeChanged = true;
		decision.transition.occurred = true;
		decision.transition.to = decision.effectiveMode;
		decision.nextState = MakeState(
			decision.effectiveMode,
			decision.reason,
			request.fixedTick,
			request.continuity);
		return decision;
	}

	AnimationExecutionModeBatchResult ResolveAnimationExecutionModes(
		std::span<const AnimationExecutionModeBatchRequest> requests,
		std::vector<AnimationExecutionModeDecision>& output)
	{
		if(requests.empty()) {
			output.clear();
			return {};
		}
		if(requests.size() > kMaximumAnimationExecutionModeBatchSize) {
			// A hard-limit rejection is a failed publication, not an empty frame.
			// Keep the caller's previous complete output intact for atomicity.
			return { AnimationStatus::HardLimitExceeded, 0U };
		}

		// Stage the complete batch before publication. Allocation failure leaves the
		// caller's prior non-empty result intact rather than exposing a partial frame.
		try {
			std::vector<AnimationExecutionModeDecision> staged;
			staged.reserve(requests.size());
			for(const AnimationExecutionModeBatchRequest& entry : requests) {
				staged.push_back(ResolveAnimationExecutionMode(entry.request, entry.previousState));
			}
			output.swap(staged);
		} catch(const std::bad_alloc&) {
			return { AnimationStatus::InternalError, 0U };
		}
		return { AnimationStatus::Ok, requests.size() };
	}
}
