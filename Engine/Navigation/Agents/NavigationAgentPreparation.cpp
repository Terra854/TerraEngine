#include "NavigationAgentPreparation.h"
#include "Scene/WorldCoordinate.h"
#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Navigation {
	namespace {
		// Localize before converting cell addresses to metres, avoiding signed-cell
		// overflow and loss of precision at large absolute world coordinates.
		std::optional<glm::dvec3> Delta(const NavigationLocation& from, const NavigationLocation& to) {
			if(from.world != to.world || !ValidateNavigationLocation(from).accepted
				|| !ValidateNavigationLocation(to).accepted) return {};
			const auto local = Scene::LocalizeWorldPosition(to.position, from.position, 1.0e9);
			return local.Succeeded() ? std::optional<glm::dvec3>{local.offsetMetres} : std::nullopt;
		}
	}

	NavigationAgentCandidate PrepareNavigationAgent(const NavigationAgentCapture& capture) {
		NavigationAgentCandidate candidate{capture.runtime, capture.waypointIndex};
		auto& runtime = candidate.runtime;
		runtime.guidance = {}; // Every early return is explicitly movement-neutral.
		if(runtime.intentSequence == 0U) {
			runtime.state = NavigationAgentState::Idle;
			return candidate;
		}
		if(runtime.state == NavigationAgentState::Stopped || runtime.state == NavigationAgentState::Failed) return candidate;
		const auto fail = [&](NavigationOutcome outcome, NavigationReason reason) {
			runtime.state = NavigationAgentState::Failed;
			runtime.outcome = outcome;
			runtime.reason = reason;
		};
		const auto replan = [&] {
			runtime.state = NavigationAgentState::Requesting;
			candidate.requestReplan = true;
		};
		if(runtime.destination.kind == NavigationDestinationKind::Object && !capture.target) {
			fail(NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination);
			return candidate;
		}
		const NavigationLocation destination = runtime.destination.kind == NavigationDestinationKind::Object
			? capture.target->location : runtime.destination.location;
		const auto requestedDelta = Delta(capture.owner.location, destination);
		if(!runtime.settings.Validate() || !requestedDelta) {
			fail(NavigationOutcome::InvalidRequest, NavigationReason::InvalidDestination);
			return candidate;
		}
		// A completed location destination stays complete after releasing its route.
		// Moving object targets restart only after leaving the arrival tolerance.
		if(runtime.state == NavigationAgentState::Arrived
			&& glm::length(*requestedDelta) <= runtime.settings.arrivalToleranceMetres) return candidate;
		if(!capture.routeUsable || capture.routePoints.empty() || !capture.observation.HasWorldEvidence()
			|| capture.routeGeneration == 0U || capture.publicationGeneration == 0U) {
			replan();
			return candidate;
		}
		const auto endpointDrift = Delta(capture.routePoints.back(), destination);
		if(!endpointDrift) {
			fail(NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination);
			return candidate;
		}
		if(runtime.destination.kind == NavigationDestinationKind::Object
			&& glm::length(*endpointDrift) > runtime.settings.targetDisplacementThresholdMetres) {
			replan();
			return candidate;
		}
		std::size_t waypoint = std::min(capture.waypointIndex, capture.routePoints.size() - 1U);
		auto nextDelta = Delta(capture.owner.location, capture.routePoints[waypoint]);
		if(!nextDelta) {
			fail(NavigationOutcome::InvalidEndpoint, NavigationReason::DomainLimit);
			return candidate;
		}
		while(waypoint + 1U < capture.routePoints.size()
			&& glm::length(*nextDelta) <= runtime.settings.arrivalToleranceMetres) {
			++waypoint;
			nextDelta = Delta(capture.owner.location, capture.routePoints[waypoint]);
			if(!nextDelta) {
				fail(NavigationOutcome::InvalidEndpoint, NavigationReason::DomainLimit);
				return candidate;
			}
		}
		candidate.nextWaypointIndex = waypoint;
		const double distance = glm::length(*nextDelta);
		// Reaching an intermediate corner or a vertically separated destination
		// cannot produce arrival. Both final support and requested location agree.
		if(waypoint + 1U == capture.routePoints.size() && distance <= runtime.settings.arrivalToleranceMetres
			&& glm::length(*requestedDelta) <= runtime.settings.arrivalToleranceMetres) {
			runtime.state = NavigationAgentState::Arrived;
			runtime.outcome = NavigationOutcome::Complete;
			runtime.reason = NavigationReason::None;
			return candidate;
		}
		if(distance <= std::numeric_limits<double>::epsilon()) {
			// A projected endpoint outside the arrival tolerance is not success.
			fail(NavigationOutcome::Unreachable, NavigationReason::MissingSupport);
			return candidate;
		}
		double remaining = distance;
		for(std::size_t index = waypoint + 1U; index < capture.routePoints.size(); ++index) {
			const auto segment = Delta(capture.routePoints[index - 1U], capture.routePoints[index]);
			if(!segment) {
				fail(NavigationOutcome::InvalidEndpoint, NavigationReason::DomainLimit);
				return candidate;
			}
			remaining += glm::length(*segment);
		}
		runtime.state = NavigationAgentState::Following;
		runtime.outcome = NavigationOutcome::Complete;
		runtime.reason = NavigationReason::None;
		auto& guidance = runtime.guidance;
		guidance.identity = runtime.identity;
		guidance.configurationRevision = runtime.configurationRevision;
		guidance.intentSequence = runtime.intentSequence;
		guidance.publicationGeneration = capture.publicationGeneration;
		guidance.routeGeneration = capture.routeGeneration;
		guidance.observation = capture.observation;
		guidance.state = runtime.state;
		guidance.outcome = runtime.outcome;
		guidance.destination = runtime.destination;
		guidance.nextWaypoint = capture.routePoints[waypoint];
		guidance.remainingDistanceMetres = remaining;
		guidance.suggestedDirection = *nextDelta / distance;
		guidance.usable = true;
		return candidate;
	}
}
