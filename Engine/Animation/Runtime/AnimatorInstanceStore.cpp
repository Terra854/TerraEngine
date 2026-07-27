// AnimatorInstanceStore.cpp - Protected storage and immutable animator snapshots.

#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <algorithm>

namespace TerraEngine::Animation {
	AnimatorInstanceHandle AnimatorInstanceStore::Create(AnimatorInstanceDescriptor descriptor) {
		if(descriptor.ownerObjectId == EMPTY_UUID || descriptor.componentGeneration == 0U) return {};
		std::scoped_lock lock(mutex_);
		std::uint32_t slotIndex = 0U;
		while(!freeSlots_.empty()) {
			slotIndex = freeSlots_.back();
			freeSlots_.pop_back();
			Slot& slot = slots_[slotIndex];
			if(slot.retired || slot.generation == UINT32_MAX) {
				slot.retired = true;
				continue;
			}
			++slot.generation;
			slot.descriptor = descriptor;
			slot.alive = true;
			activeCount_.fetch_add(1U, std::memory_order_release);
			return {slotIndex, slot.generation};
		}
		if(slots_.size() >= kInvalidAnimatorInstanceSlot) return {};
		else {
			slotIndex = static_cast<std::uint32_t>(slots_.size());
			slots_.push_back(Slot{descriptor, 1U, true, false});
			activeCount_.fetch_add(1U, std::memory_order_release);
		}
		return {slotIndex, slots_[slotIndex].generation};
	}

	bool AnimatorInstanceStore::Invalidate(AnimatorInstanceHandle handle) {
		std::scoped_lock lock(mutex_);
		if(!handle.IsValid() || handle.slotIndex >= slots_.size()) return false;
		Slot& slot = slots_[handle.slotIndex];
		if(!slot.alive || slot.generation != handle.generation) return false;
		InvalidateSlotLocked(handle.slotIndex);
		return true;
	}

	void AnimatorInstanceStore::InvalidateAll() {
		std::scoped_lock lock(mutex_);
		for(std::uint32_t slotIndex = 0U; slotIndex < slots_.size(); ++slotIndex) {
			// Dead slots are already either in the free list or permanently
			// retired. Skipping them makes repeated lifecycle invalidation
			// idempotent and prevents duplicate free-list entries.
			if(slots_[slotIndex].alive) InvalidateSlotLocked(slotIndex);
		}
	}

	bool AnimatorInstanceStore::IsAlive(AnimatorInstanceHandle handle) const {
		std::scoped_lock lock(mutex_);
		return handle.IsValid() && handle.slotIndex < slots_.size()
			&& slots_[handle.slotIndex].alive && slots_[handle.slotIndex].generation == handle.generation;
	}

	std::size_t AnimatorInstanceStore::ActiveCount() const noexcept {
		// Release updates occur only while slot ownership is protected. The
		// scheduler needs the count as activation metadata and never dereferences a
		// slot through this lock-free query.
		return activeCount_.load(std::memory_order_acquire);
	}

	AnimatorInstanceHandle AnimatorInstanceStore::SetGenerationForTesting(AnimatorInstanceHandle handle, AnimationGeneration generation) {
		std::scoped_lock lock(mutex_);
		if(!handle.IsValid() || handle.slotIndex >= slots_.size()) return {};
		Slot& slot = slots_[handle.slotIndex];
		if(!slot.alive || slot.generation != handle.generation || generation <= slot.generation) return {};
		slot.generation = generation;
		return {handle.slotIndex, generation};
	}

	void AnimatorInstanceStore::InvalidateSlotLocked(std::uint32_t slotIndex) {
		Slot& slot = slots_[slotIndex];
		slot.alive = false;
		activeCount_.fetch_sub(1U, std::memory_order_release);
		if(slot.generation == UINT32_MAX) {
			// There is no distinct non-zero generation left for this slot. It must
			// never re-enter the free list or the next create would wrap and alias.
			slot.retired = true;
			return;
		}
		freeSlots_.push_back(slotIndex);
	}

	void AnimatorInstanceStore::CaptureFrameInputs(AnimationFixedTick tick, std::vector<AnimationFrameInput>& output) const {
		output.clear();
		std::scoped_lock lock(mutex_);
		// The mutex already serializes slot mutations, so a relaxed load is enough
		// to size this protected snapshot consistently with the slots below.
		const std::size_t activeCount = activeCount_.load(std::memory_order_relaxed);
		if(activeCount == 0U) return;
		output.reserve(std::max(output.capacity(), activeCount));
		for(std::uint32_t index = 0U; index < slots_.size(); ++index) {
			const Slot& slot = slots_[index];
			if(slot.alive) output.push_back({{index, slot.generation}, slot.descriptor, tick});
		}
	}
}
