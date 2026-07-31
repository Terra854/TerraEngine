// AnimationHistory.h - Bounded per-animator rollback snapshot rings.

#pragma once

#include "Animation/Determinism/AnimationRollbackSnapshot.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Animation {
	enum class AnimationHistoryPressurePolicy : std::uint8_t {
		EvictOldest = 0U,
		RejectNew = 1U
	};

	struct AnimationHistoryConfiguration {
		std::size_t capacityPerAnimator = kAnimationDeterminismDefaultHistorySnapshots;
		AnimationHistoryPressurePolicy pressurePolicy = AnimationHistoryPressurePolicy::EvictOldest;
	};

	// A result reason keeps an evicted correction distinct from a packet that has
	// not arrived yet. AnimationStatus remains the stable cross-subsystem status.
	enum class AnimationHistoryLookupOutcome : std::uint8_t {
		Found = 0U,
		AnimatorNotRetained = 1U,
		TickNotRetained = 2U,
		TooOld = 3U,
		FingerprintMismatch = 4U,
		GenerationMismatch = 5U,
		CorruptSnapshot = 6U,
		InvalidRequest = 7U
	};

	struct AnimationHistoryLookupResult {
		AnimationStatus status = AnimationStatus::NotReady;
		AnimationHistoryLookupOutcome outcome = AnimationHistoryLookupOutcome::AnimatorNotRetained;
		AnimationFixedTick oldestRetainedTick = 0;
		AnimationFixedTick newestRetainedTick = 0;
		std::uint64_t historyRevision = 0U;
		auto operator<=>(const AnimationHistoryLookupResult&) const = default;
	};

	class TERRAENGINE_API AnimationHistory final {
	public:
		explicit AnimationHistory(AnimationHistoryConfiguration configuration = {});

		// Store accepts only already captured, checksummed values. An animator ring
		// cannot mix asset or generation identities; callers clear it at an explicit
		// compatible publication boundary before starting a new history generation.
		[[nodiscard]] AnimationStatus Store(AnimationRollbackSnapshot snapshot) noexcept;

		[[nodiscard]] AnimationHistoryLookupResult LookupDetailed(
			AnimatorInstanceHandle animator,
			AnimationFixedTick tick,
			const AnimationRollbackRestoreRequirements& requirements,
			AnimationRollbackSnapshot& output) const noexcept;

		// Compatibility lookup retains the existing status-only ImplementationCheck+ API.
		[[nodiscard]] AnimationStatus Lookup(
			AnimatorInstanceHandle animator,
			AnimationFixedTick tick,
			const AnimationDeterministicFingerprint& expectedFingerprint,
			AnimationGeneration expectedPublicationGeneration,
			AnimationRollbackSnapshot& output) const noexcept;

		[[nodiscard]] AnimationStatus LatestAtOrBefore(
			AnimatorInstanceHandle animator,
			AnimationFixedTick tick,
			const AnimationDeterministicFingerprint& expectedFingerprint,
			AnimationRollbackSnapshot& output) const noexcept;

		void Clear(AnimatorInstanceHandle animator) noexcept;
		void ClearAll() noexcept;

		[[nodiscard]] std::uint64_t Revision() const noexcept
		{
			return revision_.load(std::memory_order_acquire);
		}

	private:
		struct SnapshotRing {
			explicit SnapshotRing(std::size_t capacity);

			[[nodiscard]] std::size_t Size() const noexcept { return snapshots.size(); }
			[[nodiscard]] bool Empty() const noexcept { return snapshots.empty(); }
			[[nodiscard]] bool Full() const noexcept { return snapshots.size() == capacity; }
			[[nodiscard]] const AnimationRollbackSnapshot& At(std::size_t logicalIndex) const noexcept;
			[[nodiscard]] AnimationRollbackSnapshot& At(std::size_t logicalIndex) noexcept;
			[[nodiscard]] std::size_t LowerBound(AnimationFixedTick tick) const noexcept;
			[[nodiscard]] std::size_t UpperBound(AnimationFixedTick tick) const noexcept;
			void Append(AnimationRollbackSnapshot snapshot);
			void EvictOldestAndAppend(AnimationRollbackSnapshot snapshot) noexcept;

			std::size_t capacity = 1U;
			std::size_t oldestIndex = 0U;
			std::vector<AnimationRollbackSnapshot> snapshots;
		};

		struct AnimatorKeyHash {
			std::size_t operator()(AnimatorInstanceHandle value) const noexcept;
		};

		AnimationHistoryConfiguration configuration_;
		mutable std::mutex mutex_;
		std::unordered_map<AnimatorInstanceHandle, SnapshotRing, AnimatorKeyHash> records_;
		std::atomic<std::uint64_t> revision_ = 0U;
	};
}
