// TrackingMemoryResource.h - Thread-confined PMR allocation accounting.

#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>

namespace TerraEngine::Memory {

	struct MemoryResourceStatistics {
		std::size_t allocationCount = 0U;
		std::size_t deallocationCount = 0U;
		std::size_t bytesInUse = 0U;
		std::size_t highWaterBytes = 0U;
	};

	// The resource is intentionally thread-confined. Domain owners publish copied
	// counters at protected boundaries instead of adding atomics to every allocation.
	class TrackingMemoryResource final : public std::pmr::memory_resource {
	public:
		explicit TrackingMemoryResource(
			std::pmr::memory_resource* upstream = std::pmr::new_delete_resource()) noexcept
			: upstream_(upstream)
		{
		}

		[[nodiscard]] const MemoryResourceStatistics& Statistics() const noexcept { return statistics_; }

	private:
		void* do_allocate(const std::size_t bytes, const std::size_t alignment) override
		{
			void* memory = upstream_->allocate(bytes, alignment);
			++statistics_.allocationCount;
			statistics_.bytesInUse += bytes;
			statistics_.highWaterBytes = std::max(statistics_.highWaterBytes, statistics_.bytesInUse);
			return memory;
		}

		void do_deallocate(void* memory, const std::size_t bytes, const std::size_t alignment) override
		{
			upstream_->deallocate(memory, bytes, alignment);
			++statistics_.deallocationCount;
			statistics_.bytesInUse = bytes > statistics_.bytesInUse ? 0U : statistics_.bytesInUse - bytes;
		}

		[[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
		{
			return this == &other;
		}

		std::pmr::memory_resource* upstream_ = nullptr;
		MemoryResourceStatistics statistics_{};
	};

} // namespace TerraEngine::Memory
