// NavigationTypes.h - provider-neutral identities and validation contracts.
//
// Navigation requests outlive a single worker quantum, so every externally
// visible reference carries world, owner, slot, and non-wrapping generation
// identity. Recast/Detour types intentionally do not appear in this header.

#pragma once

#include "Core/Export.h"
#include "Scene/WorldCoordinate.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace TerraEngine::Navigation {
	using NavigationGeneration = std::uint64_t;
	using NavigationSlotIndex = std::uint32_t;

	inline constexpr NavigationSlotIndex kInvalidNavigationSlot = std::numeric_limits<NavigationSlotIndex>::max();

	// Source representations are physical inputs to immutable geometry capture.
	// They describe how a source was observed, never an object-side navigation
	// component or a traversal command.
	enum class NavigationSourceKind : std::uint8_t {
		Auto,
		Mesh,
		Collider,
		Terrain,
		Procedural
	};

	// A world ID is a compact engine-owned value, not a provider mesh pointer.
	struct NavigationWorldId {
		std::uint64_t value = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0U; }
		[[nodiscard]] constexpr bool operator==(const NavigationWorldId&) const = default;
	};

	struct NavigationWorldHandle {
		NavigationWorldId worldId{};
		NavigationGeneration worldGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return worldId.IsValid() && worldGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationWorldHandle&) const = default;
	};

	// Owner identity distinguishes a replaced scene object or script domain from
	// a new object that happens to retain the same authored persistent ID.
	struct NavigationOwnerId {
		std::uint64_t value = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0U; }
		[[nodiscard]] constexpr bool operator==(const NavigationOwnerId&) const = default;
	};

	struct NavigationOwnerHandle {
		NavigationWorldHandle world{};
		NavigationOwnerId ownerId{};
		NavigationGeneration ownerGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return world.IsValid() && ownerId.IsValid() && ownerGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationOwnerHandle&) const = default;
	};

	struct NavigationRequestHandle {
		NavigationOwnerHandle owner{};
		NavigationSlotIndex slotIndex = kInvalidNavigationSlot;
		NavigationGeneration slotGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return owner.IsValid() && slotIndex != kInvalidNavigationSlot && slotGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationRequestHandle&) const = default;
	};

	struct NavigationRouteHandle {
		NavigationOwnerHandle owner{};
		NavigationSlotIndex slotIndex = kInvalidNavigationSlot;
		NavigationGeneration slotGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return owner.IsValid() && slotIndex != kInvalidNavigationSlot && slotGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationRouteHandle&) const = default;
	};

	// Handle status is deliberately richer than a boolean so callers can tell an
	// expired result from a stale owner and choose a safe gameplay response.
	enum class NavigationHandleStatus : std::uint8_t {
		Invalid,
		InvalidOwner,
		Alive,
		Expired,
		Destroyed,
		StaleSlot,
		StaleOwner,
		StaleWorld,
		Retired,
		CapacityExceeded
	};

	// Generation-qualified handles are stored in bounded slot arrays. This small
	// allocator is usable by value-only stores and tests; it never owns scene or
	// provider memory and never creates a worker thread.
	template<typename Handle>
	class NavigationHandleAllocator {
	public:
		explicit NavigationHandleAllocator(std::size_t capacity = 0U)
			: capacity_(capacity) {
			slots_.reserve(capacity);
			freeSlots_.reserve(capacity);
		}

		NavigationHandleAllocator(const NavigationHandleAllocator&) = delete;
		NavigationHandleAllocator& operator=(const NavigationHandleAllocator&) = delete;

		[[nodiscard]] Handle Allocate(NavigationOwnerHandle owner) {
			if(!owner.IsValid()) {
				return {};
			}

			std::lock_guard<std::mutex> lock(mutex_);
			while(!freeSlots_.empty()) {
				const NavigationSlotIndex index = freeSlots_.back();
				freeSlots_.pop_back();
				Slot& slot = slots_[index];
				if(slot.generation == std::numeric_limits<NavigationGeneration>::max()) {
					// Wrapping could make an ancient handle valid again. Retire the
					// slot permanently and continue looking for safe capacity.
					slot.state = SlotState::Retired;
					continue;
				}

				++slot.generation;
				slot.owner = owner;
				slot.state = SlotState::Alive;
				return MakeHandle(owner, index, slot.generation);
			}

			if(slots_.size() >= capacity_ || slots_.size() >= static_cast<std::size_t>(kInvalidNavigationSlot)) {
				return {};
			}

			const NavigationSlotIndex index = static_cast<NavigationSlotIndex>(slots_.size());
			slots_.push_back({ owner, 1U, SlotState::Alive });
			return MakeHandle(owner, index, 1U);
		}

		// Test-only seeding exercises the non-wrapping retirement path without
		// requiring 2^64 allocation/release cycles in a normal unit test.
		[[nodiscard]] Handle AllocateForTesting(NavigationOwnerHandle owner, NavigationGeneration generation) {
			if(!owner.IsValid() || generation == 0U) {
				return {};
			}

			std::lock_guard<std::mutex> lock(mutex_);
			if(!freeSlots_.empty() || slots_.size() >= capacity_ || slots_.size() >= static_cast<std::size_t>(kInvalidNavigationSlot)) {
				return {};
			}

			const NavigationSlotIndex index = static_cast<NavigationSlotIndex>(slots_.size());
			slots_.push_back({ owner, generation, SlotState::Alive });
			return MakeHandle(owner, index, generation);
		}

		[[nodiscard]] bool Release(Handle handle) {
			std::lock_guard<std::mutex> lock(mutex_);
			if(StatusLocked(handle, handle.owner) != NavigationHandleStatus::Alive) {
				return false;
			}

			Slot& slot = slots_[handle.slotIndex];
			slot.state = SlotState::Destroyed;
			freeSlots_.push_back(handle.slotIndex);
			return true;
		}

		[[nodiscard]] NavigationHandleStatus Status(Handle handle) const {
			return Status(handle, handle.owner);
		}

		[[nodiscard]] NavigationHandleStatus Status(Handle handle, NavigationOwnerHandle owner) const {
			std::lock_guard<std::mutex> lock(mutex_);
			return StatusLocked(handle, owner);
		}

		[[nodiscard]] std::size_t Capacity() const {
			return capacity_;
		}

		[[nodiscard]] std::size_t AliveCount() const {
			std::lock_guard<std::mutex> lock(mutex_);
			std::size_t aliveCount = 0U;
			for(const Slot& slot : slots_) {
				aliveCount += slot.state == SlotState::Alive ? 1U : 0U;
			}
			return aliveCount;
		}

	private:
		enum class SlotState : std::uint8_t {
			Alive,
			Destroyed,
			Retired
		};

		struct Slot {
			NavigationOwnerHandle owner{};
			NavigationGeneration generation = 0U;
			SlotState state = SlotState::Destroyed;
		};

		[[nodiscard]] static Handle MakeHandle(
			NavigationOwnerHandle owner,
			NavigationSlotIndex index,
			NavigationGeneration generation) {
			return Handle{ owner, index, generation };
		}

		[[nodiscard]] NavigationHandleStatus StatusLocked(Handle handle, NavigationOwnerHandle owner) const {
			if(!owner.IsValid()) {
				return NavigationHandleStatus::InvalidOwner;
			}
			if(!handle.IsValid() || handle.slotIndex >= slots_.size()) {
				return NavigationHandleStatus::Invalid;
			}
			if(handle.owner.world != owner.world) {
				return NavigationHandleStatus::StaleWorld;
			}
			if(handle.owner.ownerId != owner.ownerId || handle.owner.ownerGeneration != owner.ownerGeneration) {
				return NavigationHandleStatus::StaleOwner;
			}

			const Slot& slot = slots_[handle.slotIndex];
			if(slot.owner.world != owner.world) {
				return NavigationHandleStatus::StaleWorld;
			}
			if(slot.owner.ownerId != owner.ownerId || slot.owner.ownerGeneration != owner.ownerGeneration) {
				return NavigationHandleStatus::StaleOwner;
			}
			if(slot.generation != handle.slotGeneration) {
				return NavigationHandleStatus::StaleSlot;
			}
			if(slot.state == SlotState::Retired) {
				return NavigationHandleStatus::Retired;
			}
			return slot.state == SlotState::Alive
				? NavigationHandleStatus::Alive
				: NavigationHandleStatus::Destroyed;
		}

		std::size_t capacity_ = 0U;
		mutable std::mutex mutex_;
		std::vector<Slot> slots_;
		std::vector<NavigationSlotIndex> freeSlots_;
	};

	// Lifecycle and outcome values are provider-neutral. Backend success bits are
	// mapped into these values before a result becomes visible to gameplay.
	enum class NavigationRequestLifecycle : std::uint8_t {
		Queued,
		PendingPreparation,
		Searching,
		Terminal
	};

	enum class NavigationOutcome : std::uint8_t {
		Queued,
		PendingPreparation,
		Searching,
		Complete,
		Partial,
		Unreachable,
		InvalidRequest,
		InvalidEndpoint,
		UnavailableCoverage,
		BudgetExhausted,
		RejectedCapacity,
		Cancelled,
		Expired,
		Stale,
		Failed
	};

	enum class NavigationReason : std::uint8_t {
		None,
		MissingTerrain,
		MissingSource,
		DirtyCoverage,
		MissingNeighbor,
		DomainLimit,
		ResolutionLimit,
		NodeLimit,
		OutputLimit,
		InvalidStart,
		InvalidDestination,
		InsufficientClearance,
		MissingSupport,
		SupersededRequest,
		SceneReplaced,
		ProviderFailure,
		Cancelled,
		Expired
	};

	[[nodiscard]] constexpr bool IsTerminalNavigationOutcome(NavigationOutcome outcome) noexcept {
		return outcome != NavigationOutcome::Queued
			&& outcome != NavigationOutcome::PendingPreparation
			&& outcome != NavigationOutcome::Searching;
	}

	TERRAENGINE_API std::string_view ToString(NavigationHandleStatus status) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationRequestLifecycle lifecycle) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationOutcome outcome) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationReason reason) noexcept;

	// Public navigation locations retain the engine's exact large-world address
	// rather than leaking a provider-local float vector outside a shard build.
	struct NavigationLocation {
		NavigationWorldHandle world{};
		Scene::WorldPosition position{};

		[[nodiscard]] bool operator==(const NavigationLocation&) const = default;
	};

	// A UUID alone is not enough at an asynchronous boundary: a scene can reuse
	// the same persistent UUID after replacement. The lifetime generation makes
	// that replacement visible to request admission and worker completion checks.
	struct NavigationObjectIdentity {
		UUID objectId = EMPTY_UUID;
		NavigationGeneration lifetimeGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return objectId != EMPTY_UUID && lifetimeGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationObjectIdentity&) const = default;
	};

	// This is a copied protected-boundary observation. Worker jobs may retain it
	// freely because it contains no ObjectManager pointer, Transform pointer, or
	// mutable scene storage reference.
	struct NavigationObjectEndpoint {
		NavigationObjectIdentity object{};
		NavigationLocation location{};
		NavigationGeneration transformRevision = 0U;
		bool activeInHierarchy = false;

		[[nodiscard]] bool operator==(const NavigationObjectEndpoint&) const = default;
	};

	enum class NavigationDestinationKind : std::uint8_t {
		Location,
		Object
	};

	// Request policy is deliberately a value passed by gameplay. The navigation
	// system never persists it on a scene object or changes gameplay movement.
	enum class NavigationRoutePolicy : std::uint8_t {
		ExactDestination,
		NearestReachable
	};

	// An object request first describes its desired target without resolving live
	// scene state. The protected capture step then creates NavigationObjectEndpoint
	// values for both source and destination before a worker receives the request.
	struct NavigationRequestedDestination {
		NavigationDestinationKind kind = NavigationDestinationKind::Location;
		NavigationLocation location{};
		NavigationObjectIdentity object{};

		[[nodiscard]] bool operator==(const NavigationRequestedDestination&) const = default;
	};

	enum class NavigationEndpointValidationCode : std::uint8_t {
		None,
		InvalidObject,
		InactiveObject,
		InvalidLocation,
		MissingTransformRevision,
		WrongDestinationVariant
	};

	struct NavigationEndpointValidation {
		bool accepted = false;
		NavigationEndpointValidationCode code = NavigationEndpointValidationCode::InvalidObject;

		[[nodiscard]] constexpr bool Succeeded() const noexcept { return accepted; }
	};

	TERRAENGINE_API NavigationEndpointValidation ValidateNavigationObjectEndpoint(
		const NavigationObjectEndpoint& endpoint) noexcept;
	TERRAENGINE_API NavigationEndpointValidation ValidateNavigationRequestedDestination(
		const NavigationRequestedDestination& destination) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationDestinationKind kind) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationRoutePolicy policy) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationEndpointValidationCode code) noexcept;

	// These values are copied with each admitted request and route result. They
	// make mixed world generations observable and allow completion to fail stale
	// rather than publishing a route from an incoherent navigation world.
	struct NavigationObservationEvidence {
		NavigationGeneration sceneGeneration = 0U;
		NavigationGeneration sourceTransformRevision = 0U;
		NavigationGeneration destinationTransformRevision = 0U;
		NavigationGeneration terrainPublicationGeneration = 0U;
		NavigationGeneration worldPublicationGeneration = 0U;
		NavigationGeneration sourceIndexRevision = 0U;

		[[nodiscard]] constexpr bool HasWorldEvidence() const noexcept {
			return terrainPublicationGeneration != 0U && worldPublicationGeneration != 0U
				&& sourceIndexRevision != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationObservationEvidence&) const = default;
	};

	// Automatic geometry uses a stable representation identity rather than an
	// authored source component. Source ID is deterministic at capture time from
	// the object/procedural producer, representation, asset, and subshape.
	struct NavigationEligibleSourceIdentity {
		UUID sourceId = EMPTY_UUID;
		UUID objectId = EMPTY_UUID;
		NavigationGeneration objectLifetimeGeneration = 0U;
		UUID assetId = EMPTY_UUID;
		NavigationSourceKind kind = NavigationSourceKind::Mesh;
		std::uint32_t subshapeIndex = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return sourceId != EMPTY_UUID && kind != NavigationSourceKind::Auto;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationEligibleSourceIdentity&) const = default;
	};

	// Revision evidence identifies exactly which physical publication supplied a
	// source. A nonzero value in every field prevents a worker from treating an
	// unready or partially captured source as current geometry.
	struct NavigationSourceRevisionEvidence {
		NavigationGeneration objectLifetimeGeneration = 0U;
		NavigationGeneration transformRevision = 0U;
		NavigationGeneration geometryRevision = 0U;
		NavigationGeneration physicalPublicationGeneration = 0U;
		NavigationGeneration resourcePublicationGeneration = 0U;

		[[nodiscard]] constexpr bool IsComplete() const noexcept {
			return objectLifetimeGeneration != 0U && transformRevision != 0U
				&& geometryRevision != 0U && physicalPublicationGeneration != 0U
				&& resourcePublicationGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationSourceRevisionEvidence&) const = default;
	};

	enum class NavigationLocationValidationCode : std::uint8_t {
		None,
		InvalidWorld,
		NonFinitePosition,
		CoordinateOverflow,
		UnnormalizedPosition
	};

	struct NavigationLocationValidation {
		bool accepted = false;
		NavigationLocationValidationCode code = NavigationLocationValidationCode::InvalidWorld;

		[[nodiscard]] constexpr bool Succeeded() const noexcept { return accepted; }
	};

	TERRAENGINE_API NavigationLocationValidation ValidateNavigationLocation(const NavigationLocation& location) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationLocationValidationCode code) noexcept;
}
