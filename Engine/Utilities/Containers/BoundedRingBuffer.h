// BoundedRingBuffer.h - Fixed-capacity FIFO storage with explicit overwrite facts.
//
// This container is intended for worker-local histories, events, and diagnostics
// that must never grow after configuration. It owns no locks; callers publish or
// synchronize it at their subsystem's declared protected boundary.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Utilities {

	enum class BoundedRingBufferConfigureStatus : std::uint8_t {
		Configured = 0U,
		CapacityExceeded = 1U,
		AllocationFailure = 2U
	};

	enum class BoundedRingBufferPushStatus : std::uint8_t {
		Stored = 0U,
		ReplacedOldest = 1U,
		Unconfigured = 2U,
		NoCapacity = 3U
	};

	// Statistics distinguish current occupancy from lifetime pressure. Clear()
	// resets occupancy but intentionally retains the high-water and overwrite facts.
	struct BoundedRingBufferStatistics {
		std::size_t size = 0U;
		std::size_t capacity = 0U;
		std::size_t highWaterEntries = 0U;
		std::uint64_t pushes = 0U;
		std::uint64_t overwrites = 0U;
	};

	template<typename T>
	class BoundedRingBuffer final {
		static_assert(std::is_nothrow_move_constructible_v<T>,
			"BoundedRingBuffer entries must be nothrow move constructible.");
		static_assert(std::is_nothrow_move_assignable_v<T>,
			"BoundedRingBuffer entries must be nothrow move assignable.");

	public:
		// Configure allocates the complete slot table before publishing it. A limit
		// or allocation failure therefore leaves the previous ring fully intact.
		[[nodiscard]] BoundedRingBufferConfigureStatus Configure(
			std::size_t capacity,
			std::size_t hardMaximum) noexcept
		{
			if(capacity > hardMaximum) {
				return BoundedRingBufferConfigureStatus::CapacityExceeded;
			}

			try {
				std::vector<std::optional<T>> staged(capacity);
				entries_.swap(staged);
			} catch(...) {
				return BoundedRingBufferConfigureStatus::AllocationFailure;
			}

			head_ = 0U;
			size_ = 0U;
			statistics_ = {};
			statistics_.capacity = capacity;
			configured_ = true;
			return BoundedRingBufferConfigureStatus::Configured;
		}

		// Push appends in logical FIFO order. Once full, exactly the oldest slot is
		// replaced and the caller receives an explicit pressure outcome.
		[[nodiscard]] BoundedRingBufferPushStatus Push(T value) noexcept
		{
			if(!configured_) {
				return BoundedRingBufferPushStatus::Unconfigured;
			}
			if(entries_.empty()) {
				return BoundedRingBufferPushStatus::NoCapacity;
			}

			std::size_t target = (head_ + size_) % entries_.size();
			BoundedRingBufferPushStatus status = BoundedRingBufferPushStatus::Stored;
			if(size_ == entries_.size()) {
				target = head_;
				head_ = (head_ + 1U) % entries_.size();
				++statistics_.overwrites;
				status = BoundedRingBufferPushStatus::ReplacedOldest;
			} else {
				++size_;
			}

			entries_[target].reset();
			entries_[target].emplace(std::move(value));
			++statistics_.pushes;
			statistics_.size = size_;
			if(statistics_.highWaterEntries < size_) {
				statistics_.highWaterEntries = size_;
			}
			return status;
		}

		void Clear() noexcept
		{
			for(std::optional<T>& entry : entries_) {
				entry.reset();
			}
			head_ = 0U;
			size_ = 0U;
			statistics_.size = 0U;
		}

		[[nodiscard]] const T* At(std::size_t logicalIndex) const noexcept
		{
			if(logicalIndex >= size_ || entries_.empty()) {
				return nullptr;
			}

			const std::optional<T>& entry =
				entries_[(head_ + logicalIndex) % entries_.size()];
			return entry ? std::addressof(*entry) : nullptr;
		}

		[[nodiscard]] const T* Oldest() const noexcept { return At(0U); }
		[[nodiscard]] const T* Newest() const noexcept
		{
			return size_ == 0U ? nullptr : At(size_ - 1U);
		}

		[[nodiscard]] const BoundedRingBufferStatistics& Statistics() const noexcept
		{
			return statistics_;
		}

		[[nodiscard]] bool IsConfigured() const noexcept { return configured_; }

	private:
		std::vector<std::optional<T>> entries_;
		std::size_t head_ = 0U;
		std::size_t size_ = 0U;
		BoundedRingBufferStatistics statistics_{};
		bool configured_ = false;
	};

} // namespace TerraEngine::Utilities
