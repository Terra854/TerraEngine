// NavigationStores.h - bounded generation-qualified runtime record stores.
//
// Stores keep copied value data under a small mutex. Worker jobs receive their
// own immutable copies and return candidates through NavigationSystem; no method
// here grants access to live scene storage or provider-owned navigation objects.

#pragma once

#include "NavigationTypes.h"
#include "NavigationSettings.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Navigation {
	enum class NavigationCommandState : std::uint8_t {
		Queued,
		Applied,
		Rejected,
		Expired
	};

	// Common retained metadata makes expiry and cancellation deterministic across
	// request, route, agent, and command records. An expiry value of zero means
	// the owner must explicitly release the record before it can be reclaimed.
	struct NavigationStoredRecord {
		std::uint64_t createdTick = 0U;
		std::uint64_t expiryTick = 0U;
		NavigationRequestLifecycle lifecycle = NavigationRequestLifecycle::Queued;
		NavigationOutcome outcome = NavigationOutcome::Queued;
		NavigationReason reason = NavigationReason::None;
	};

	struct NavigationRequestRecord : NavigationStoredRecord {
		NavigationRequestHandle handle{};
		std::uint64_t intentGeneration = 0U;
		NavigationLocation start{};
		NavigationLocation destination{};
		bool allowPartial = false;
	};

	struct NavigationRouteRecord : NavigationStoredRecord {
		NavigationRouteHandle handle{};
		NavigationRequestHandle request{};
		std::uint64_t intentGeneration = 0U;
	};

	struct NavigationAgentRecord : NavigationStoredRecord {
		NavigationAgentHandle handle{};
		std::uint64_t intentGeneration = 0U;
		// A protected scene-object scan advances this value only when the
		// canonical pose changes. Route publication compares the captured value so
		// a result can never attach to an object that moved while it was searching.
		std::uint64_t poseGeneration = 0U;
		NavigationRequestHandle activeRequest{};
		NavigationRouteHandle activeRoute{};
		// Agents retain copied profile and endpoint state so a protected destination
		// command can replace intent without borrowing an ECS component from a job.
		NavigationProfile profile = NavigationProfile::Defaults();
		NavigationLocation currentPosition{};
		NavigationLocation destination{};
		std::uint32_t routePolicy = 0U;
	};

	struct NavigationCommandRecord : NavigationStoredRecord {
		NavigationCommandHandle handle{};
		NavigationCommandState state = NavigationCommandState::Queued;
		std::uint64_t resultingIntentGeneration = 0U;
		NavigationRequestHandle resultingRequest{};
	};

	template<typename Handle>
	struct NavigationStoreCreateResult {
		Handle handle{};
		NavigationHandleStatus status = NavigationHandleStatus::Invalid;

		[[nodiscard]] constexpr bool Succeeded() const noexcept {
			return status == NavigationHandleStatus::Alive && handle.IsValid();
		}
	};

	template<typename Record>
	struct NavigationStoreReadResult {
		NavigationHandleStatus status = NavigationHandleStatus::Invalid;
		std::optional<Record> record;

		[[nodiscard]] constexpr bool HasRecord() const noexcept { return record.has_value(); }
	};

	// The store owns a fixed maximum number of slots. It uses 64-bit generations
	// and retires a slot at overflow, rather than wrapping into a potentially
	// valid historic handle. It is intentionally a value-store, not a scheduler
	// publication mechanism; NavigationSystem controls when copied results become
	// visible to other engine systems.
	template<typename Handle, typename Record>
	class NavigationGenerationStore {
	public:
		explicit NavigationGenerationStore(std::size_t capacity)
			: capacity_(capacity) {
			slots_.reserve(capacity);
			freeSlots_.reserve(capacity);
		}

		NavigationGenerationStore(const NavigationGenerationStore&) = delete;
		NavigationGenerationStore& operator=(const NavigationGenerationStore&) = delete;

		[[nodiscard]] NavigationStoreCreateResult<Handle> Create(
			NavigationOwnerHandle owner,
			Record record) {
			if(!owner.IsValid()) {
				return { {}, NavigationHandleStatus::InvalidOwner };
			}

			std::lock_guard<std::mutex> lock(mutex_);
			while(!freeSlots_.empty()) {
				const NavigationSlotIndex index = freeSlots_.back();
				freeSlots_.pop_back();
				Slot& slot = slots_[index];
				if(slot.generation == std::numeric_limits<NavigationGeneration>::max()) {
					slot.state = SlotState::Retired;
					continue;
				}

				++slot.generation;
				slot.owner = owner;
				slot.state = SlotState::Alive;
				const Handle handle = MakeHandle(owner, index, slot.generation);
				record.handle = handle;
				slot.record = std::move(record);
				return { handle, NavigationHandleStatus::Alive };
			}

			if(slots_.size() >= capacity_ || slots_.size() >= static_cast<std::size_t>(kInvalidNavigationSlot)) {
				return { {}, NavigationHandleStatus::CapacityExceeded };
			}

			const NavigationSlotIndex index = static_cast<NavigationSlotIndex>(slots_.size());
			const Handle handle = MakeHandle(owner, index, 1U);
			record.handle = handle;
			slots_.push_back({ owner, 1U, SlotState::Alive, std::move(record) });
			return { handle, NavigationHandleStatus::Alive };
		}

		[[nodiscard]] NavigationStoreReadResult<Record> Read(Handle handle) const {
			return Read(handle, handle.owner);
		}

		[[nodiscard]] NavigationStoreReadResult<Record> Read(
			Handle handle,
			NavigationOwnerHandle owner) const {
			std::lock_guard<std::mutex> lock(mutex_);
			const NavigationHandleStatus status = StatusLocked(handle, owner);
			if(status != NavigationHandleStatus::Alive && status != NavigationHandleStatus::Expired) {
				return { status, std::nullopt };
			}
			return { status, slots_[handle.slotIndex].record };
		}

		[[nodiscard]] NavigationHandleStatus Status(Handle handle) const {
			return Status(handle, handle.owner);
		}

		[[nodiscard]] NavigationHandleStatus Status(Handle handle, NavigationOwnerHandle owner) const {
			std::lock_guard<std::mutex> lock(mutex_);
			return StatusLocked(handle, owner);
		}

		// Updates are limited to records that are currently alive. Terminal and
		// expired records are immutable audit data until their bounded retention
		// ends or their owner releases them.
		template<typename Mutator>
		[[nodiscard]] bool Update(Handle handle, Mutator&& mutator) {
			std::lock_guard<std::mutex> lock(mutex_);
			if(StatusLocked(handle, handle.owner) != NavigationHandleStatus::Alive) {
				return false;
			}
			mutator(slots_[handle.slotIndex].record);
			return true;
		}

		// Cancellation is idempotent. It turns active work into a terminal value
		// but preserves the slot until explicit release/expiry handling finishes.
		[[nodiscard]] bool Cancel(Handle handle) {
			std::lock_guard<std::mutex> lock(mutex_);
			const NavigationHandleStatus status = StatusLocked(handle, handle.owner);
			if(status == NavigationHandleStatus::Expired) {
				return false;
			}
			if(status != NavigationHandleStatus::Alive) {
				return false;
			}

			Record& record = slots_[handle.slotIndex].record;
			if(record.outcome == NavigationOutcome::Cancelled) {
				return true;
			}
			if(IsTerminalNavigationOutcome(record.outcome)) {
				return false;
			}
			record.lifecycle = NavigationRequestLifecycle::Terminal;
			record.outcome = NavigationOutcome::Cancelled;
			record.reason = NavigationReason::Cancelled;
			return true;
		}

		// Expiry makes old handles explicit while keeping a copied terminal result
		// available for bounded diagnostics. It never invokes callbacks or frees a
		// buffer currently owned by another navigation worker.
		[[nodiscard]] std::size_t Expire(std::uint64_t currentTick) {
			std::lock_guard<std::mutex> lock(mutex_);
			std::size_t expiredCount = 0U;
			for(Slot& slot : slots_) {
				if(slot.state != SlotState::Alive || slot.record.expiryTick == 0U
					|| slot.record.expiryTick > currentTick) {
					continue;
				}
				slot.state = SlotState::Expired;
				slot.record.lifecycle = NavigationRequestLifecycle::Terminal;
				slot.record.outcome = NavigationOutcome::Expired;
				slot.record.reason = NavigationReason::Expired;
				++expiredCount;
			}
			return expiredCount;
		}

		[[nodiscard]] bool Release(Handle handle) {
			std::lock_guard<std::mutex> lock(mutex_);
			const NavigationHandleStatus status = StatusLocked(handle, handle.owner);
			if(status != NavigationHandleStatus::Alive && status != NavigationHandleStatus::Expired) {
				return false;
			}
			slots_[handle.slotIndex].state = SlotState::Destroyed;
			freeSlots_.push_back(handle.slotIndex);
			return true;
		}

		// Scene replacement, script reload, and Play Mode exit call this once with
		// the exact old lifetime. A later owner may reuse capacity, but cannot use
		// an old handle because its owner generation will no longer match.
		[[nodiscard]] std::size_t RevokeOwner(NavigationOwnerHandle owner) {
			if(!owner.IsValid()) {
				return 0U;
			}

			std::lock_guard<std::mutex> lock(mutex_);
			std::size_t revokedCount = 0U;
			for(NavigationSlotIndex index = 0U; index < slots_.size(); ++index) {
				Slot& slot = slots_[index];
				if(slot.owner != owner || slot.state == SlotState::Retired || slot.state == SlotState::Destroyed) {
					continue;
				}
				slot.state = SlotState::Revoked;
				slot.record.lifecycle = NavigationRequestLifecycle::Terminal;
				slot.record.outcome = NavigationOutcome::Stale;
				slot.record.reason = NavigationReason::SceneReplaced;
				freeSlots_.push_back(index);
				++revokedCount;
			}
			return revokedCount;
		}

		[[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

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
			Expired,
			Destroyed,
			Revoked,
			Retired
		};

		struct Slot {
			NavigationOwnerHandle owner{};
			NavigationGeneration generation = 0U;
			SlotState state = SlotState::Destroyed;
			Record record{};
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
			switch(slot.state) {
			case SlotState::Alive: return NavigationHandleStatus::Alive;
			case SlotState::Expired: return NavigationHandleStatus::Expired;
			case SlotState::Destroyed: return NavigationHandleStatus::Destroyed;
			case SlotState::Revoked: return NavigationHandleStatus::StaleOwner;
			case SlotState::Retired: return NavigationHandleStatus::Retired;
			}
			return NavigationHandleStatus::Invalid;
		}

		std::size_t capacity_ = 0U;
		mutable std::mutex mutex_;
		std::vector<Slot> slots_;
		std::vector<NavigationSlotIndex> freeSlots_;
	};

	using NavigationRequestStore = NavigationGenerationStore<NavigationRequestHandle, NavigationRequestRecord>;
	using NavigationRouteStore = NavigationGenerationStore<NavigationRouteHandle, NavigationRouteRecord>;
	using NavigationAgentStore = NavigationGenerationStore<NavigationAgentHandle, NavigationAgentRecord>;
	using NavigationCommandStore = NavigationGenerationStore<NavigationCommandHandle, NavigationCommandRecord>;

	TERRAENGINE_API std::string_view ToString(NavigationCommandState state) noexcept;
}
