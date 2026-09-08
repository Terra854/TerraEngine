// NavigationTypes.h - provider-neutral identities and validation contracts.
//
// Navigation requests outlive a single worker quantum, so every externally
// visible reference carries world, owner, slot, and non-wrapping generation
// identity. Recast/Detour types intentionally do not appear in this header.

#pragma once

#include "Core/Export.h"
#include "Scene/WorldCoordinate.h"

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

	// Source selection is shared by reflected authoring and immutable geometry
	// capture. Keeping the enum here prevents the two boundaries from assigning
	// different numeric meanings to the same persisted source kind.
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

	struct NavigationAgentHandle {
		NavigationOwnerHandle owner{};
		NavigationSlotIndex slotIndex = kInvalidNavigationSlot;
		NavigationGeneration slotGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return owner.IsValid() && slotIndex != kInvalidNavigationSlot && slotGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationAgentHandle&) const = default;
	};

	struct NavigationCommandHandle {
		NavigationOwnerHandle owner{};
		NavigationSlotIndex slotIndex = kInvalidNavigationSlot;
		NavigationGeneration slotGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return owner.IsValid() && slotIndex != kInvalidNavigationSlot && slotGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationCommandHandle&) const = default;
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
