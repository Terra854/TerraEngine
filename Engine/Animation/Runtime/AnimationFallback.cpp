// AnimationFallback.cpp - Deterministic safe fallback policy resolution.

#include "Animation/Runtime/AnimationFallback.h"

namespace TerraEngine::Animation {
	namespace {
		[[nodiscard]] bool IsValidPolicy(AnimationFallbackPolicy policy) noexcept
		{
			return policy >= AnimationFallbackPolicy::Disabled
				&& policy <= AnimationFallbackPolicy::CpuThenBindPose;
		}

		[[nodiscard]] bool IsValidReason(AnimationFallbackReason reason) noexcept
		{
			return reason >= AnimationFallbackReason::None
				&& reason <= AnimationFallbackReason::InternalFailure;
		}

		[[nodiscard]] bool IsExactUsableCandidate(
			const AnimationFallbackAvailability& candidate,
			const AnimationFallbackGenerationKey& expected) noexcept
		{
			return candidate.available
				&& candidate.complete
				&& candidate.finite
				&& candidate.generations.IsValid()
				&& candidate.generations == expected;
		}

		[[nodiscard]] bool IsUsablePrior(
			const AnimationFallbackAvailability& prior,
			const AnimationFallbackGenerationKey& expected) noexcept
		{
			return prior.available
				&& prior.complete
				&& prior.finite
				&& prior.generations.IsValid()
				&& prior.generations.IsHoldCompatibleWith(expected);
		}

		[[nodiscard]] AnimationFallbackDecision MakeDegradedDecision(
			const AnimationFallbackRequest& request,
			AnimationFallbackSource source,
			const AnimationFallbackGenerationKey& selected,
			bool preservesPrior,
			bool publishesNew) noexcept
		{
			AnimationFallbackDecision decision;
			decision.status = AnimationFallbackDecisionStatus::Degraded;
			decision.source = source;
			decision.reason = request.reason;
			decision.selectedGenerations = selected;
			decision.preservesPriorGeneration = preservesPrior;
			decision.publishesNewGeneration = publishesNew;
			return decision;
		}
	}

	bool AnimationFallbackGenerationKey::IsValid() const noexcept
	{
		// Renderer and physics generations may be zero in headless or non-physics
		// execution. The owner, lifecycle, content, and publication identities may not.
		return animator.IsValid()
			&& componentGeneration != 0U
			&& sceneGeneration != 0U
			&& contentGeneration != 0U
			&& publicationGeneration != 0U
			&& compatibilityFingerprint != 0U;
	}

	bool AnimationFallbackGenerationKey::HasSameLifecycle(
		const AnimationFallbackGenerationKey& other) const noexcept
	{
		return animator == other.animator
			&& componentGeneration == other.componentGeneration
			&& sceneGeneration == other.sceneGeneration;
	}

	bool AnimationFallbackGenerationKey::IsHoldCompatibleWith(
		const AnimationFallbackGenerationKey& expected) const noexcept
	{
		return IsValid()
			&& expected.IsValid()
			&& HasSameLifecycle(expected)
			&& compatibilityFingerprint == expected.compatibilityFingerprint;
	}

	AnimationFallbackDecision SelectAnimationFallback(
		const AnimationFallbackRequest& request) noexcept
	{
		AnimationFallbackDecision invalid;
		if(!IsValidPolicy(request.policy)
			|| !IsValidReason(request.reason)
			|| !request.expectedGenerations.IsValid()) {
			return invalid;
		}

		if(IsExactUsableCandidate(request.current, request.expectedGenerations)) {
			AnimationFallbackDecision ready;
			ready.status = AnimationFallbackDecisionStatus::Ready;
			ready.source = AnimationFallbackSource::Current;
			ready.reason = AnimationFallbackReason::None;
			ready.selectedGenerations = request.current.generations;
			ready.publishesNewGeneration = true;
			return ready;
		}

		// A missing current result without a cause would hide an integration error.
		// Fail closed rather than inventing a silent fallback diagnostic.
		if(request.reason == AnimationFallbackReason::None) {
			return invalid;
		}

		const bool cpuUsable = IsExactUsableCandidate(
			request.cpuFallback,
			request.expectedGenerations);
		const bool priorUsable = IsUsablePrior(request.prior, request.expectedGenerations);
		switch(request.policy) {
		case AnimationFallbackPolicy::Disabled:
			return MakeDegradedDecision(
				request,
				AnimationFallbackSource::Disabled,
				{},
				false,
				false);
		case AnimationFallbackPolicy::BindPose:
			break;
		case AnimationFallbackPolicy::HoldLastCompatible:
			if(priorUsable) {
				return MakeDegradedDecision(
					request,
					AnimationFallbackSource::HeldPriorGeneration,
					request.prior.generations,
					true,
					false);
			}
			break;
		case AnimationFallbackPolicy::CpuThenHoldLast:
			if(cpuUsable) {
				return MakeDegradedDecision(
					request,
					AnimationFallbackSource::CpuDeterministic,
					request.cpuFallback.generations,
					false,
					true);
			}
			if(priorUsable) {
				return MakeDegradedDecision(
					request,
					AnimationFallbackSource::HeldPriorGeneration,
					request.prior.generations,
					true,
					false);
			}
			break;
		case AnimationFallbackPolicy::CpuThenBindPose:
			if(cpuUsable) {
				return MakeDegradedDecision(
					request,
					AnimationFallbackSource::CpuDeterministic,
					request.cpuFallback.generations,
					false,
					true);
			}
			break;
		}

		if(request.bindPoseAvailable) {
			return MakeDegradedDecision(
				request,
				AnimationFallbackSource::BindPose,
				request.expectedGenerations,
				false,
				true);
		}

		AnimationFallbackDecision unavailable;
		unavailable.status = AnimationFallbackDecisionStatus::Unavailable;
		unavailable.source = AnimationFallbackSource::None;
		unavailable.reason = request.reason;
		return unavailable;
	}

