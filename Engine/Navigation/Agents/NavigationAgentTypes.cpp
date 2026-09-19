// NavigationAgentTypes.cpp - value validation and diagnostic names for agents.

#include "NavigationAgentTypes.h"

#include <cmath>

namespace TerraEngine::Navigation {
	bool NavigationAgentGuidanceSnapshot::IsUsable() const noexcept {
		// Following is the only state allowed to offer movement input. Keep the
		// vector and distance checks here so every public reader gets the same
		// conservative result before performing a controller command.
		return usable && identity.IsValid() && state == NavigationAgentState::Following
			&& configurationRevision != 0U && intentSequence != 0U
			&& publicationGeneration != 0U && routeGeneration != 0U
			&& ValidateNavigationLocation(nextWaypoint).accepted
			&& nextWaypoint.world == identity.world
			&& observation.HasWorldEvidence()
			&& std::isfinite(suggestedDirection.x) && std::isfinite(suggestedDirection.y)
			&& std::isfinite(suggestedDirection.z) && std::isfinite(remainingDistanceMetres)
			&& remainingDistanceMetres >= 0.0
			// Direction is a unit advisory vector, not a speed or velocity. Reject
			// zero/oversized vectors rather than letting consumers infer authority.
			&& std::abs(suggestedDirection.x * suggestedDirection.x
				+ suggestedDirection.y * suggestedDirection.y
				+ suggestedDirection.z * suggestedDirection.z - 1.0) <= 1.0e-6;
	}

	bool NavigationAgentAuthoredSettings::Validate() const noexcept {
		return traversal.Validate().accepted
			&& std::isfinite(arrivalToleranceMetres) && arrivalToleranceMetres > 0.0
			&& std::isfinite(targetDisplacementThresholdMetres) && targetDisplacementThresholdMetres > 0.0
			&& std::isfinite(minimumReplanIntervalSeconds) && minimumReplanIntervalSeconds >= 0.10;
	}

	std::string_view ToString(NavigationAgentState state) noexcept {
		switch(state) {
		case NavigationAgentState::Idle: return "Idle";
		case NavigationAgentState::Requesting: return "Requesting";
		case NavigationAgentState::Following: return "Following";
		case NavigationAgentState::Arrived: return "Arrived";
		case NavigationAgentState::Stopped: return "Stopped";
		case NavigationAgentState::Failed: return "Failed";
		}
		return "UnknownNavigationAgentState";
	}

	std::string_view ToString(NavigationAgentCommandKind kind) noexcept {
		switch(kind) {
		case NavigationAgentCommandKind::SetLocationDestination: return "SetLocationDestination";
		case NavigationAgentCommandKind::SetObjectDestination: return "SetObjectDestination";
		case NavigationAgentCommandKind::Stop: return "Stop";
		}
		return "UnknownNavigationAgentCommandKind";
	}

	std::string_view ToString(NavigationAgentReceiptState state) noexcept {
		switch(state) {
		case NavigationAgentReceiptState::Queued: return "Queued";
		case NavigationAgentReceiptState::Applied: return "Applied";
		case NavigationAgentReceiptState::Rejected: return "Rejected";
		case NavigationAgentReceiptState::Superseded: return "Superseded";
		case NavigationAgentReceiptState::Expired: return "Expired";
		}
		return "UnknownNavigationAgentReceiptState";
	}
}
