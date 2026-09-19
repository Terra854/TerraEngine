// NavigationAgentStore.cpp - bounded generation-qualified runtime ownership.

#include "NavigationAgentStore.h"

#include <atomic>
#include <limits>

namespace TerraEngine::Navigation {
	namespace {
		constexpr NavigationGeneration kStoreSlotGenerationMask = 0x00000000FFFFFFFFULL;
		std::atomic_uint32_t nextStoreIncarnation{1U};

		[[nodiscard]] std::uint32_t AllocateStoreIncarnation() noexcept {
			std::uint32_t incarnation = nextStoreIncarnation.load(std::memory_order_relaxed);
			// Exhaustion fails closed instead of wrapping to an earlier incarnation.
			while(incarnation != std::numeric_limits<std::uint32_t>::max()) {
				if(nextStoreIncarnation.compare_exchange_weak(incarnation, incarnation + 1U,
					std::memory_order_relaxed)) return incarnation;
			}
			return 0U;
		}
	}

	NavigationAgentStore::NavigationAgentStore(std::size_t capacity)
		: capacity_(capacity), generationPrefix_(static_cast<NavigationGeneration>(AllocateStoreIncarnation()) << 32U) {
		slots_.reserve(capacity);
		free_.reserve(capacity);
	}

	bool NavigationAgentStore::Matches(NavigationAgentIdentity identity) const {
		return identity.IsValid()
			&& identity.runtimeSlot < slots_.size()
			&& slots_[identity.runtimeSlot].alive
			&& slots_[identity.runtimeSlot].generation == identity.runtimeSlotGeneration
			&& slots_[identity.runtimeSlot].record.identity.object == identity.object
			&& slots_[identity.runtimeSlot].record.identity.world == identity.world
			&& slots_[identity.runtimeSlot].record.identity.attachmentGeneration == identity.attachmentGeneration;
	}

	NavigationAgentStoreCreateResult NavigationAgentStore::Create(
		NavigationAgentIdentity identity, NavigationAgentRuntimeRecord record) {
		if(!identity.world.IsValid() || !identity.object.IsValid() || identity.attachmentGeneration == 0U)
			return {{}, NavigationHandleStatus::InvalidOwner};
		std::lock_guard lock(mutex_);
		if(generationPrefix_ == 0U) return {{}, NavigationHandleStatus::Retired};
		NavigationSlotIndex index = kInvalidNavigationSlot;
		if(!free_.empty()) {
			index = free_.back();
			free_.pop_back();
		} else if(slots_.size() < capacity_) {
			index = static_cast<NavigationSlotIndex>(slots_.size());
			slots_.push_back({});
		} else {
			return {{}, NavigationHandleStatus::CapacityExceeded};
		}
		Slot& slot = slots_[index];
		if(slot.generation == 0U) slot.generation = generationPrefix_;
		// Retire a slot before its low reuse counter could spill into the next
		// incarnation prefix and accidentally reproduce another store's identity.
		if((slot.generation & kStoreSlotGenerationMask) == kStoreSlotGenerationMask)
			return {{}, NavigationHandleStatus::Retired};
		++slot.generation;
		slot.alive = true;
		identity.runtimeSlot = index;
		identity.runtimeSlotGeneration = slot.generation;
		record.identity = identity;
		slot.record = std::move(record);
		return {identity, NavigationHandleStatus::Alive};
	}

	NavigationAgentStoreReadResult NavigationAgentStore::Read(NavigationAgentIdentity identity) const {
		std::lock_guard lock(mutex_);
		if(!Matches(identity)) return {};
		return {NavigationHandleStatus::Alive, slots_[identity.runtimeSlot].record};
	}

	bool NavigationAgentStore::Release(NavigationAgentIdentity identity) {
		std::lock_guard lock(mutex_);
		if(!Matches(identity)) return false;
		slots_[identity.runtimeSlot].record = {};
		slots_[identity.runtimeSlot].alive = false;
		free_.push_back(identity.runtimeSlot);
		return true;
	}

	std::size_t NavigationAgentStore::RevokeWorld(NavigationWorldHandle world) {
		std::lock_guard lock(mutex_);
		std::size_t revoked = 0U;
		for(NavigationSlotIndex index = 0U; index < slots_.size(); ++index) {
			if(!slots_[index].alive || slots_[index].record.identity.world != world) continue;
			slots_[index].record = {};
			slots_[index].alive = false;
			free_.push_back(index);
			++revoked;
		}
		return revoked;
	}

	std::vector<NavigationAgentRuntimeRecord> NavigationAgentStore::Snapshot() const {
		std::lock_guard lock(mutex_);
		std::vector<NavigationAgentRuntimeRecord> result;
		result.reserve(slots_.size());
		for(const Slot& slot : slots_) if(slot.alive) result.push_back(slot.record);
		return result;
	}

	std::size_t NavigationAgentStore::AliveCount() const {
		std::lock_guard lock(mutex_);
		std::size_t count = 0U;
		for(const Slot& slot : slots_) count += slot.alive ? 1U : 0U;
		return count;
	}

	std::size_t NavigationAgentStore::MemoryBytes() const {
		std::lock_guard lock(mutex_);
		return slots_.capacity() * sizeof(Slot) + free_.capacity() * sizeof(NavigationSlotIndex);
	}
}
