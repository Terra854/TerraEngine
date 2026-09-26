// LocomotionInspectorSupport.cpp - detached authoring and copied runtime display.

#include "LocomotionInspectorSupport.h"

#include <array>
#include <cmath>
#include <utility>

namespace TerraEngine::Editor::Locomotion {
	namespace {
		LocomotionAuthoringValidation Reject(std::string field, std::string explanation) {
			return {false, std::move(field), std::move(explanation)};
		}

		std::string_view AuthorityName(TerraEngine::Locomotion::AuthorityState state) {
			using TerraEngine::Locomotion::AuthorityState;
			switch (state) {
			case AuthorityState::Idle: return "Idle";
			case AuthorityState::Controlled: return "Controlled";
			case AuthorityState::Stopped: return "Stopped";
			case AuthorityState::Suspended: return "Suspended";
			}
			return "Unknown";
		}

		std::string_view ReasonName(TerraEngine::Locomotion::LocomotionReason reason) {
			using TerraEngine::Locomotion::LocomotionReason;
			switch (reason) {
			case LocomotionReason::None: return "Ready";
			case LocomotionReason::InvalidArgument: return "Invalid argument";
			case LocomotionReason::StaleHandle: return "Stale handle";
			case LocomotionReason::StaleConfiguration: return "Stale configuration";
			case LocomotionReason::OwnershipConflict: return "Ownership conflict";
			case LocomotionReason::UnsupportedController: return "Unsupported controller";
			case LocomotionReason::UnsupportedFrame: return "Unsupported frame";
			case LocomotionReason::MissingBinding: return "Missing binding";
			case LocomotionReason::AnimationUnavailable: return "Animation unavailable";
			case LocomotionReason::CapacityExceeded: return "Capacity exceeded";
			case LocomotionReason::NotPlaying: return "Not playing";
			case LocomotionReason::ExpiredReceipt: return "Expired receipt";
			case LocomotionReason::SequenceConflict: return "Sequence conflict";
			case LocomotionReason::Stopped: return "Stopped";
			case LocomotionReason::Revoked: return "Revoked";
			case LocomotionReason::ExternalExclusive: return "External exclusive";
			}
			return "Unknown";
		}

		std::string_view NavigationName(TerraEngine::Locomotion::NavigationMotionState state) {
			using TerraEngine::Locomotion::NavigationMotionState;
			switch (state) {
			case NavigationMotionState::Unavailable: return "Unavailable";
			case NavigationMotionState::Waiting: return "Waiting";
			case NavigationMotionState::Following: return "Following";
			case NavigationMotionState::Blocked: return "Blocked";
			case NavigationMotionState::Arrived: return "Arrived";
			case NavigationMotionState::Failed: return "Failed";
			}
			return "Unknown";
		}
	}

	LocomotionAuthoringValidation ValidateCharacterLocomotionDraft(const CharacterLocomotionComponent& value) {
		// Validate the detached candidate completely before the editor enqueues any
		// live mutation. These ordered checks name the field beside the error.
		if (value.controllerBinding != CharacterLocomotionComponent::kSelfBinding)
			return Reject("controllerBinding", "The controller must be bound to Self.");
		if (value.navigationBinding > CharacterLocomotionComponent::kSelfBinding)
			return Reject("navigationBinding", "Navigation must be None or Self.");
		if (value.animatorBinding > CharacterLocomotionComponent::kDescendantBinding)
			return Reject("animatorBinding", "Animator must be None, Self, or Descendant.");
		if ((value.animatorBinding == CharacterLocomotionComponent::kDescendantBinding)
			!= (value.animatorObjectId != TerraEngine::EMPTY_UUID))
			return Reject("animatorObjectId", "Only Descendant may name an animator UUID.");
		if (!std::isfinite(value.maximumSpeed) || value.maximumSpeed < 0.0 || value.maximumSpeed > 100.0)
			return Reject("maximumSpeed", "Maximum speed must be finite and within [0, 100] m/s.");
		if (!std::isfinite(value.acceleration) || value.acceleration <= 0.0 || value.acceleration > 1000.0)
			return Reject("acceleration", "Acceleration must be finite and within (0, 1000] m/s².");
		if (!std::isfinite(value.braking) || value.braking <= 0.0 || value.braking > 1000.0)
			return Reject("braking", "Braking must be finite and within (0, 1000] m/s².");
		if (!std::isfinite(value.turnRate) || value.turnRate < 0.0 || value.turnRate > 100.0)
			return Reject("turnRate", "Turn rate must be finite and within [0, 100] rad/s.");
		if (value.facing > CharacterLocomotionComponent::kFacingMovement)
			return Reject("facing", "Facing must be Hold or Movement.");
		if (value.initialMotionMode > CharacterLocomotionComponent::kRootMotionMode)
			return Reject("initialMotionMode", "Mode must be Velocity or Root Motion.");
		const std::array<std::pair<std::string_view, std::string_view>, 5U> names{{
			{"speedParameter", value.speedParameter}, {"localDirectionXParameter", value.localDirectionXParameter},
			{"localDirectionZParameter", value.localDirectionZParameter}, {"groundedParameter", value.groundedParameter},
			{"movingParameter", value.movingParameter}}};
		for (std::size_t index = 0U; index < names.size(); ++index) {
			// Let the component's UTF-8 rule remain the sole byte-level authority.
			CharacterLocomotionComponent oneName{};
			oneName.speedParameter = names[index].second;
			if (!oneName.ValidateAuthoredValues())
				return Reject(std::string(names[index].first), "Name must be valid UTF-8, NUL-free, and at most 128 bytes.");
			for (std::size_t other = 0U; other < index; ++other)
				if (!names[index].second.empty() && names[index].second == names[other].second)
					return Reject(std::string(names[index].first), "Parameter names must be distinct.");
		}
		return value.ValidateAuthoredValues() ? LocomotionAuthoringValidation{true, {}, {}}
			: Reject("component", "Character locomotion authoring is invalid.");
	}

	LocomotionRuntimeDisplay DescribeLocomotionRuntime(
		const TerraEngine::Locomotion::LocomotionObservation& observation) {
		LocomotionRuntimeDisplay display;
		display.authority = AuthorityName(observation.authority.state);
		display.mode = observation.authority.mode == TerraEngine::Locomotion::MotionMode::RootMotion
			? "Root Motion" : "Velocity";
		display.binding = ReasonName(observation.bindingReason);
		display.navigation = NavigationName(observation.navigationState);
		if (observation.navigationReason != TerraEngine::Locomotion::LocomotionReason::None)
			display.navigation += " / " + std::string(ReasonName(observation.navigationReason));
		display.animation = ReasonName(observation.animationReason);
		display.desiredSpeed = std::hypot(observation.desiredVelocity.x, observation.desiredVelocity.z);
		display.realizedSpeed = observation.observedPlanarSpeed;
		display.physicalTick = observation.physicalTick;
		return display;
	}
}
