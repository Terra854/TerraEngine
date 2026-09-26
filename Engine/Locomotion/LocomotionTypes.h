// LocomotionTypes.h - copied runtime identities, authority and observations.

#pragma once

#include "Core/Export.h"
#include "Navigation/NavigationTypes.h"
#include "Physics/PhysicsCommands.h"
#include "Utilities/UUID.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Locomotion {
	inline constexpr std::size_t kMaximumLiveSlots = 4096U;
	inline constexpr std::size_t kMaximumQueuedCommands = 8192U;
	inline constexpr std::size_t kMaximumCommandsPerActor = 64U;
	inline constexpr std::size_t kMaximumCommandBatch = 256U;
	inline constexpr std::size_t kMaximumRetainedReceipts = 16384U;
	inline constexpr std::size_t kMaximumProducersPerActor = 64U;
	inline constexpr std::size_t kMaximumObservationCopy = 256U;
	inline constexpr std::uint32_t kInvalidLocomotionSlot = std::numeric_limits<std::uint32_t>::max();

	// Every field participates in equality: UUID reuse, component replacement and
	// slot reuse must all invalidate an old managed/native command handle.
	struct TERRAENGINE_API LocomotionHandle {
		std::uint64_t sceneLifetime = 0U;
		UUID objectId = EMPTY_UUID;
		std::uint64_t objectGeneration = 0U;
		std::uint64_t attachmentGeneration = 0U;
		std::uint32_t slotIndex = kInvalidLocomotionSlot;
		std::uint32_t slotGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return sceneLifetime != 0U && objectId != EMPTY_UUID && objectGeneration != 0U
				&& attachmentGeneration != 0U && slotIndex != kInvalidLocomotionSlot && slotGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const LocomotionHandle&) const = default;
	};

	enum class MotionMode : std::uint8_t { Velocity, RootMotion };
	enum class AuthorityState : std::uint8_t { Idle, Controlled, Stopped, Suspended };
	enum class IntentKind : std::uint8_t { Player, Script, Navigation };
	enum class DirectionSpace : std::uint8_t { World, ControllerLocal };
	enum class IntentLifetime : std::uint8_t { OneUpdate, UntilStopped };
	// Route state is separate from ownership and binding status. In particular,
	// a zero target from waiting or blockage does not prove arrival.
	enum class NavigationMotionState : std::uint8_t {
		Unavailable, Waiting, Following, Blocked, Arrived, Failed
	};
	enum class NavigationCompatibilityField : std::uint8_t {
		None, ControllerEnabled, PhysicsEnabled, Shape, WorldScale,
		UpDirection, LocalCenter, OwnerColliderSet, Radius, Height,
		ClearanceMargin, StepHeight, SlopeAngle, InvalidProfile
	};
	enum class LocomotionReason : std::uint8_t {
		None, InvalidArgument, StaleHandle, StaleConfiguration, OwnershipConflict,
		UnsupportedController, UnsupportedFrame, MissingBinding, AnimationUnavailable,
		CapacityExceeded, NotPlaying, ExpiredReceipt, SequenceConflict,
		Stopped, Revoked, ExternalExclusive
	};
	enum class MotionParameterField : std::uint8_t { Speed, LocalDirectionX, LocalDirectionZ, Grounded, Moving, None };
	enum class MotionParameterError : std::uint8_t { None, Missing, WrongType, Duplicate, UnsupportedId, InvalidObservation };

	struct TERRAENGINE_API ResolvedLocomotionBindings {
		std::uint64_t configurationRevision = 0U;
		double maximumSpeed = 5.0;
		// The authored mode is visible while Idle; Acquire still chooses a mode
		// explicitly and must reserve any required RootMotion consumer.
		MotionMode initialMotionMode = MotionMode::Velocity;
		Physics::PhysicsCharacterCommandTarget controller{};
		Physics::PhysicsFrameHandle frame{};
		std::uint64_t controllerComponentGeneration = 0U;
		UUID navigationObjectId = EMPTY_UUID;
		std::uint64_t navigationAttachmentGeneration = 0U;
		std::uint64_t navigationConfigurationRevision = 0U;
		UUID animatorObjectId = EMPTY_UUID;
		std::uint64_t animatorAttachmentGeneration = 0U;
		std::uint64_t animatorControllerGeneration = 0U;
		std::uint64_t rootSourceGeneration = 0U;
		bool controllerSupported = false;
		bool animationRootReady = false;
	};

	struct TERRAENGINE_API MovementIntent {
		std::uint64_t ownerToken = 0U;
		IntentKind kind = IntentKind::Player;
		std::uint64_t sequence = 0U;
		DirectionSpace directionSpace = DirectionSpace::World;
		glm::dvec3 direction{0.0};
		double requestedSpeed = 0.0;
		IntentLifetime lifetime = IntentLifetime::OneUpdate;
		std::uint64_t acceptedUpdateEpoch = 0U;
	};

	struct TERRAENGINE_API MovementAuthority {
		LocomotionHandle handle{};
		Physics::PhysicsCharacterCommandTarget controller{};
		std::uint64_t ownerToken = 0U;
		std::uint64_t ownerProducerId = 0U;
		IntentKind kind = IntentKind::Player;
		MotionMode mode = MotionMode::Velocity;
		AuthorityState state = AuthorityState::Idle;
		std::uint64_t authorityEpoch = 0U;
		std::uint64_t configurationRevision = 0U;
		std::uint64_t acceptedSequence = 0U;
		LocomotionReason suspendedReason = LocomotionReason::None;
	};

	struct PhysicsCharacterHandleHash {
		[[nodiscard]] std::size_t operator()(const Physics::PhysicsCharacterHandle& handle) const noexcept {
			std::size_t hash = std::hash<std::uint64_t>{}(handle.worldId.value);
			auto mix = [&hash](std::size_t value) {
				hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
			};
			mix(handle.worldGeneration);
			mix(handle.slotIndex);
			mix(handle.slotGeneration);
			return hash;
		}
	};

	struct TERRAENGINE_API LocomotionAuthorityRoot {
		std::uint64_t publicationEpoch = 0U;
		std::vector<MovementAuthority> entries;
		std::unordered_map<Physics::PhysicsCharacterHandle, std::size_t,
			PhysicsCharacterHandleHash> byController;

		[[nodiscard]] const MovementAuthority* Find(Physics::PhysicsCharacterHandle controller) const noexcept {
			const auto found = byController.find(controller);
			return found == byController.end() ? nullptr : &entries[found->second];
		}
	};

	struct TERRAENGINE_API LocomotionObservation {
		LocomotionHandle handle{};
		MovementAuthority authority{};
		LocomotionReason bindingReason = LocomotionReason::None;
		LocomotionReason navigationReason = LocomotionReason::None;
		NavigationMotionState navigationState = NavigationMotionState::Unavailable;
		Navigation::NavigationOutcome navigationOutcome = Navigation::NavigationOutcome::Queued;
		Navigation::NavigationReason navigationAgentReason = Navigation::NavigationReason::None;
		NavigationCompatibilityField navigationCompatibilityField = NavigationCompatibilityField::None;
		double navigationControllerValue = 0.0;
		double navigationProfileValue = 0.0;
		LocomotionReason animationReason = LocomotionReason::None;
		MotionParameterField animationParameterField = MotionParameterField::None;
		MotionParameterError animationParameterError = MotionParameterError::None;
		glm::dvec3 desiredVelocity{0.0};
		double observedPlanarSpeed = 0.0;
		glm::dvec3 observedLocalDirection{0.0};
		bool grounded = false;
		Physics::PhysicsFrameHandle frame{};
		std::uint64_t physicalTick = 0U;
		double observationDurationSeconds = 0.0;
		bool motionHistoryAvailable = false;
		double remainingRouteDistanceMetres = 0.0;
		glm::dvec3 requestedTranslation{0.0};
		glm::dvec3 realizedTranslation{0.0};
		double requestedYawRadians = 0.0;
		double realizedYawRadians = 0.0;
		std::uint64_t publicationEpoch = 0U;
	};

	struct TERRAENGINE_API LocomotionObservationRoot {
		std::uint64_t publicationEpoch = 0U;
		std::vector<LocomotionObservation> entries;
	};

	// A copied live slot is safe for worker preparation; the store never hands out
	// mutable component or controller pointers.
	struct TERRAENGINE_API LocomotionRuntimeRecord {
		LocomotionHandle handle{};
		ResolvedLocomotionBindings bindings{};
		MovementAuthority authority{};
		std::optional<MovementIntent> intent;
		LocomotionReason bindingReason = LocomotionReason::None;
	};
}
