// AnimationHistory.cpp - Thread-safe fixed-capacity rollback history rings.

#include "Animation/Determinism/AnimationHistory.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace TerraEngine::Animation {
	namespace {
		[[nodiscard]] bool HasValidIntegrity(const AnimationRollbackSnapshot& snapshot) noexcept
		{
			return snapshot.stateChecksum == ComputeDeterministicStateChecksum(snapshot)
				&& snapshot.poseChecksum == ComputeDeterministicSnapshotChecksum(snapshot);
		}

		[[nodiscard]] AnimationHistoryLookupResult Result(
			AnimationStatus status,
			AnimationHistoryLookupOutcome outcome,
			AnimationFixedTick oldest,
			AnimationFixedTick newest,
			std::uint64_t revision) noexcept
		{
			return {status, outcome, oldest, newest, revision};
		}
	}

	AnimationHistory::SnapshotRing::SnapshotRing(std::size_t requestedCapacity)
		: capacity(requestedCapacity)
	{
		// Reserve once when the animator opts into history. Appends before pressure
		// cannot repeatedly grow the outer storage, and deep snapshot allocations
		// remain bounded by the validated per-domain limits.
		snapshots.reserve(capacity);
	}

	const AnimationRollbackSnapshot& AnimationHistory::SnapshotRing::At(
		std::size_t logicalIndex) const noexcept
	{
		return snapshots[(oldestIndex + logicalIndex) % capacity];
	}

	AnimationRollbackSnapshot& AnimationHistory::SnapshotRing::At(
		std::size_t logicalIndex) noexcept
	{
		return snapshots[(oldestIndex + logicalIndex) % capacity];
	}

	std::size_t AnimationHistory::SnapshotRing::LowerBound(AnimationFixedTick tick) const noexcept
	{
		std::size_t first = 0U;
		std::size_t count = Size();
		while(count != 0U) {
			const std::size_t step = count / 2U;
			const std::size_t candidate = first + step;
			if(At(candidate).tick < tick) {
				first = candidate + 1U;
				count -= step + 1U;
			} else {
				count = step;
			}
		}
		return first;
	}

	std::size_t AnimationHistory::SnapshotRing::UpperBound(AnimationFixedTick tick) const noexcept
	{
		std::size_t first = 0U;
		std::size_t count = Size();
		while(count != 0U) {
			const std::size_t step = count / 2U;
			const std::size_t candidate = first + step;
			if(tick >= At(candidate).tick) {
				first = candidate + 1U;
				count -= step + 1U;
			} else {
				count = step;
			}
		}
		return first;
	}

	void AnimationHistory::SnapshotRing::Append(AnimationRollbackSnapshot snapshot)
	{
		// Before the ring becomes full, physical and logical order are identical.
		// After it is full, Append is never used; pressure overwrites oldestIndex.
		snapshots.push_back(std::move(snapshot));
	}

	void AnimationHistory::SnapshotRing::EvictOldestAndAppend(
		AnimationRollbackSnapshot snapshot) noexcept
	{
		snapshots[oldestIndex] = std::move(snapshot);
		oldestIndex = (oldestIndex + 1U) % capacity;
	}

	AnimationHistory::AnimationHistory(AnimationHistoryConfiguration configuration)
		: configuration_(configuration)
	{
		// Construction cannot return a status, so normalize a caller's zero or
		// oversized request to the frozen supported interval.
		configuration_.capacityPerAnimator = std::clamp(
			configuration_.capacityPerAnimator,
			std::size_t{1U},
			kAnimationDeterminismMaximumHistorySnapshots);
		if(configuration_.pressurePolicy != AnimationHistoryPressurePolicy::EvictOldest
			&& configuration_.pressurePolicy != AnimationHistoryPressurePolicy::RejectNew) {
			configuration_.pressurePolicy = AnimationHistoryPressurePolicy::EvictOldest;
		}
	}

	std::size_t AnimationHistory::AnimatorKeyHash::operator()(
		AnimatorInstanceHandle value) const noexcept
	{
		return (static_cast<std::size_t>(value.slotIndex) << 32U)
			^ static_cast<std::size_t>(value.generation);
	}

	AnimationStatus AnimationHistory::Store(AnimationRollbackSnapshot snapshot) noexcept
	{
		const AnimationStatus validation = ValidateRollbackSnapshot(snapshot);
		if(validation != AnimationStatus::Ok) {
			return validation;
		}
		if(!HasValidIntegrity(snapshot)) {
			return AnimationStatus::ContentHashMismatch;
		}

		try {
			std::scoped_lock lock(mutex_);
			auto found = records_.find(snapshot.animator);
			if(found == records_.end()) {
				found = records_.try_emplace(
					snapshot.animator,
					configuration_.capacityPerAnimator).first;
			}
			SnapshotRing& ring = found->second;
			if(ring.Empty()) {
				ring.Append(std::move(snapshot));
				revision_.fetch_add(1U, std::memory_order_release);
				return AnimationStatus::Ok;
			}

			const AnimationRollbackSnapshot& newest = ring.At(ring.Size() - 1U);
			// One ring represents one compatible publication. A hot reload must
			// explicitly clear/start a new ring so a stale remote snapshot cannot
			// silently replace retained last-good state.
			if(snapshot.fingerprint != newest.fingerprint) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(snapshot.publicationGeneration != newest.publicationGeneration
				|| snapshot.generations != newest.generations) {
				return AnimationStatus::StaleHandle;
			}

			const std::size_t position = ring.LowerBound(snapshot.tick);
			if(position < ring.Size() && ring.At(position).tick == snapshot.tick) {
				ring.At(position) = std::move(snapshot);
				revision_.fetch_add(1U, std::memory_order_release);
				return AnimationStatus::Ok;
			}
			// Fixed-step capture is monotonic. Rejecting an out-of-order hole keeps
			// the ring's binary-search and too-old classification deterministic.
			if(snapshot.tick < newest.tick) {
				return AnimationStatus::Rejected;
			}
			if(!ring.Full()) {
				ring.Append(std::move(snapshot));
			} else if(configuration_.pressurePolicy == AnimationHistoryPressurePolicy::RejectNew) {
				return AnimationStatus::HardLimitExceeded;
			} else {
				ring.EvictOldestAndAppend(std::move(snapshot));
			}
			revision_.fetch_add(1U, std::memory_order_release);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationHistoryLookupResult AnimationHistory::LookupDetailed(
		AnimatorInstanceHandle animator,
		AnimationFixedTick tick,
		const AnimationRollbackRestoreRequirements& requirements,
		AnimationRollbackSnapshot& output) const noexcept
	{
		const std::uint64_t revision = revision_.load(std::memory_order_acquire);
		if(!animator.IsValid() || tick < 0 || requirements.animator != animator
			|| !requirements.fingerprint.IsValid() || requirements.publicationGeneration == 0U
			|| !requirements.generations.IsValid()) {
			return Result(AnimationStatus::InvalidArgument,
				AnimationHistoryLookupOutcome::InvalidRequest, 0, 0, revision);
		}

		try {
			std::scoped_lock lock(mutex_);
			const auto found = records_.find(animator);
			if(found == records_.end() || found->second.Empty()) {
				return Result(AnimationStatus::NotReady,
					AnimationHistoryLookupOutcome::AnimatorNotRetained, 0, 0, revision);
			}
			const SnapshotRing& ring = found->second;
			const AnimationFixedTick oldest = ring.At(0U).tick;
			const AnimationFixedTick newest = ring.At(ring.Size() - 1U).tick;
			if(tick < oldest) {
				return Result(AnimationStatus::RangeOutOfBounds,
					AnimationHistoryLookupOutcome::TooOld, oldest, newest, revision);
			}
			const std::size_t position = ring.LowerBound(tick);
			if(position == ring.Size() || ring.At(position).tick != tick) {
				return Result(AnimationStatus::NotReady,
					AnimationHistoryLookupOutcome::TickNotRetained, oldest, newest, revision);
			}
			const AnimationRollbackSnapshot& candidate = ring.At(position);
			if(candidate.fingerprint != requirements.fingerprint) {
				return Result(AnimationStatus::IncompatibleAsset,
					AnimationHistoryLookupOutcome::FingerprintMismatch, oldest, newest, revision);
			}
			if(candidate.publicationGeneration != requirements.publicationGeneration
				|| candidate.generations != requirements.generations) {
				return Result(AnimationStatus::StaleHandle,
					AnimationHistoryLookupOutcome::GenerationMismatch, oldest, newest, revision);
			}
			if(!HasValidIntegrity(candidate)) {
				return Result(AnimationStatus::ContentHashMismatch,
					AnimationHistoryLookupOutcome::CorruptSnapshot, oldest, newest, revision);
			}
			AnimationRollbackSnapshot staged = candidate;
			output = std::move(staged);
			return Result(AnimationStatus::Ok,
				AnimationHistoryLookupOutcome::Found, oldest, newest, revision);
		} catch(const std::bad_alloc&) {
			return Result(AnimationStatus::HardLimitExceeded,
				AnimationHistoryLookupOutcome::InvalidRequest, 0, 0, revision);
		} catch(const std::length_error&) {
			return Result(AnimationStatus::HardLimitExceeded,
				AnimationHistoryLookupOutcome::InvalidRequest, 0, 0, revision);
		}
	}

	AnimationStatus AnimationHistory::Lookup(
		AnimatorInstanceHandle animator,
		AnimationFixedTick tick,
		const AnimationDeterministicFingerprint& expectedFingerprint,
		AnimationGeneration expectedPublicationGeneration,
		AnimationRollbackSnapshot& output) const noexcept
	{
		if(!animator.IsValid() || tick < 0 || !expectedFingerprint.IsValid()
			|| expectedPublicationGeneration == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		try {
			std::scoped_lock lock(mutex_);
			const auto found = records_.find(animator);
			if(found == records_.end() || found->second.Empty()) {
				return AnimationStatus::NotReady;
			}
			const SnapshotRing& ring = found->second;
			if(tick < ring.At(0U).tick) {
				return AnimationStatus::RangeOutOfBounds;
			}
			const std::size_t position = ring.LowerBound(tick);
			if(position == ring.Size() || ring.At(position).tick != tick) {
				return AnimationStatus::NotReady;
			}
			const AnimationRollbackSnapshot& candidate = ring.At(position);
			if(candidate.fingerprint != expectedFingerprint) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(candidate.publicationGeneration != expectedPublicationGeneration) {
				return AnimationStatus::StaleHandle;
			}
			if(!HasValidIntegrity(candidate)) {
				return AnimationStatus::ContentHashMismatch;
			}
			AnimationRollbackSnapshot staged = candidate;
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AnimationHistory::LatestAtOrBefore(
		AnimatorInstanceHandle animator,
		AnimationFixedTick tick,
		const AnimationDeterministicFingerprint& expectedFingerprint,
		AnimationRollbackSnapshot& output) const noexcept
	{
		if(!animator.IsValid() || tick < 0 || !expectedFingerprint.IsValid()) {
			return AnimationStatus::InvalidArgument;
		}
		try {
			std::scoped_lock lock(mutex_);
			const auto found = records_.find(animator);
			if(found == records_.end() || found->second.Empty()) {
				return AnimationStatus::NotReady;
			}
			const SnapshotRing& ring = found->second;
			const std::size_t firstAfter = ring.UpperBound(tick);
			if(firstAfter == 0U) {
				return AnimationStatus::RangeOutOfBounds;
			}
			const AnimationRollbackSnapshot& candidate = ring.At(firstAfter - 1U);
			if(candidate.fingerprint != expectedFingerprint) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(!HasValidIntegrity(candidate)) {
				return AnimationStatus::ContentHashMismatch;
			}
			AnimationRollbackSnapshot staged = candidate;
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	void AnimationHistory::Clear(AnimatorInstanceHandle animator) noexcept
	{
		std::scoped_lock lock(mutex_);
		if(records_.erase(animator) != 0U) {
			revision_.fetch_add(1U, std::memory_order_release);
		}
	}

	void AnimationHistory::ClearMany(
		std::span<const AnimatorInstanceHandle> animators) noexcept
	{
		if(animators.empty()) return;
		std::scoped_lock lock(mutex_);
		bool changed = false;
		for(const AnimatorInstanceHandle animator : animators) {
			changed = records_.erase(animator) != 0U || changed;
		}
		if(changed) {
			revision_.fetch_add(1U, std::memory_order_acq_rel);
		}
	}

	void AnimationHistory::ClearAll() noexcept
	{
		std::scoped_lock lock(mutex_);
		if(!records_.empty()) {
			records_.clear();
			revision_.fetch_add(1U, std::memory_order_release);
		}
	}
}
