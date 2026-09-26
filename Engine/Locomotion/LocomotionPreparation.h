// LocomotionPreparation.h - copied inputs for independent movement worker shards.

#pragma once

#include "LocomotionTypes.h"
#include "LocomotionNavigationCompatibility.h"
#include "Navigation/Agents/NavigationAgentTypes.h"
#include "Physics/PhysicsWorld.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>

namespace TerraEngine::Locomotion {
	// Capture fills this at a protected boundary. A worker can transform direction
	// and physical feedback without reading ECS, PhysicsSystem or provider state.
	struct TERRAENGINE_API LocomotionPreparationInput {
		LocomotionRuntimeRecord runtime{};
		// A captured pending command may supply math inputs before Publish admits
		// ownership. Its result is advisory until protected authority succeeds.
		std::optional<MovementIntent> candidateIntent;
		// Capture copies authoritative ReadGuidance evidence and its geometry
		// result; workers never query an agent, controller or route provider.
		std::optional<Navigation::NavigationAgentGuidanceSnapshot> navigationGuidance;
		std::optional<NavigationCompatibilityResult> navigationCompatibility;
		double arrivalToleranceMetres = 0.10;
		Physics::PhysicsCharacterState physical{};
		Physics::PhysicsFrameHandle frame{};
		glm::dvec3 controllerUp{0.0, 1.0, 0.0};
		double acceleration = 20.0;
		double braking = 25.0;
		double turnRate = 6.283185;
		std::uint32_t facing = 1U; // Authored Movement; 0 holds the current yaw.
		std::uint64_t locomotionUpdateEpoch = 0U;
		std::uint64_t physicalMotionEpoch = 0U;
	};

	struct TERRAENGINE_API PreparedLocomotionMotion {
		LocomotionReason reason = LocomotionReason::None;
		Physics::CharacterMotionTarget target{};
		LocomotionObservation observation{};
		bool publishTarget = false;
	};

	// Pure worker-safe preparation. Physical authority and lease checks remain at
	// the protected publisher and Physics fixed-step ingress.
	[[nodiscard]] TERRAENGINE_API PreparedLocomotionMotion PrepareLocomotionMotion(
		const LocomotionPreparationInput& input) noexcept;
}