	std::string_view ToString(AnimationFallbackPolicy policy) noexcept
	{
		switch(policy) {
		case AnimationFallbackPolicy::Disabled: return "Disabled";
		case AnimationFallbackPolicy::BindPose: return "BindPose";
		case AnimationFallbackPolicy::HoldLastCompatible: return "HoldLastCompatible";
		case AnimationFallbackPolicy::CpuThenHoldLast: return "CpuThenHoldLast";
		case AnimationFallbackPolicy::CpuThenBindPose: return "CpuThenBindPose";
		}
		return "InvalidAnimationFallbackPolicy";
	}

	std::string_view ToString(AnimationFallbackReason reason) noexcept
	{
		switch(reason) {
		case AnimationFallbackReason::None: return "None";
		case AnimationFallbackReason::MissingAsset: return "MissingAsset";
		case AnimationFallbackReason::IncompatibleAsset: return "IncompatibleAsset";
		case AnimationFallbackReason::AllocationFailure: return "AllocationFailure";
		case AnimationFallbackReason::BudgetPressure: return "BudgetPressure";
		case AnimationFallbackReason::Cancelled: return "Cancelled";
		case AnimationFallbackReason::StaleGeneration: return "StaleGeneration";
		case AnimationFallbackReason::RendererUnavailable: return "RendererUnavailable";
		case AnimationFallbackReason::RendererLost: return "RendererLost";
		case AnimationFallbackReason::DeviceLost: return "DeviceLost";
		case AnimationFallbackReason::PhysicsUnavailable: return "PhysicsUnavailable";
		case AnimationFallbackReason::ValidationFailure: return "ValidationFailure";
		case AnimationFallbackReason::InternalFailure: return "InternalFailure";
		}
		return "InvalidAnimationFallbackReason";
	}

	std::string_view ToString(AnimationFallbackSource source) noexcept
	{
		switch(source) {
		case AnimationFallbackSource::None: return "None";
		case AnimationFallbackSource::Current: return "Current";
		case AnimationFallbackSource::CpuDeterministic: return "CpuDeterministic";
		case AnimationFallbackSource::HeldPriorGeneration: return "HeldPriorGeneration";
		case AnimationFallbackSource::BindPose: return "BindPose";
		case AnimationFallbackSource::Disabled: return "Disabled";
		}
		return "InvalidAnimationFallbackSource";
	}

	std::string_view ToString(AnimationFallbackDecisionStatus status) noexcept
	{
		switch(status) {
		case AnimationFallbackDecisionStatus::Ready: return "Ready";
		case AnimationFallbackDecisionStatus::Degraded: return "Degraded";
		case AnimationFallbackDecisionStatus::Unavailable: return "Unavailable";
		case AnimationFallbackDecisionStatus::InvalidArgument: return "InvalidArgument";
		}
		return "InvalidAnimationFallbackDecisionStatus";
	}

	std::string_view ToString(AnimationFallbackPublishStatus status) noexcept
	{
		switch(status) {
		case AnimationFallbackPublishStatus::Published: return "Published";
		case AnimationFallbackPublishStatus::AlreadyPublished: return "AlreadyPublished";
		case AnimationFallbackPublishStatus::RejectedInvalid: return "RejectedInvalid";
		case AnimationFallbackPublishStatus::RejectedUnexpectedGeneration:
			return "RejectedUnexpectedGeneration";
		case AnimationFallbackPublishStatus::RejectedStale: return "RejectedStale";
		}
		return "InvalidAnimationFallbackPublishStatus";
	}
}
