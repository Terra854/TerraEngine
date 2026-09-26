// LocomotionPreparation.cpp - copied planar motion and completed-tick feedback.

#include "LocomotionPreparation.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Locomotion {
	namespace {
		constexpr double kDirectionEpsilonSquared = 1.0e-12;

		bool Finite(const glm::dvec3& value) noexcept {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		glm::dvec3 ToGlm(Physics::PhysicsVector3 value) noexcept {
			return {value.x, value.y, value.z};
		}

		Physics::PhysicsVector3 ToPhysics(const glm::dvec3& value) noexcept {
			return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
		}

		glm::dvec3 Planar(glm::dvec3 value, const glm::dvec3& up) noexcept {
			return value - glm::dot(value, up) * up;
		}

		glm::dvec3 NormalizeOrZero(const glm::dvec3& value) noexcept {
			// Scale first: squaring a large but finite direction can overflow while
			// its unit direction and bounded requested speed remain well-defined.
			const double largest = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
			if(largest <= std::sqrt(kDirectionEpsilonSquared)) return glm::dvec3(0.0);
			const glm::dvec3 scaled = value / largest;
			return scaled / std::sqrt(glm::dot(scaled, scaled));
		}

		Physics::CharacterMotionOwner ToOwner(const LocomotionHandle& handle) noexcept {
			return {handle.sceneLifetime, handle.objectId, handle.objectGeneration,
				handle.attachmentGeneration, handle.slotIndex, handle.slotGeneration};
		}
	}

	PreparedLocomotionMotion PrepareLocomotionMotion(
		const LocomotionPreparationInput& input) noexcept {
		PreparedLocomotionMotion result{};
		const auto& record = input.runtime;
		const auto& physical = input.physical;
		const auto& binding = record.bindings;
		const auto& authority = record.authority;
		auto& observation = result.observation;
		observation.handle = record.handle;
		observation.authority = authority;
		observation.bindingReason = record.bindingReason;
		observation.frame = input.frame;
		observation.physicalTick = physical.tick;
		observation.grounded = physical.supported
			&& physical.groundState == Physics::PhysicsCharacterGroundState::OnGround;

		// A copied packet must still address the exact bound controller attachment.
		// Workers never read live ECS or Physics to repair a stale generation.
		if(!record.handle.IsValid() || !binding.controllerSupported
			|| !binding.controller.world.IsValid() || !binding.controller.character.IsValid()
			|| physical.world != binding.controller.world
			|| physical.handle != binding.controller.character
			|| physical.objectId != record.handle.objectId
			|| physical.objectGeneration != record.handle.objectGeneration
			|| physical.componentGeneration != binding.controller.componentGeneration) {
			result.reason = LocomotionReason::MissingBinding;
			return result;
		}
		if(!input.frame.IsValid() || input.frame != binding.frame) {
			result.reason = LocomotionReason::UnsupportedFrame;
			return result;
		}
		if(!Finite(input.controllerUp) || !std::isfinite(input.acceleration)
			|| !std::isfinite(input.braking) || !std::isfinite(input.turnRate)
			|| input.acceleration <= 0.0 || input.acceleration > 1000.0
			|| input.braking <= 0.0 || input.braking > 1000.0
			|| input.turnRate < 0.0 || input.turnRate > 100.0
			|| !std::isfinite(binding.maximumSpeed)
			|| binding.maximumSpeed < 0.0 || binding.maximumSpeed > 100.0
			|| input.facing > 1U || !physical.pose.IsFinite()) {
			result.reason = LocomotionReason::InvalidArgument;
			return result;
		}
		const double upLengthSquared = glm::dot(input.controllerUp, input.controllerUp);
		const auto& rotation = physical.pose.rotation;
		const double rotationLengthSquared = static_cast<double>(rotation.x) * rotation.x
			+ static_cast<double>(rotation.y) * rotation.y
			+ static_cast<double>(rotation.z) * rotation.z
			+ static_cast<double>(rotation.w) * rotation.w;
		if(upLengthSquared <= kDirectionEpsilonSquared
			|| rotationLengthSquared <= kDirectionEpsilonSquared) {
			result.reason = LocomotionReason::InvalidArgument;
			return result;
		}
		const glm::dvec3 up = input.controllerUp / std::sqrt(upLengthSquared);
		const glm::dquat orientation = glm::normalize(glm::dquat(
			rotation.w, rotation.x, rotation.y, rotation.z));

		// Pending intent gives a shard arithmetic to prepare, not lease authority.
		const std::optional<MovementIntent>& intent = input.candidateIntent
			? input.candidateIntent : record.intent;
		glm::dvec3 desiredVelocity(0.0);
		double approachBudget = -1.0;
		const bool expired = !input.candidateIntent && intent
			&& intent->lifetime == IntentLifetime::OneUpdate
			&& intent->acceptedUpdateEpoch < input.locomotionUpdateEpoch;
		if(intent && !expired
			&& (input.candidateIntent || authority.state != AuthorityState::Stopped)) {
			if(static_cast<unsigned>(intent->kind) > static_cast<unsigned>(IntentKind::Navigation)
				|| static_cast<unsigned>(intent->lifetime) > static_cast<unsigned>(IntentLifetime::UntilStopped)
				|| (intent->kind == IntentKind::Player
					&& intent->lifetime != IntentLifetime::OneUpdate)
				|| !Finite(intent->direction) || !std::isfinite(intent->requestedSpeed)
				|| intent->requestedSpeed < 0.0) {
				result.reason = LocomotionReason::InvalidArgument;
				return result;
			}
			if(intent->kind == IntentKind::Navigation) {
				// Navigation advice is the sole route direction. FollowNavigation
				// carries no duplicate waypoint/path state in its command payload.
				const auto& guidance = input.navigationGuidance;
				if(guidance) {
					observation.navigationOutcome = guidance->outcome;
					observation.navigationAgentReason = guidance->reason;
				}
				if(!input.navigationCompatibility) {
					observation.navigationState = NavigationMotionState::Unavailable;
					observation.navigationReason = LocomotionReason::MissingBinding;
					result.reason = LocomotionReason::MissingBinding;
				} else if(!input.navigationCompatibility->compatible) {
					const auto& compatibility = *input.navigationCompatibility;
					observation.navigationState = NavigationMotionState::Failed;
					observation.navigationReason = compatibility.reason;
					observation.navigationCompatibilityField = compatibility.field;
					observation.navigationControllerValue = compatibility.controllerValue;
					observation.navigationProfileValue = compatibility.profileValue;
					result.reason = compatibility.reason;
				} else if(!guidance) {
					observation.navigationState = NavigationMotionState::Waiting;
					observation.navigationReason = LocomotionReason::MissingBinding;
					result.reason = LocomotionReason::MissingBinding;
				} else if(guidance->state == Navigation::NavigationAgentState::Arrived) {
					// The agent's observed arrival is authoritative; a zero target or
					// depleted budget alone cannot establish arrival through a wall.
					observation.navigationState = NavigationMotionState::Arrived;
					approachBudget = 0.0;
				} else if(guidance->state == Navigation::NavigationAgentState::Failed) {
					observation.navigationState = NavigationMotionState::Failed;
					observation.navigationReason = LocomotionReason::MissingBinding;
					result.reason = LocomotionReason::MissingBinding;
				} else if(!guidance->IsUsable()) {
					observation.navigationState = NavigationMotionState::Waiting;
					observation.navigationReason = LocomotionReason::MissingBinding;
					result.reason = LocomotionReason::MissingBinding;
				} else if(!std::isfinite(input.arrivalToleranceMetres)
					|| input.arrivalToleranceMetres < 0.0) {
					result.reason = LocomotionReason::InvalidArgument;
					return result;
				} else {
					observation.navigationState = NavigationMotionState::Following;
					observation.remainingRouteDistanceMetres = guidance->remainingDistanceMetres;
					approachBudget = std::max(0.0,
						guidance->remainingDistanceMetres - input.arrivalToleranceMetres);
					const double brakingCap = std::sqrt(2.0 * input.braking * approachBudget);
					const double speed = std::min({intent->requestedSpeed,
						binding.maximumSpeed, brakingCap});
					desiredVelocity = NormalizeOrZero(Planar(guidance->suggestedDirection, up)) * speed;
				}
			} else {
				glm::dvec3 direction = NormalizeOrZero(intent->direction);
				if(intent->directionSpace == DirectionSpace::ControllerLocal)
					direction = orientation * direction;
				else if(intent->directionSpace != DirectionSpace::World) {
					result.reason = LocomotionReason::InvalidArgument;
					return result;
				}
				const double speed = std::min(intent->requestedSpeed, binding.maximumSpeed);
				desiredVelocity = NormalizeOrZero(Planar(direction, up)) * speed;
			}
		}
		observation.desiredVelocity = desiredVelocity;

		// Requested and realized displacement belong to this completed physical
		// tick; zero duration or a teleport suppresses derivative history.
		const double duration = physical.completedStepDuration;
		if(!physical.motionDiscontinuity && std::isfinite(duration) && duration > 0.0
			&& physical.realizedDisplacement.IsFinite()
			&& physical.requestedPlanarVelocity.IsFinite()) {
			const glm::dvec3 realized = Planar(ToGlm(physical.realizedDisplacement), up);
			const glm::dvec3 requested = Planar(ToGlm(physical.requestedPlanarVelocity), up);
			const double speed = std::sqrt(glm::dot(realized, realized)) / duration;
			if(std::isfinite(speed)) {
				observation.motionHistoryAvailable = true;
				observation.observationDurationSeconds = duration;
				observation.observedPlanarSpeed = speed;
				observation.observedLocalDirection = NormalizeOrZero(
					glm::inverse(orientation) * NormalizeOrZero(realized));
				observation.requestedTranslation = requested * duration;
				observation.realizedTranslation = realized;
				observation.requestedYawRadians = physical.requestedYawRadians;
				observation.realizedYawRadians = physical.realizedYawRadians;
				if(observation.navigationState == NavigationMotionState::Following) {
					const double previousRequestSpeed = std::sqrt(glm::dot(requested, requested));
					if(previousRequestSpeed > 0.05 && speed < previousRequestSpeed * 0.1)
						observation.navigationState = NavigationMotionState::Blocked;
				}
			}
		}

		auto& target = result.target;
		target.controller = binding.controller;
		target.owner = ToOwner(record.handle);
		target.authorityEpoch = authority.authorityEpoch;
		target.motionEpoch = input.physicalMotionEpoch;
		target.locomotionUpdateEpoch = input.locomotionUpdateEpoch;
		target.mode = authority.mode == MotionMode::RootMotion
			? Physics::CharacterMotionMode::RootMotion : Physics::CharacterMotionMode::Velocity;
		target.frame = input.frame;
		target.planarVelocity = ToPhysics(desiredVelocity);
		if(approachBudget >= 0.0)
			target.approachDistanceMetres = static_cast<float>(
				std::min(approachBudget, static_cast<double>(std::numeric_limits<float>::max())));
		target.acceleration = static_cast<float>(input.acceleration);
		target.braking = static_cast<float>(input.braking);
		target.turnRate = static_cast<float>(input.turnRate);
		target.clearImmediately = authority.state == AuthorityState::Stopped
			&& !input.candidateIntent;
		const glm::dvec3 facing = NormalizeOrZero(desiredVelocity);
		target.hasFacing = input.facing == 1U && !target.clearImmediately
			&& glm::dot(facing, facing) > kDirectionEpsilonSquared
			&& authority.mode == MotionMode::Velocity;
		if(target.hasFacing) target.desiredFacing = ToPhysics(facing);
		// Root intervals are a separate story; do not invent Velocity motion.
		result.publishTarget = !input.candidateIntent && authority.ownerToken != 0U
			&& (authority.state == AuthorityState::Controlled
				|| authority.state == AuthorityState::Stopped)
			&& authority.mode == MotionMode::Velocity;
		return result;
	}
}
