// ReusableArenaResource.cpp - Alignment-safe arena allocation and reset.

#include "ReusableArenaResource.h"

#include <algorithm>
#include <cstdint>

namespace TerraEngine::Memory {

	ReusableArenaResource::ReusableArenaResource(
		const std::size_t capacityBytes,
		std::pmr::memory_resource* upstream)
		: storage_(capacityBytes), upstream_(upstream)
	{
		fallbacks_.reserve(8U);
		statistics_.capacityBytes = storage_.size();
	}

	ReusableArenaResource::~ReusableArenaResource()
	{
		Reset();
	}

	void ReusableArenaResource::Reset() noexcept
	{
		for (const FallbackAllocation& fallback : fallbacks_) {
			upstream_->deallocate(fallback.memory, fallback.bytes, fallback.alignment);
			// Publish reclamation only after this tracked allocation was actually
			// returned upstream. Empty/repeated resets cannot invent releases.
			++statistics_.fallbackDeallocationCount;
			--statistics_.fallbackOutstandingCount;
			statistics_.fallbackOutstandingBytes -= fallback.bytes;
		}
		fallbacks_.clear();
		offset_ = 0U;
		statistics_.usedBytes = 0U;
		++statistics_.resetCount;
	}

	void* ReusableArenaResource::do_allocate(const std::size_t bytes, const std::size_t alignment)
	{
		if (!storage_.empty()) {
			const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(storage_.data());
			const std::uintptr_t current = base + offset_;
			const std::size_t padding = (alignment - (current % alignment)) % alignment;
			if (padding <= storage_.size() - offset_ && bytes <= storage_.size() - offset_ - padding) {
				offset_ += padding + bytes;
				statistics_.usedBytes = offset_;
				statistics_.highWaterBytes = std::max(statistics_.highWaterBytes, offset_);
				++statistics_.reuseAllocationCount;
				return reinterpret_cast<void*>(current + padding);
			}
		}

		void* memory = upstream_->allocate(bytes, alignment);
		try {
			fallbacks_.push_back({ memory, bytes, alignment });
		} catch (...) {
			upstream_->deallocate(memory, bytes, alignment);
			throw;
		}
		++statistics_.fallbackAllocationCount;
		// Tracking succeeded, so these bytes now belong to the reset episode.
		// Allocation/tracking exceptions above leave committed counts untouched.
		++statistics_.fallbackOutstandingCount;
		statistics_.fallbackOutstandingBytes += bytes;
		return memory;
	}

	void ReusableArenaResource::do_deallocate(void*, std::size_t, std::size_t)
	{
		// Individual allocations share one episode lifetime. Reset releases every
		// fallback and makes the fixed backing available to the next episode.
	}

	bool ReusableArenaResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
	{
		return this == &other;
	}

} // namespace TerraEngine::Memory
