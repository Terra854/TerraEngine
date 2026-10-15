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
			const std::byte* address = reinterpret_cast<const std::byte*>(value);
			const std::byte* begin = storage_.data();
			const std::ptrdiff_t difference = address - begin;
			if (difference < 0 || difference % static_cast<std::ptrdiff_t>(sizeof(T)) != 0) return false;
			const std::size_t index = static_cast<std::size_t>(difference) / sizeof(T);
			if (index >= Capacity || !occupied_[index]) return false;
			std::destroy_at(value);
			occupied_[index] = false;
			next_[index] = freeHead_;
			freeHead_ = index;
			--size_;
			return true;
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
