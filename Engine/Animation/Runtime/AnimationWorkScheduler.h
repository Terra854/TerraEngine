// AnimationWorkScheduler.h - Deterministic, range-disjoint animation work planning.
//
// The scheduler in this file only prepares immutable value records. Worker tasks
// may evaluate the resulting ranges in any order, but they never receive access
// to ObjectManager, AnimatorComponent, or another live scene mutation surface.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Runtime/AnimationExecutionMode.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	// Every field that changes evaluator code, source data, outputs, or scratch
	// layout participates in compatibility. A range may contain only one key.
	struct AnimationWorkCompatibilityKey {
		std::uint64_t programGeneration = 0U;
		std::uint64_t skeletonGeneration = 0U;
		std::uint64_t clipBindingGeneration = 0U;
		std::uint64_t outputRequirementsFingerprint = 0U;
		std::uint64_t resourceLayoutFingerprint = 0U;
		AnimationEffectiveExecutionMode effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;

		auto operator<=>(const AnimationWorkCompatibilityKey&) const = default;
	};

	// A producer creates one work item from an immutable frame snapshot. The
	// stable publication ordinal must not depend on worker discovery order.
	struct AnimationWorkItem {
		AnimatorInstanceHandle animator;
		AnimationWorkCompatibilityKey compatibility;
		std::uint64_t stablePublicationOrder = 0U;
		std::uint64_t sourceGeneration = 0U;
		std::size_t scratchBytes = 0U;
	};

	struct AnimationScheduledWorkItem {
		AnimationWorkItem item;
		std::size_t originalInputIndex = 0U;
		std::size_t scratchOffset = 0U;
	};

	// Half-open item and scratch intervals give one worker exclusive ownership of
	// its temporary memory. The protected publisher reads completed values later.
	struct AnimationWorkRange {
		std::size_t itemBegin = 0U;
		std::size_t itemEnd = 0U;
		std::size_t scratchBegin = 0U;
		std::size_t scratchEnd = 0U;
		AnimationWorkCompatibilityKey compatibility;
	};

	enum class AnimationWorkExecutionPolicy : std::uint8_t {
		Omitted = 0U,
		Inline = 1U,
		WorkerTasks = 2U,
		Cancelled = 3U,
		Rejected = 4U
	};

	// Cancellation state is shared by a captured token and its source. This
	// narrowly named, plan-local adapter does not replace AnimationCancellation's
	// lifecycle token/latch: an integrating system polls that richer token and
	// calls RequestCancellation here to stop claims and protected publication.
	// The implementation keeps the atomic details out of the public contract.
	struct AnimationWorkCancellationState;

	class TERRAENGINE_API AnimationWorkCancellationToken {
	public:
		AnimationWorkCancellationToken() = default;

		[[nodiscard]] bool IsCancellationRequested() const noexcept;

	private:
		AnimationWorkCancellationToken(
			std::shared_ptr<AnimationWorkCancellationState> state,
			std::uint64_t generation) noexcept;

		std::shared_ptr<AnimationWorkCancellationState> state_;
		std::uint64_t generation_ = 0U;

		friend class AnimationWorkCancellationSource;
	};

	class TERRAENGINE_API AnimationWorkCancellationSource {
	public:
		AnimationWorkCancellationSource();

		[[nodiscard]] AnimationWorkCancellationToken CaptureToken() const noexcept;
		void RequestCancellation() noexcept;

	private:
		std::shared_ptr<AnimationWorkCancellationState> state_;
	};

	struct AnimationWorkSchedulerConfiguration {
		// Tiny batches stay on the caller because task dispatch would cost more than
		// the bounded evaluation. Their range and scratch rules remain identical.
		std::size_t inlineItemThreshold = 16U;
		std::size_t targetItemsPerRange = 64U;
		std::size_t maximumRangeCount = 256U;
		std::size_t maximumItemCount = 65536U;
		std::size_t maximumScratchBytes = 64U * 1024U * 1024U;
		std::size_t scratchAlignment = 64U;
	};

	struct AnimationWorkPlan {
		AnimationStatus status = AnimationStatus::Ok;
		AnimationWorkExecutionPolicy executionPolicy = AnimationWorkExecutionPolicy::Omitted;
		std::vector<AnimationScheduledWorkItem> items;
		std::vector<AnimationWorkRange> ranges;
		// Each value indexes items. The vector itself is ordered by the stable
		// publication ordinal and is independent from worker completion order.
		std::vector<std::size_t> publicationOrder;
		std::size_t totalScratchBytes = 0U;
		std::size_t selectedWorkerCount = 0U;
		std::size_t scratchAlignment = 1U;
		bool inlineSelected = false;
		AnimationWorkCancellationToken cancellation;
	};

	class TERRAENGINE_API AnimationWorkScheduler {
	public:
		AnimationWorkScheduler() = default;
		explicit AnimationWorkScheduler(AnimationWorkSchedulerConfiguration configuration) noexcept;

		// Planning is deterministic for the same logical items. availableWorkerCount
		// selects inline versus task execution, but never changes range boundaries.
		[[nodiscard]] AnimationWorkPlan BuildPlan(
			std::span<const AnimationWorkItem> items,
			std::size_t availableWorkerCount,
			AnimationWorkCancellationToken cancellation) const;

	private:
		AnimationWorkSchedulerConfiguration configuration_;
	};

	// Revalidation is intentionally public so diagnostics and stress tests can
	// prove that no range overlaps another worker's items or scratch memory.
	[[nodiscard]] TERRAENGINE_API AnimationStatus ValidateAnimationWorkPlan(
		const AnimationWorkPlan& plan);

	struct AnimationWorkCompletion {
		std::size_t scheduledItemIndex = 0U;
		AnimatorInstanceHandle animator;
		std::uint64_t sourceGeneration = 0U;
		AnimationStatus status = AnimationStatus::NotReady;
	};

	// This protected-boundary helper validates a complete batch before swapping
	// the caller's publication list. Rejection and cancellation are atomic.
	[[nodiscard]] TERRAENGINE_API AnimationStatus BuildDeterministicAnimationPublicationOrder(
		const AnimationWorkPlan& plan,
		std::span<const AnimationWorkCompletion> completions,
		std::vector<std::size_t>& output);

	// The ledger grants each prepared range to at most one worker. It tracks only
	// task ownership; actual results remain in caller-owned, range-disjoint memory.
	class TERRAENGINE_API AnimationWorkRangeLedger {
	public:
		explicit AnimationWorkRangeLedger(const AnimationWorkPlan& plan);
		~AnimationWorkRangeLedger();

		AnimationWorkRangeLedger(const AnimationWorkRangeLedger&) = delete;
		AnimationWorkRangeLedger& operator=(const AnimationWorkRangeLedger&) = delete;
		AnimationWorkRangeLedger(AnimationWorkRangeLedger&&) noexcept;
		AnimationWorkRangeLedger& operator=(AnimationWorkRangeLedger&&) noexcept;

		[[nodiscard]] bool TryBegin(std::size_t rangeIndex, std::uint32_t workerOrdinal) noexcept;
		[[nodiscard]] bool Complete(std::size_t rangeIndex, std::uint32_t workerOrdinal) noexcept;
		void CancelUnclaimed() noexcept;
		[[nodiscard]] std::size_t CompletedCount() const noexcept;
		[[nodiscard]] bool AllCompleted() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
