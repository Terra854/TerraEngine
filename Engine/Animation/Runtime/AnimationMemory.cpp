// AnimationMemory.cpp - Bounded animation allocation accounting and arenas.

#include "Animation/Runtime/AnimationMemory.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		struct AnimationMemoryReservationEntry {
			AnimationMemoryClass memoryClass = AnimationMemoryClass::FrameArena;
			AnimationMemoryRetention retention = AnimationMemoryRetention::Required;
			std::size_t bytes = 0U;
			std::uint64_t ownerGeneration = 0U;
			std::uint32_t retentionPriority = 0U;
			std::uint64_t reservationSerial = 0U;
			bool active = false;
		};

		[[nodiscard]] bool IsValidMemoryClass(AnimationMemoryClass memoryClass) noexcept
		{
			return static_cast<std::size_t>(memoryClass) < kAnimationMemoryClassCount;
		}

		[[nodiscard]] bool IsValidRetention(AnimationMemoryRetention retention) noexcept
		{
			return retention == AnimationMemoryRetention::Required
				|| retention == AnimationMemoryRetention::Evictable;
		}

		[[nodiscard]] std::size_t ClassIndex(AnimationMemoryClass memoryClass) noexcept
		{
			return static_cast<std::size_t>(memoryClass);
		}

		template<typename Value>
		void SaturatingIncrement(Value& value) noexcept
		{
			if(value != std::numeric_limits<Value>::max()) {
				++value;
			}
		}

		template<typename Value, typename Addend>
		void SaturatingAdd(Value& value, Addend addend) noexcept
		{
			const Value converted = addend > static_cast<Addend>(std::numeric_limits<Value>::max())
				? std::numeric_limits<Value>::max()
				: static_cast<Value>(addend);
			if(converted > std::numeric_limits<Value>::max() - value) {
				value = std::numeric_limits<Value>::max();
			} else {
				value += converted;
			}
		}
	}

	// The state is shared by leases so destroying or reconfiguring the controller
	// cannot leave a lease with a dangling owner pointer.
	struct AnimationMemoryState {
		mutable std::mutex mutex;
		AnimationMemoryConfiguration configuration{};
		std::vector<AnimationMemoryReservationEntry> entries;
		// This vector reserves its full capacity during Configure. Pressure planning
		// can therefore remain atomic without allocating while the mutex is held.
		std::vector<std::size_t> evictionScratch;
		AnimationMemorySnapshot snapshot{};
		std::uint64_t nextReservationSerial = 1U;
		bool failNextReservation = false;
	};

	namespace {
		void RecordRejection(
			AnimationMemoryState& state,
			AnimationMemoryClass memoryClass,
			std::size_t bytes) noexcept
		{
			AnimationMemoryClassCounters& counters = state.snapshot.classes[ClassIndex(memoryClass)];
			SaturatingIncrement(counters.rejectedRequests);
			SaturatingAdd(counters.rejectedBytes, bytes);
		}

		void RemoveEntryAccounting(
			AnimationMemoryState& state,
			AnimationMemoryReservationEntry& entry,
			bool evicted) noexcept
		{
			AnimationMemoryClassCounters& counters =
				state.snapshot.classes[ClassIndex(entry.memoryClass)];
			counters.bytesInUse -= entry.bytes;
			state.snapshot.totalBytesInUse -= entry.bytes;
			--state.snapshot.activeReservations;
			if(evicted) {
				SaturatingIncrement(counters.evictions);
				SaturatingAdd(counters.evictedBytes, entry.bytes);
			} else {
				SaturatingIncrement(counters.releases);
			}
			entry.active = false;
		}

		[[nodiscard]] bool IsSelected(
			const AnimationMemoryState& state,
			std::size_t entryIndex) noexcept
		{
			return std::find(
				state.evictionScratch.begin(),
				state.evictionScratch.end(),
				entryIndex) != state.evictionScratch.end();
		}

		[[nodiscard]] std::optional<std::size_t> FindEvictionCandidate(
			const AnimationMemoryState& state,
			const AnimationMemoryReservationRequest& request,
			std::optional<AnimationMemoryClass> requiredClass) noexcept
		{
			std::optional<std::size_t> best;
			for(std::size_t index = 0U; index < state.entries.size(); ++index) {
				const AnimationMemoryReservationEntry& entry = state.entries[index];
				if(!entry.active
					|| entry.retention != AnimationMemoryRetention::Evictable
					|| IsSelected(state, index)
					|| (requiredClass && entry.memoryClass != *requiredClass)) {
					continue;
				}
				// Optional work cannot evict a more important optional allocation.
				// Required work may evict any optional entry to preserve correctness.
				if(request.retention == AnimationMemoryRetention::Evictable
					&& entry.retentionPriority > request.retentionPriority) {
					continue;
				}
				if(!best) {
					best = index;
					continue;
				}

				const AnimationMemoryReservationEntry& current = state.entries[*best];
				if(entry.retentionPriority < current.retentionPriority
					|| (entry.retentionPriority == current.retentionPriority
						&& entry.reservationSerial < current.reservationSerial)) {
					best = index;
				}
			}
			return best;
		}

		[[nodiscard]] bool PlanEvictions(
			AnimationMemoryState& state,
			const AnimationMemoryReservationRequest& request) noexcept
		{
			state.evictionScratch.clear();
			const std::size_t classIndex = ClassIndex(request.memoryClass);
			const std::size_t classLimit = state.configuration.classes[classIndex].maximumBytes;
			std::size_t freedFromClass = 0U;
			std::size_t freedTotal = 0U;

			// First make the requested class fit its own cap. Choosing only entries
			// from that class avoids evicting unrelated data that cannot help.
			while(request.bytes > classLimit - (state.snapshot.classes[classIndex].bytesInUse - freedFromClass)) {
				const std::optional<std::size_t> candidate =
					FindEvictionCandidate(state, request, request.memoryClass);
				if(!candidate) {
					return false;
				}
				state.evictionScratch.push_back(*candidate);
				freedFromClass += state.entries[*candidate].bytes;
				freedTotal += state.entries[*candidate].bytes;
			}

			// The global cap can be satisfied by any caller-declared optional data.
			while(request.bytes > state.configuration.maximumTotalBytes
				- (state.snapshot.totalBytesInUse - freedTotal)) {
				const std::optional<std::size_t> candidate =
					FindEvictionCandidate(state, request, std::nullopt);
				if(!candidate) {
					return false;
				}
				state.evictionScratch.push_back(*candidate);
				freedTotal += state.entries[*candidate].bytes;
				if(state.entries[*candidate].memoryClass == request.memoryClass) {
					freedFromClass += state.entries[*candidate].bytes;
				}
			}

			// A full reservation table also needs one optional slot to be retired.
			const bool hasFreeSlot = std::any_of(
				state.entries.begin(),
				state.entries.end(),
				[](const AnimationMemoryReservationEntry& entry) { return !entry.active; });
			if(!hasFreeSlot && state.evictionScratch.empty()) {
				const std::optional<std::size_t> candidate =
					FindEvictionCandidate(state, request, std::nullopt);
				if(!candidate) {
					return false;
				}
				state.evictionScratch.push_back(*candidate);
			}
			return true;
		}
	}

	AnimationMemoryLease::AnimationMemoryLease(
		std::shared_ptr<AnimationMemoryState> state,
		std::size_t slotIndex,
		std::uint64_t reservationSerial,
		const AnimationMemoryReservationRequest& request) noexcept
		: state_(std::move(state))
		, slotIndex_(slotIndex)
		, reservationSerial_(reservationSerial)
		, memoryClass_(request.memoryClass)
		, bytes_(request.bytes)
		, ownerGeneration_(request.ownerGeneration)
	{
	}

	AnimationMemoryLease::~AnimationMemoryLease()
	{
		Release();
	}

	AnimationMemoryLease::AnimationMemoryLease(AnimationMemoryLease&& other) noexcept
		: state_(std::move(other.state_))
		, slotIndex_(std::exchange(other.slotIndex_, 0U))
		, reservationSerial_(std::exchange(other.reservationSerial_, 0U))
		, memoryClass_(other.memoryClass_)
		, bytes_(std::exchange(other.bytes_, 0U))
		, ownerGeneration_(std::exchange(other.ownerGeneration_, 0U))
	{
	}

	AnimationMemoryLease& AnimationMemoryLease::operator=(AnimationMemoryLease&& other) noexcept
	{
		if(this == &other) {
			return *this;
		}
		Release();
		state_ = std::move(other.state_);
		slotIndex_ = std::exchange(other.slotIndex_, 0U);
		reservationSerial_ = std::exchange(other.reservationSerial_, 0U);
		memoryClass_ = other.memoryClass_;
		bytes_ = std::exchange(other.bytes_, 0U);
		ownerGeneration_ = std::exchange(other.ownerGeneration_, 0U);
		return *this;
	}

	void AnimationMemoryLease::Release() noexcept
	{
		if(!state_ || reservationSerial_ == 0U) {
			state_.reset();
			return;
		}

		{
			std::lock_guard lock(state_->mutex);
			if(slotIndex_ < state_->entries.size()) {
				AnimationMemoryReservationEntry& entry = state_->entries[slotIndex_];
				if(entry.active && entry.reservationSerial == reservationSerial_) {
					RemoveEntryAccounting(*state_, entry, false);
				}
			}
		}

		state_.reset();
		reservationSerial_ = 0U;
		bytes_ = 0U;
		ownerGeneration_ = 0U;
	}

	bool AnimationMemoryLease::IsActive() const noexcept
	{
		if(!state_ || reservationSerial_ == 0U) {
			return false;
		}
		std::lock_guard lock(state_->mutex);
		return !state_->snapshot.shutdown
			&& slotIndex_ < state_->entries.size()
			&& state_->entries[slotIndex_].active
			&& state_->entries[slotIndex_].reservationSerial == reservationSerial_;
	}

	AnimationMemoryController::~AnimationMemoryController()
	{
		Shutdown();
	}

	AnimationMemoryStatus AnimationMemoryController::Configure(
		const AnimationMemoryConfiguration& configuration) noexcept
	{
		if(configuration.maximumReservations > kAnimationMemoryMaximumReservations) {
			return AnimationMemoryStatus::InvalidArgument;
		}

		std::shared_ptr<AnimationMemoryState> staged;
		try {
			staged = std::make_shared<AnimationMemoryState>();
			staged->configuration = configuration;
			staged->entries.resize(configuration.maximumReservations);
			staged->evictionScratch.reserve(configuration.maximumReservations);
			staged->snapshot.configured = true;
		} catch(...) {
			return AnimationMemoryStatus::AllocationFailure;
		}

		Shutdown();
		state_ = std::move(staged);
		return AnimationMemoryStatus::Reserved;
	}

	AnimationMemoryStatus AnimationMemoryController::TryReserve(
		const AnimationMemoryReservationRequest& request,
		AnimationMemoryLease& output) noexcept
	{
		if(!state_) {
			return AnimationMemoryStatus::Uninitialized;
		}
		if(!IsValidMemoryClass(request.memoryClass)
			|| !IsValidRetention(request.retention)
			|| request.bytes == 0U
			|| request.ownerGeneration == 0U
			|| output.IsActive()) {
			return AnimationMemoryStatus::InvalidArgument;
		}
		// An inactive evicted lease may be reused as an output without retaining its
		// old shared state. Release performs no accounting when its serial is stale.
		output.Release();

		std::lock_guard lock(state_->mutex);
		if(state_->snapshot.shutdown) {
			return AnimationMemoryStatus::Shutdown;
		}

		const std::size_t classIndex = ClassIndex(request.memoryClass);
		if(state_->failNextReservation) {
			state_->failNextReservation = false;
			SaturatingIncrement(state_->snapshot.allocationFailures);
			RecordRejection(*state_, request.memoryClass, request.bytes);
			return AnimationMemoryStatus::AllocationFailure;
		}
		if(request.bytes > state_->configuration.classes[classIndex].maximumBytes
			|| request.bytes > state_->configuration.maximumTotalBytes) {
			RecordRejection(*state_, request.memoryClass, request.bytes);
			return AnimationMemoryStatus::BudgetExceeded;
		}
		if(state_->nextReservationSerial == 0U) {
			RecordRejection(*state_, request.memoryClass, request.bytes);
			return AnimationMemoryStatus::ReservationTableFull;
		}

		const bool needsClassPressure = request.bytes
			> state_->configuration.classes[classIndex].maximumBytes
				- state_->snapshot.classes[classIndex].bytesInUse;
		const bool needsTotalPressure = request.bytes
			> state_->configuration.maximumTotalBytes - state_->snapshot.totalBytesInUse;
		const bool tableFull = state_->snapshot.activeReservations == state_->entries.size();
		if(needsClassPressure || needsTotalPressure || tableFull) {
			SaturatingIncrement(state_->snapshot.pressureEvents);
		}

		if(!PlanEvictions(*state_, request)) {
			RecordRejection(*state_, request.memoryClass, request.bytes);
			return tableFull && !needsClassPressure && !needsTotalPressure
				? AnimationMemoryStatus::ReservationTableFull
				: AnimationMemoryStatus::BudgetExceeded;
		}

		// Eviction was planned completely before this loop. A rejected request never
		// partially removes optional live state.
		for(const std::size_t entryIndex : state_->evictionScratch) {
			RemoveEntryAccounting(*state_, state_->entries[entryIndex], true);
		}

		const auto freeEntry = std::find_if(
			state_->entries.begin(),
			state_->entries.end(),
			[](const AnimationMemoryReservationEntry& entry) { return !entry.active; });
		if(freeEntry == state_->entries.end()) {
			// PlanEvictions proves this branch unreachable, but retain a fail-safe
			// result in case a future table implementation violates that invariant.
			RecordRejection(*state_, request.memoryClass, request.bytes);
			return AnimationMemoryStatus::InternalError;
		}

		const std::size_t slotIndex = static_cast<std::size_t>(freeEntry - state_->entries.begin());
		const std::uint64_t reservationSerial = state_->nextReservationSerial++;
		freeEntry->memoryClass = request.memoryClass;
		freeEntry->retention = request.retention;
		freeEntry->bytes = request.bytes;
		freeEntry->ownerGeneration = request.ownerGeneration;
		freeEntry->retentionPriority = request.retentionPriority;
		freeEntry->reservationSerial = reservationSerial;
		freeEntry->active = true;

		AnimationMemoryClassCounters& counters = state_->snapshot.classes[classIndex];
		counters.bytesInUse += request.bytes;
		counters.highWaterBytes = std::max(counters.highWaterBytes, counters.bytesInUse);
		SaturatingIncrement(counters.successfulReservations);
		state_->snapshot.totalBytesInUse += request.bytes;
		state_->snapshot.totalHighWaterBytes = std::max(
			state_->snapshot.totalHighWaterBytes,
			state_->snapshot.totalBytesInUse);
		++state_->snapshot.activeReservations;
		output = AnimationMemoryLease(state_, slotIndex, reservationSerial, request);
		return AnimationMemoryStatus::Reserved;
	}

	AnimationMemorySnapshot AnimationMemoryController::Snapshot() const noexcept
	{
		if(!state_) {
			return {};
		}
		std::lock_guard lock(state_->mutex);
		return state_->snapshot;
	}

	void AnimationMemoryController::Shutdown() noexcept
	{
		if(!state_) {
			return;
		}
		std::lock_guard lock(state_->mutex);
		if(state_->snapshot.shutdown) {
			return;
		}
		for(AnimationMemoryReservationEntry& entry : state_->entries) {
			if(entry.active) {
				RemoveEntryAccounting(*state_, entry, true);
			}
		}
		state_->snapshot.shutdown = true;
	}

	void AnimationMemoryController::FailNextReservationForTesting() noexcept
	{
		if(!state_) {
			return;
		}
		std::lock_guard lock(state_->mutex);
		state_->failNextReservation = true;
	}

	AnimationMemoryStatus AnimationFrameArena::Configure(std::size_t capacityBytes) noexcept
	{
		if(capacityBytes > kAnimationFrameArenaHardMaximumBytes) {
			return AnimationMemoryStatus::InvalidArgument;
		}

		std::unique_ptr<std::byte[]> staged;
		if(capacityBytes != 0U) {
			staged.reset(new(std::nothrow) std::byte[capacityBytes]);
			if(!staged) {
				return AnimationMemoryStatus::AllocationFailure;
			}
		}

		storage_ = std::move(staged);
		capacityBytes_ = capacityBytes;
		offsetBytes_ = 0U;
		highWaterBytes_ = 0U;
		generation_ = 0U;
		configured_ = true;
		shutdown_ = false;
		failNextAllocation_ = false;
		return AnimationMemoryStatus::Reserved;
	}

	AnimationMemoryStatus AnimationFrameArena::BeginGeneration(AnimationGeneration generation) noexcept
	{
		if(!configured_) {
			return AnimationMemoryStatus::Uninitialized;
		}
		if(shutdown_) {
			return AnimationMemoryStatus::Shutdown;
		}
		if(generation == 0U) {
			return AnimationMemoryStatus::InvalidArgument;
		}
		offsetBytes_ = 0U;
		generation_ = generation;
		return AnimationMemoryStatus::Reserved;
	}

	AnimationArenaAllocation AnimationFrameArena::Allocate(
		std::size_t bytes,
		std::size_t alignment) noexcept
	{
		if(!configured_ || generation_ == 0U) {
			return {AnimationMemoryStatus::Uninitialized, {}, 0U, generation_};
		}
		if(shutdown_) {
			return {AnimationMemoryStatus::Shutdown, {}, 0U, generation_};
		}
		if(bytes == 0U || alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
			return {AnimationMemoryStatus::InvalidArgument, {}, 0U, generation_};
		}
		if(failNextAllocation_) {
			failNextAllocation_ = false;
			return {AnimationMemoryStatus::AllocationFailure, {}, 0U, generation_};
		}

		// Align the absolute address rather than only the arena-relative offset.
		// This also supports requests above max_align_t when sufficient padding is
		// available in the fixed backing allocation.
		const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(storage_.get());
		if(offsetBytes_ > std::numeric_limits<std::uintptr_t>::max() - baseAddress) {
			return {AnimationMemoryStatus::BudgetExceeded, {}, 0U, generation_};
		}
		const std::uintptr_t currentAddress = baseAddress + offsetBytes_;
		if(alignment - 1U > std::numeric_limits<std::uintptr_t>::max() - currentAddress) {
			return {AnimationMemoryStatus::BudgetExceeded, {}, 0U, generation_};
		}
		const std::uintptr_t alignedAddress =
			(currentAddress + alignment - 1U) & ~static_cast<std::uintptr_t>(alignment - 1U);
		const std::size_t padding = static_cast<std::size_t>(alignedAddress - currentAddress);
		if(padding > capacityBytes_ - offsetBytes_
			|| bytes > capacityBytes_ - offsetBytes_ - padding) {
			return {AnimationMemoryStatus::BudgetExceeded, {}, 0U, generation_};
		}

		const std::size_t alignedOffset = offsetBytes_ + padding;
		offsetBytes_ = alignedOffset + bytes;
		highWaterBytes_ = std::max(highWaterBytes_, offsetBytes_);
		return {
			AnimationMemoryStatus::Reserved,
			std::span<std::byte>(storage_.get() + alignedOffset, bytes),
			alignedOffset,
			generation_};
	}

	void AnimationFrameArena::Shutdown() noexcept
	{
		storage_.reset();
		capacityBytes_ = 0U;
		offsetBytes_ = 0U;
		generation_ = 0U;
		shutdown_ = true;
	}

	std::string_view ToString(AnimationMemoryClass memoryClass) noexcept
	{
		switch(memoryClass) {
		case AnimationMemoryClass::FrameArena: return "FrameArena";
		case AnimationMemoryClass::PosePool: return "PosePool";
		case AnimationMemoryClass::ScratchPool: return "ScratchPool";
		case AnimationMemoryClass::EventRing: return "EventRing";
		case AnimationMemoryClass::HistoryRing: return "HistoryRing";
		case AnimationMemoryClass::DiagnosticRing: return "DiagnosticRing";
		case AnimationMemoryClass::Residency: return "Residency";
		case AnimationMemoryClass::Count: break;
		}
		return "InvalidAnimationMemoryClass";
	}

	std::string_view ToString(AnimationMemoryStatus status) noexcept
	{
		switch(status) {
		case AnimationMemoryStatus::Reserved: return "Reserved";
		case AnimationMemoryStatus::Uninitialized: return "Uninitialized";
		case AnimationMemoryStatus::InvalidArgument: return "InvalidArgument";
		case AnimationMemoryStatus::BudgetExceeded: return "BudgetExceeded";
		case AnimationMemoryStatus::ReservationTableFull: return "ReservationTableFull";
		case AnimationMemoryStatus::AllocationFailure: return "AllocationFailure";
		case AnimationMemoryStatus::Shutdown: return "Shutdown";
		case AnimationMemoryStatus::InternalError: return "InternalError";
		}
		return "InvalidAnimationMemoryStatus";
	}
}
