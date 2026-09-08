#include "NavigationTypes.h"

namespace TerraEngine::Navigation {
	std::string_view ToString(NavigationHandleStatus status) noexcept {
		switch(status) {
		case NavigationHandleStatus::Invalid: return "Invalid";
		case NavigationHandleStatus::InvalidOwner: return "InvalidOwner";
		case NavigationHandleStatus::Alive: return "Alive";
		case NavigationHandleStatus::Expired: return "Expired";
		case NavigationHandleStatus::Destroyed: return "Destroyed";
		case NavigationHandleStatus::StaleSlot: return "StaleSlot";
		case NavigationHandleStatus::StaleOwner: return "StaleOwner";
		case NavigationHandleStatus::StaleWorld: return "StaleWorld";
		case NavigationHandleStatus::Retired: return "Retired";
		case NavigationHandleStatus::CapacityExceeded: return "CapacityExceeded";
		}
		return "UnknownNavigationHandleStatus";
	}

	std::string_view ToString(NavigationRequestLifecycle lifecycle) noexcept {
		switch(lifecycle) {
		case NavigationRequestLifecycle::Queued: return "Queued";
		case NavigationRequestLifecycle::PendingPreparation: return "PendingPreparation";
		case NavigationRequestLifecycle::Searching: return "Searching";
		case NavigationRequestLifecycle::Terminal: return "Terminal";
		}
		return "UnknownNavigationRequestLifecycle";
	}

	std::string_view ToString(NavigationOutcome outcome) noexcept {
		switch(outcome) {
		case NavigationOutcome::Queued: return "Queued";
		case NavigationOutcome::PendingPreparation: return "PendingPreparation";
		case NavigationOutcome::Searching: return "Searching";
		case NavigationOutcome::Complete: return "Complete";
		case NavigationOutcome::Partial: return "Partial";
		case NavigationOutcome::Unreachable: return "Unreachable";
		case NavigationOutcome::InvalidRequest: return "InvalidRequest";
		case NavigationOutcome::InvalidEndpoint: return "InvalidEndpoint";
		case NavigationOutcome::UnavailableCoverage: return "UnavailableCoverage";
		case NavigationOutcome::BudgetExhausted: return "BudgetExhausted";
		case NavigationOutcome::RejectedCapacity: return "RejectedCapacity";
		case NavigationOutcome::Cancelled: return "Cancelled";
		case NavigationOutcome::Expired: return "Expired";
		case NavigationOutcome::Stale: return "Stale";
		case NavigationOutcome::Failed: return "Failed";
		}
		return "UnknownNavigationOutcome";
	}

	std::string_view ToString(NavigationReason reason) noexcept {
		switch(reason) {
		case NavigationReason::None: return "None";
		case NavigationReason::MissingSource: return "MissingSource";
		case NavigationReason::DirtyCoverage: return "DirtyCoverage";
		case NavigationReason::MissingNeighbor: return "MissingNeighbor";
		case NavigationReason::DomainLimit: return "DomainLimit";
		case NavigationReason::ResolutionLimit: return "ResolutionLimit";
		case NavigationReason::NodeLimit: return "NodeLimit";
		case NavigationReason::OutputLimit: return "OutputLimit";
		case NavigationReason::InvalidStart: return "InvalidStart";
		case NavigationReason::InvalidDestination: return "InvalidDestination";
		case NavigationReason::InsufficientClearance: return "InsufficientClearance";
		case NavigationReason::MissingSupport: return "MissingSupport";
		case NavigationReason::SupersededRequest: return "SupersededRequest";
		case NavigationReason::SceneReplaced: return "SceneReplaced";
		case NavigationReason::ProviderFailure: return "ProviderFailure";
		case NavigationReason::Cancelled: return "Cancelled";
		case NavigationReason::Expired: return "Expired";
		}
		return "UnknownNavigationReason";
	}

	NavigationLocationValidation ValidateNavigationLocation(const NavigationLocation& location) noexcept {
		if(!location.world.IsValid()) {
			return { false, NavigationLocationValidationCode::InvalidWorld };
		}

		const Scene::WorldCoordinateResult normalized = Scene::NormalizeWorldPosition(location.position);
		if(!normalized.Succeeded()) {
			switch(normalized.error) {
			case Scene::WorldCoordinateError::NonFiniteOffset:
				return { false, NavigationLocationValidationCode::NonFinitePosition };
			case Scene::WorldCoordinateError::CellOverflow:
				return { false, NavigationLocationValidationCode::CoordinateOverflow };
			case Scene::WorldCoordinateError::UnsafeLocalRange:
			case Scene::WorldCoordinateError::None:
				return { false, NavigationLocationValidationCode::UnnormalizedPosition };
			}
		}

		// Navigation accepts only canonical addresses. Silently fixing an input
		// here would make a request's stored endpoint differ from its audit log.
		if(normalized.position != location.position) {
			return { false, NavigationLocationValidationCode::UnnormalizedPosition };
		}
		return { true, NavigationLocationValidationCode::None };
	}

	std::string_view ToString(NavigationLocationValidationCode code) noexcept {
		switch(code) {
		case NavigationLocationValidationCode::None: return "None";
		case NavigationLocationValidationCode::InvalidWorld: return "InvalidWorld";
		case NavigationLocationValidationCode::NonFinitePosition: return "NonFinitePosition";
		case NavigationLocationValidationCode::CoordinateOverflow: return "CoordinateOverflow";
		case NavigationLocationValidationCode::UnnormalizedPosition: return "UnnormalizedPosition";
		}
		return "UnknownNavigationLocationValidationCode";
	}
}
