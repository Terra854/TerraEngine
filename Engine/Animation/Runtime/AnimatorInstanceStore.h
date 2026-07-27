// AnimatorInstanceStore.h - Generation-safe runtime ownership for animator instances.

#pragma once

#include "Animation/AnimationTypes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint32_t kInvalidAnimatorInstanceSlot = 0xffffffffU;

	struct AnimatorInstanceHandle {
		std::uint32_t slotIndex = kInvalidAnimatorInstanceSlot;
		AnimationGeneration generation = 0U;

		constexpr bool IsValid() const noexcept { return slotIndex != kInvalidAnimatorInstanceSlot && generation != 0U; }
		auto operator<=>(const AnimatorInstanceHandle&) const = default;
	};

	struct AnimatorInstanceDescriptor {
		UUID ownerObjectId = EMPTY_UUID;
		AnimationGeneration componentGeneration = 0U;
		AnimationAssetHandle controller;
	};

	// Frame inputs are copied snapshots. Worker evaluation reads these values but
	// never mutates the store or a live component while a frame is being planned.
	struct AnimationFrameInput {
		AnimatorInstanceHandle handle;
		AnimatorInstanceDescriptor descriptor;
		AnimationFixedTick tick = 0;
	};

	class TERRAENGINE_API AnimatorInstanceStore {
	public:
		AnimatorInstanceHandle Create(AnimatorInstanceDescriptor descriptor);
		bool Invalidate(AnimatorInstanceHandle handle);
		// Bulk invalidation preserves slot generation history across scene or
		// system lifecycles. Calling it repeatedly is safe: only live slots enter
		// the free list, so a slot can never be inserted twice.
		void InvalidateAll();
		bool IsAlive(AnimatorInstanceHandle handle) const;
		// Scheduler activation needs only the membership count. Keeping this value
		// atomic avoids taking the store mutex on every dormant frame; descriptor
		// capture still takes the mutex before reading any live slot data.
		std::size_t ActiveCount() const noexcept;
		// This narrow monotonic hook exists solely for the regression test that
		// proves an exhausted 32-bit generation can never wrap and alias an ancient
		// handle. It cannot move a live slot backward to revive a stale identity.
		AnimatorInstanceHandle SetGenerationForTesting(AnimatorInstanceHandle handle, AnimationGeneration generation);
		// The caller supplies reusable storage. An empty store clears it without a
		// new allocation, preserving the zero-animator dormant path.
		void CaptureFrameInputs(AnimationFixedTick tick, std::vector<AnimationFrameInput>& output) const;

	private:
		struct Slot {
			AnimatorInstanceDescriptor descriptor;
			AnimationGeneration generation = 1U;
			bool alive = false;
			bool retired = false;
		};

		// The caller holds mutex_ and has already proven the slot is live. Both
		// single-instance and bulk invalidation use this path so exhausted
		// generations retire identically and reusable slots are queued once.
		void InvalidateSlotLocked(std::uint32_t slotIndex);

		mutable std::mutex mutex_;
		std::vector<Slot> slots_;
		std::vector<std::uint32_t> freeSlots_;
		std::atomic_size_t activeCount_ = 0U;
	};
}
