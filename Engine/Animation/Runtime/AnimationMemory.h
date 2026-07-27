// AnimationMemory.h - Bounded animation allocation and retention primitives.
//
// Animation workers use these helpers to reserve memory before they prepare
// immutable output. The controller accounts bytes but never exposes live scene
// storage, while the arena and ring types own only worker-local prepared data.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Core/Export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Animation {
	// Every long-lived animation allocation belongs to one diagnostic class. The
	// Count value is a checked array bound and is never accepted as a request.
	enum class AnimationMemoryClass : std::uint8_t {
		FrameArena = 0U,
		PosePool = 1U,
		ScratchPool = 2U,
		EventRing = 3U,
		HistoryRing = 4U,
		DiagnosticRing = 5U,
		Residency = 6U,
		Count = 7U
	};

	inline constexpr std::size_t kAnimationMemoryClassCount =
		static_cast<std::size_t>(AnimationMemoryClass::Count);
	inline constexpr std::size_t kAnimationMemoryMaximumReservations = 1U << 20U;
	inline constexpr std::size_t kAnimationFrameArenaHardMaximumBytes = 1ULL << 30U;

	// Required storage remains resident until its owner releases it. Evictable
	// storage represents caches or optional history that callers can rebuild.
	enum class AnimationMemoryRetention : std::uint8_t {
		Required = 0U,
		Evictable = 1U
	};

	// A dedicated result preserves the reason for a safe fallback without adding
	// provider-specific allocation outcomes to the public AnimationStatus ABI.
	enum class AnimationMemoryStatus : std::uint8_t {
		Reserved = 0U,
		Uninitialized = 1U,
		InvalidArgument = 2U,
		BudgetExceeded = 3U,
		ReservationTableFull = 4U,
		AllocationFailure = 5U,
		Shutdown = 6U,
		InternalError = 7U
	};

	struct AnimationMemoryClassBudget {
		std::size_t maximumBytes = 0U;
	};

	struct AnimationMemoryConfiguration {
		std::array<AnimationMemoryClassBudget, kAnimationMemoryClassCount> classes{};
		std::size_t maximumTotalBytes = 0U;
		std::size_t maximumReservations = 0U;
	};

	struct AnimationMemoryReservationRequest {
		AnimationMemoryClass memoryClass = AnimationMemoryClass::FrameArena;
		std::size_t bytes = 0U;
		// Frame/publication identities are 64-bit process-lifetime values. Memory
		// accounting must retain the complete owner identity in long editor sessions.
		std::uint64_t ownerGeneration = 0U;
		AnimationMemoryRetention retention = AnimationMemoryRetention::Required;
		// Lower values are evicted first. Equal priorities use oldest-first order,
		// which makes pressure behavior deterministic across worker schedules.
		std::uint32_t retentionPriority = 0U;
	};

	struct AnimationMemoryClassCounters {
		std::size_t bytesInUse = 0U;
		std::size_t highWaterBytes = 0U;
		std::uint64_t successfulReservations = 0U;
		std::uint64_t releases = 0U;
		std::uint64_t evictions = 0U;
		std::uint64_t evictedBytes = 0U;
		std::uint64_t rejectedRequests = 0U;
		std::uint64_t rejectedBytes = 0U;
	};

	struct AnimationMemorySnapshot {
		std::array<AnimationMemoryClassCounters, kAnimationMemoryClassCount> classes{};
		std::size_t totalBytesInUse = 0U;
		std::size_t totalHighWaterBytes = 0U;
		std::size_t activeReservations = 0U;
		std::uint64_t pressureEvents = 0U;
		std::uint64_t allocationFailures = 0U;
		bool configured = false;
		bool shutdown = false;
	};

	struct AnimationMemoryState;

	// A lease is a move-only accounting token. Destruction releases a live
	// reservation; an optional reservation evicted under pressure simply becomes
	// inactive, so a late destructor cannot release a reused table slot.
	class TERRAENGINE_API AnimationMemoryLease {
	public:
		AnimationMemoryLease() noexcept = default;
		~AnimationMemoryLease();
		AnimationMemoryLease(const AnimationMemoryLease&) = delete;
		AnimationMemoryLease& operator=(const AnimationMemoryLease&) = delete;
		AnimationMemoryLease(AnimationMemoryLease&& other) noexcept;
		AnimationMemoryLease& operator=(AnimationMemoryLease&& other) noexcept;

		void Release() noexcept;
		[[nodiscard]] bool IsActive() const noexcept;
		[[nodiscard]] explicit operator bool() const noexcept { return IsActive(); }
		[[nodiscard]] AnimationMemoryClass MemoryClass() const noexcept { return memoryClass_; }
		[[nodiscard]] std::size_t Bytes() const noexcept { return bytes_; }
		[[nodiscard]] std::uint64_t OwnerGeneration() const noexcept { return ownerGeneration_; }

	private:
		friend class AnimationMemoryController;
		AnimationMemoryLease(
			std::shared_ptr<AnimationMemoryState> state,
			std::size_t slotIndex,
			std::uint64_t reservationSerial,
			const AnimationMemoryReservationRequest& request) noexcept;

		std::shared_ptr<AnimationMemoryState> state_;
		std::size_t slotIndex_ = 0U;
		std::uint64_t reservationSerial_ = 0U;
		AnimationMemoryClass memoryClass_ = AnimationMemoryClass::FrameArena;
		std::size_t bytes_ = 0U;
		std::uint64_t ownerGeneration_ = 0U;
	};

	// Configuration happens at a protected lifecycle boundary. Reservations and
	// snapshots are mutex-protected and may be called by independent worker jobs.
	class TERRAENGINE_API AnimationMemoryController {
	public:
		AnimationMemoryController() noexcept = default;
		~AnimationMemoryController();
		AnimationMemoryController(const AnimationMemoryController&) = delete;
		AnimationMemoryController& operator=(const AnimationMemoryController&) = delete;

		[[nodiscard]] AnimationMemoryStatus Configure(
			const AnimationMemoryConfiguration& configuration) noexcept;
		[[nodiscard]] AnimationMemoryStatus TryReserve(
			const AnimationMemoryReservationRequest& request,
			AnimationMemoryLease& output) noexcept;
		[[nodiscard]] AnimationMemorySnapshot Snapshot() const noexcept;
		void Shutdown() noexcept;

		// Tests inject failure before any accounting changes. Production paths do
		// not need allocator hooks or machine-wide memory pressure to fail safely.
		void FailNextReservationForTesting() noexcept;

	private:
		std::shared_ptr<AnimationMemoryState> state_;
	};

	struct AnimationArenaAllocation {
		AnimationMemoryStatus status = AnimationMemoryStatus::Uninitialized;
		std::span<std::byte> bytes;
		std::size_t offset = 0U;
		AnimationGeneration generation = 0U;

		[[nodiscard]] explicit operator bool() const noexcept {
			return status == AnimationMemoryStatus::Reserved && !bytes.empty();
		}
	};

	// One arena is owned by one frame range or worker. Configure performs the only
	// backing allocation; BeginGeneration then reuses the bytes without growing.
	class TERRAENGINE_API AnimationFrameArena {
	public:
		[[nodiscard]] AnimationMemoryStatus Configure(std::size_t capacityBytes) noexcept;
		[[nodiscard]] AnimationMemoryStatus BeginGeneration(AnimationGeneration generation) noexcept;
		[[nodiscard]] AnimationArenaAllocation Allocate(
			std::size_t bytes,
			std::size_t alignment = alignof(std::max_align_t)) noexcept;
		void Shutdown() noexcept;
		void FailNextAllocationForTesting() noexcept { failNextAllocation_ = true; }

		[[nodiscard]] std::size_t CapacityBytes() const noexcept { return capacityBytes_; }
		[[nodiscard]] std::size_t BytesInUse() const noexcept { return offsetBytes_; }
		[[nodiscard]] std::size_t HighWaterBytes() const noexcept { return highWaterBytes_; }
		[[nodiscard]] AnimationGeneration Generation() const noexcept { return generation_; }

	private:
		std::unique_ptr<std::byte[]> storage_;
		std::size_t capacityBytes_ = 0U;
		std::size_t offsetBytes_ = 0U;
		std::size_t highWaterBytes_ = 0U;
		AnimationGeneration generation_ = 0U;
		bool configured_ = false;
		bool shutdown_ = false;
		bool failNextAllocation_ = false;
	};

	struct AnimationMemoryRingCounters {
		std::size_t size = 0U;
		std::size_t capacity = 0U;
		std::size_t highWaterEntries = 0U;
		std::uint64_t pushes = 0U;
		std::uint64_t evictions = 0U;
	};

	// Event, history, and diagnostic owners can use the same fixed-capacity ring.
	// T must be nothrow movable so replacing the oldest entry cannot expose a
	// half-written record if a user-defined move operation fails.
	template<typename T>
	class AnimationBoundedMemoryRing {
		static_assert(std::is_nothrow_move_constructible_v<T>);
		static_assert(std::is_nothrow_move_assignable_v<T>);

	public:
		[[nodiscard]] AnimationMemoryStatus Configure(
			std::size_t capacity,
			std::size_t hardMaximum) noexcept
		{
			if(capacity > hardMaximum) {
				return AnimationMemoryStatus::InvalidArgument;
			}

			try {
				std::vector<std::optional<T>> staged(capacity);
				entries_.swap(staged);
			} catch(...) {
				return AnimationMemoryStatus::AllocationFailure;
			}

			head_ = 0U;
			size_ = 0U;
			counters_ = {};
			counters_.capacity = capacity;
			configured_ = true;
			return AnimationMemoryStatus::Reserved;
		}

		[[nodiscard]] AnimationMemoryStatus Push(T value) noexcept
		{
			if(!configured_) {
				return AnimationMemoryStatus::Uninitialized;
			}
			if(entries_.empty()) {
				return AnimationMemoryStatus::BudgetExceeded;
			}

			std::size_t target = (head_ + size_) % entries_.size();
			if(size_ == entries_.size()) {
				target = head_;
				head_ = (head_ + 1U) % entries_.size();
				++counters_.evictions;
			} else {
				++size_;
			}

			entries_[target].reset();
			entries_[target].emplace(std::move(value));
			++counters_.pushes;
			counters_.size = size_;
			if(counters_.highWaterEntries < size_) {
				counters_.highWaterEntries = size_;
			}
			return AnimationMemoryStatus::Reserved;
		}

		void Clear() noexcept
		{
			for(std::optional<T>& entry : entries_) {
				entry.reset();
			}
			head_ = 0U;
			size_ = 0U;
			counters_.size = 0U;
		}

		[[nodiscard]] const T* At(std::size_t logicalIndex) const noexcept
		{
			if(logicalIndex >= size_ || entries_.empty()) {
				return nullptr;
			}
			const std::optional<T>& entry = entries_[(head_ + logicalIndex) % entries_.size()];
			return entry ? std::addressof(*entry) : nullptr;
		}

		[[nodiscard]] const T* Oldest() const noexcept { return At(0U); }
		[[nodiscard]] const T* Newest() const noexcept {
			return size_ == 0U ? nullptr : At(size_ - 1U);
		}
		[[nodiscard]] const AnimationMemoryRingCounters& Counters() const noexcept {
			return counters_;
		}

	private:
		std::vector<std::optional<T>> entries_;
		std::size_t head_ = 0U;
		std::size_t size_ = 0U;
		AnimationMemoryRingCounters counters_{};
		bool configured_ = false;
	};

	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationMemoryClass memoryClass) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationMemoryStatus status) noexcept;
}
