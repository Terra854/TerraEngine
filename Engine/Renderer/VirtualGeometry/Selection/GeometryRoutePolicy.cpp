// GeometryRoutePolicy.cpp - Deterministic route scoring and suppression proof.

#include "GeometryRoutePolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace TerraEngine::Renderer
{
	namespace
	{
		struct CandidateRef
		{
			GeometryRoute route = GeometryRoute::VirtualGeometryFallback;
			const GeometryRoutePolicyCandidateInput *input = nullptr;
		};

		constexpr std::array<GeometryRoute, 3u> kStableTieBreakOrder = {
			GeometryRoute::DirectMeshlet,
			GeometryRoute::VirtualGeometryHardware,
			GeometryRoute::VirtualGeometryCompute
		};

		const GeometryRoutePolicyCandidateInput *FindCandidate(
			const GeometryRoutePolicyInput &input,
			GeometryRoute route)
		{
			switch (route)
			{
			case GeometryRoute::DirectMeshlet: return &input.direct;
			case GeometryRoute::VirtualGeometryHardware: return &input.virtualGeometryHardware;
			case GeometryRoute::VirtualGeometryCompute: return &input.virtualGeometryCompute;
			case GeometryRoute::VirtualGeometryFallback: return nullptr;
			}
			return nullptr;
		}

		GeometryRoute FindBestEligible(const GeometryRoutePolicyInput &input)
		{
			GeometryRoute selected = GeometryRoute::VirtualGeometryFallback;
			float selectedCost = std::numeric_limits<float>::infinity();
			for (const GeometryRoute route : kStableTieBreakOrder)
			{
				const GeometryRoutePolicyCandidateInput *candidate = FindCandidate(input, route);
				if (candidate == nullptr || !candidate->IsEligible()
					|| !std::isfinite(candidate->estimatedCost)
					|| candidate->estimatedCost < 0.0F)
				{
					continue;
				}
				if (candidate->estimatedCost < selectedCost)
				{
					selected = route;
					selectedCost = candidate->estimatedCost;
				}
			}
			return selected;
		}

		std::vector<std::string> DescribeRejections(
			const GeometryRoutePolicyCandidateInput &candidate)
		{
			std::vector<std::string> reasons;
			if (!candidate.valid) reasons.emplace_back("candidate-invalid");
			if (!candidate.representationAvailable) reasons.emplace_back("representation-unavailable");
			if (!candidate.correctnessCompatible) reasons.emplace_back("correctness-incompatible");
			if (!candidate.materialCompatible) reasons.emplace_back("material-incompatible");
			if (!candidate.deformationCompatible) reasons.emplace_back("deformation-incompatible");
			if (!candidate.viewsCompatible) reasons.emplace_back("view-incompatible");
			if (!candidate.capabilitiesAvailable) reasons.emplace_back("capability-unavailable");
			if (!candidate.residencyUsable) reasons.emplace_back("residency-unusable");
			if (!std::isfinite(candidate.estimatedCost) || candidate.estimatedCost < 0.0F)
			{
				reasons.emplace_back("estimate-invalid");
			}
			return reasons;
		}

		GeometryRouteDecision SelectInternal(
			const GeometryRoutePolicyInput &input,
			const GeometryRoutePolicyState *previousState,
			bool includeCandidateDiagnostics)
		{
			GeometryRouteDecision decision{};
			decision.policyIdentity = input.policyIdentity;
			decision.calibrationIdentity = input.calibrationIdentity;
			decision.confidenceMargin = std::max(0.0F, input.confidenceMargin);

			if (includeCandidateDiagnostics)
			{
				decision.candidates.reserve(kStableTieBreakOrder.size());
				for (const GeometryRoute route : kStableTieBreakOrder)
				{
					const GeometryRoutePolicyCandidateInput &candidate = *FindCandidate(input, route);
					decision.candidates.push_back(GeometryRouteCandidate{
						route,
						candidate.IsEligible(),
						std::isfinite(candidate.estimatedCost)
							? std::optional<double>{candidate.estimatedCost}
							: std::nullopt,
						DescribeRejections(candidate)});
				}
			}

			const GeometryRoute automaticallySelected = FindBestEligible(input);
			GeometryRoute requested = automaticallySelected;
			bool exactAuthorOverride = false;
			if (input.authorOverride.has_value())
			{
				const GeometryRoutePolicyCandidateInput *overrideCandidate =
					FindCandidate(input, *input.authorOverride);
				if (overrideCandidate != nullptr && overrideCandidate->IsEligible())
				{
					requested = *input.authorOverride;
					decision.source = GeometryRouteSelectionSource::AuthorOverride;
					exactAuthorOverride = true;
				}
				else
				{
					// A route lock is a correctness constraint, never a preference
					// that can silently select a different representation.  Keep the
					// unavailable diagnostic observable and emit no replacement work.
					requested = GeometryRoute::VirtualGeometryFallback;
					decision.fallbackReason =
						GeometryRouteFallbackReason::AuthorOverrideUnavailable;
					decision.fallbackDiagnostic = "author-override-unavailable";
				}
			}

			if (requested == GeometryRoute::VirtualGeometryFallback)
			{
				decision.selected = GeometryRoute::VirtualGeometryFallback;
				if (decision.fallbackReason == GeometryRouteFallbackReason::None)
				{
					decision.fallbackReason =
						GeometryRouteFallbackReason::CapabilityUnavailable;
					decision.fallbackDiagnostic = "capability-unavailable";
					decision.source = GeometryRouteSelectionSource::CapabilityFallback;
				}
				decision.nextState.current = decision.selected;
				decision.nextState.stableFrames =
					previousState == nullptr ? 1u : previousState->stableFrames + 1u;
				decision.stableFrameCount = decision.nextState.stableFrames;
				return decision;
			}
			if (exactAuthorOverride)
			{
				// Locks must select their eligible representation in this frame;
				// hysteresis only stabilizes automatic calibration decisions.
				decision.selected = requested;
				decision.nextState.current = requested;
				decision.nextState.challenger = requested;
				decision.nextState.stableFrames =
					previousState == nullptr ? 1u : previousState->stableFrames + 1u;
				decision.nextState.challengerFrames = 0u;
				decision.stableFrameCount = decision.nextState.stableFrames;
				decision.estimatedCost = FindCandidate(input, requested)->estimatedCost;
				return decision;
			}

			GeometryRoute selected = requested;
			GeometryRoutePolicyState next =
				previousState == nullptr ? GeometryRoutePolicyState{} : *previousState;
			if (previousState == nullptr)
			{
				next.current = requested;
				next.challenger = requested;
				next.stableFrames = 1u;
				next.challengerFrames = 0u;
			}
			else
			{
				next.stableFrames = previousState->stableFrames + 1u;
				const GeometryRoutePolicyCandidateInput *currentCandidate =
					FindCandidate(input, previousState->current);
				const bool currentEligible =
					currentCandidate != nullptr && currentCandidate->IsEligible();
				if (!currentEligible)
				{
					next.current = requested;
					next.challenger = requested;
					next.challengerFrames = 0u;
				}
				else if (requested != previousState->current)
				{
					const GeometryRoutePolicyCandidateInput *challenger =
						FindCandidate(input, requested);
					const float margin = std::clamp(input.confidenceMargin, 0.0F, 0.95F);
					const bool clearsMargin = challenger != nullptr
						&& challenger->estimatedCost
							< currentCandidate->estimatedCost * (1.0F - margin);
					if (clearsMargin)
					{
						if (next.challenger == requested) ++next.challengerFrames;
						else
						{
							next.challenger = requested;
							next.challengerFrames = 1u;
						}
						if (next.challengerFrames
							>= std::max(1u, input.minimumStableFrames))
						{
							next.current = requested;
							next.challengerFrames = 0u;
						}
					}
					else
					{
						next.challenger = previousState->current;
						next.challengerFrames = 0u;
					}
				}
				else
				{
					next.challenger = requested;
					next.challengerFrames = 0u;
				}
				selected = next.current;
			}

			decision.selected = selected;
			decision.nextState = next;
			decision.stableFrameCount = next.stableFrames;
			if (const auto *selectedCandidate = FindCandidate(input, selected);
				selectedCandidate != nullptr && std::isfinite(selectedCandidate->estimatedCost))
			{
				decision.estimatedCost = selectedCandidate->estimatedCost;
			}
			return decision;
		}
	} // namespace

	bool GeometryRoutePolicyCandidateInput::IsEligible() const noexcept
	{
		return valid
			&& representationAvailable
			&& correctnessCompatible
			&& materialCompatible
			&& deformationCompatible
			&& viewsCompatible
			&& capabilitiesAvailable
			&& residencyUsable;
	}

	GeometryRouteDecision SelectGeometryRoute(const GeometryRoutePolicyInput &input)
	{
		return SelectInternal(input, nullptr, true);
	}

	GeometryRouteDecision SelectGeometryRoute(
		const GeometryRoutePolicyInput &input,
		const GeometryRoutePolicyState &previousState)
	{
		return SelectInternal(input, &previousState, true);
	}

	GeometryRouteDecision SelectGeometryRouteCompact(
		const GeometryRoutePolicyInput &input)
	{
		return SelectInternal(input, nullptr, false);
	}

	GeometryRouteDecision SelectGeometryRouteCompact(
		const GeometryRoutePolicyInput &input,
		const GeometryRoutePolicyState &previousState)
	{
		return SelectInternal(input, &previousState, false);
	}

	void BuildGeometryRouteFramePlan(GeometryRouteFramePlan &plan)
	{
		plan.vgHierarchyTraversalCount = 0u;
		plan.vgPageRequestCount = 0u;
		plan.vgDecodeCount = 0u;
		plan.vgResidencyPublicationCount = 0u;
		plan.vgCompactionCount = 0u;
		plan.generatedVirtualGeometryPacketCount = 0u;
		plan.vgExecutorAllocationCount = 0u;
		plan.directBatchCount = 0u;
		plan.sharedGeometryInstanceCount = 0u;

		if (plan.selectedRoute == GeometryRoute::DirectMeshlet)
		{
			plan.directBatchCount = plan.instances == 0u ? 0u : 1u;
			plan.sharedGeometryInstanceCount = plan.instances;
			plan.identityStableAcrossViews = true;
			return;
		}
		if (plan.selectedRoute == GeometryRoute::VirtualGeometryFallback)
		{
			// An unavailable explicit lock fails closed.  It may be diagnosed, but
			// it must not manufacture hierarchy, page, decode, or executor work.
			plan.identityStableAcrossViews = true;
			return;
		}

		// VG work is represented as one bounded frame plan here. Runtime stages
		// replace these counters with real candidate and execution counts.
		plan.vgHierarchyTraversalCount = 1u;
		plan.vgPageRequestCount = 1u;
		plan.vgDecodeCount = 1u;
		plan.vgResidencyPublicationCount = 1u;
		plan.vgCompactionCount = 1u;
		plan.generatedVirtualGeometryPacketCount = 1u;
		plan.vgExecutorAllocationCount = 1u;
		plan.identityStableAcrossViews = true;
	}
} // namespace TerraEngine::Renderer
