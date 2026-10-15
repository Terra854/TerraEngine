// ReusableArenaResource.h - Resettable PMR backing with explicit pressure evidence.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <memory_resource>
#include <vector>

namespace TerraEngine::Memory {

	struct ReusableArenaStatistics {
		std::size_t capacityBytes = 0U;
		std::size_t usedBytes = 0U;
		std::size_t highWaterBytes = 0U;
		std::size_t reuseAllocationCount = 0U;
		std::size_t fallbackAllocationCount = 0U;
		std::size_t resetCount = 0U;
		// Cumulative releases count actual Reset deallocations, not reset calls.
		// Outstanding facts describe committed fallback ownership at this instant.
		std::size_t fallbackDeallocationCount = 0U;
		std::size_t fallbackOutstandingCount = 0U;
		std::size_t fallbackOutstandingBytes = 0U;
	};

	// One coordinator thread owns an arena. Reset is legal only after all PMR
	// containers from the previous episode have been destroyed.
	class TERRAENGINE_API ReusableArenaResource final : public std::pmr::memory_resource {
	public:
		explicit ReusableArenaResource(
			std::size_t capacityBytes,
			std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());
		~ReusableArenaResource() override;

		ReusableArenaResource(const ReusableArenaResource&) = delete;
		ReusableArenaResource& operator=(const ReusableArenaResource&) = delete;

		void Reset() noexcept;
		[[nodiscard]] const ReusableArenaStatistics& Statistics() const noexcept { return statistics_; }

	private:
		struct FallbackAllocation {
			void* memory = nullptr;
			std::size_t bytes = 0U;
			std::size_t alignment = 0U;
		};

		void* do_allocate(std::size_t bytes, std::size_t alignment) override;
		void do_deallocate(void*, std::size_t, std::size_t) override;
		[[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

		std::vector<std::byte> storage_;
		std::vector<FallbackAllocation> fallbacks_;
		std::pmr::memory_resource* upstream_ = nullptr;
		std::size_t offset_ = 0U;
		ReusableArenaStatistics statistics_{};
	};

} // namespace TerraEngine::Memory
