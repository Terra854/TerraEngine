// FixedBlockPool.h - Bounded typed storage for measured fixed-size churn.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace TerraEngine::Memory {

	template<typename T, std::size_t Capacity>
	class FixedBlockPool {
	public:
		FixedBlockPool() noexcept
		{
			for (std::size_t index = 0U; index < Capacity; ++index) next_[index] = index + 1U;
		}

		~FixedBlockPool() { Clear(); }
		FixedBlockPool(const FixedBlockPool&) = delete;
		FixedBlockPool& operator=(const FixedBlockPool&) = delete;

		template<typename... Arguments>
		[[nodiscard]] T* Create(Arguments&&... arguments)
		{
			if (freeHead_ >= Capacity) return nullptr;
			const std::size_t index = freeHead_;
			freeHead_ = next_[index];
			// Remove the slot from the free list while user construction runs, but
			// publish no live object until its lifetime has actually begun. If a
			// constructor throws, return this reservation to the current list head;
			// a nested successful allocation may have changed that head meanwhile.
			struct Reservation {
				FixedBlockPool& pool;
				std::size_t index;
				bool committed = false;
				~Reservation() noexcept {
					if (!committed) {
						pool.next_[index] = pool.freeHead_;
						pool.freeHead_ = index;
					}
				}
			} reservation{ *this, index };
			T* result = std::construct_at(Pointer(index), std::forward<Arguments>(arguments)...);
			occupied_[index] = true;
			++size_;
			highWater_ = size_ > highWater_ ? size_ : highWater_;
			reservation.committed = true;
			return result;
		}

		bool Destroy(T* value) noexcept
		{
			if (value == nullptr) return false;
			// This thread-confined pool accepts arbitrary live-storage candidates.
			// Equality is defined for foreign addresses; subtracting or ordering them
			// against our storage would not establish portable ownership. The bounded
			// O(Capacity) scan also avoids inspecting nonexistent zero-capacity slots.
			// A stale raw alias at a reused address still identifies the new live
			// object; callers needing generation checks must use a different interface.
			for (std::size_t index = 0U; index < Capacity; ++index) {
				if (!occupied_[index]) continue;
				// Occupancy proves a T lifetime exists before laundering its owned
				// address. Never dereference or destroy through the untrusted candidate.
				T* owned = std::launder(Pointer(index));
				if (owned != value) continue;
				std::destroy_at(owned);
				occupied_[index] = false;
				next_[index] = freeHead_;
				freeHead_ = index;
				--size_;
				return true;
			}
			return false;
		}

		void Clear() noexcept
		{
			for (std::size_t index = 0U; index < Capacity; ++index) {
				if (occupied_[index]) std::destroy_at(Pointer(index));
				occupied_[index] = false;
				next_[index] = index + 1U;
			}
			freeHead_ = 0U;
			size_ = 0U;
		}

		[[nodiscard]] std::size_t Size() const noexcept { return size_; }
		[[nodiscard]] std::size_t HighWater() const noexcept { return highWater_; }
		[[nodiscard]] static constexpr std::size_t MaximumSize() noexcept { return Capacity; }

	private:
		[[nodiscard]] T* Pointer(const std::size_t index) noexcept
		{
			return reinterpret_cast<T*>(storage_.data() + index * sizeof(T));
		}

		alignas(T) std::array<std::byte, sizeof(T) * Capacity> storage_{};
		std::array<std::size_t, Capacity> next_{};
		std::array<bool, Capacity> occupied_{};
		std::size_t freeHead_ = 0U;
		std::size_t size_ = 0U;
		std::size_t highWater_ = 0U;
	};

} // namespace TerraEngine::Memory
