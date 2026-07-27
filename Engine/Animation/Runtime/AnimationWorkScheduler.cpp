// AnimationWorkScheduler.cpp - Pure planning and task-ownership implementation.

#include "Animation/Runtime/AnimationWorkScheduler.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <numeric>
#include <tuple>
#include <utility>

namespace TerraEngine::Animation {
	struct AnimationWorkCancellationState {
		std::atomic_uint64_t generation = 1U;
		std::atomic_bool cancellationRequested = false;
	};

	namespace {
		inline constexpr std::size_t kAbsoluteMaximumAnimationWorkItems = 1048576U;
		inline constexpr std::size_t kAbsoluteMaximumAnimationWorkRanges = 65536U;
		inline constexpr std::size_t kAbsoluteMaximumScratchAlignment = 4096U;

		bool IsPowerOfTwo(std::size_t value) noexcept
		{
			return value != 0U && (value & (value - 1U)) == 0U;
		}

		bool TryAlignUp(std::size_t value, std::size_t alignment, std::size_t& output) noexcept
		{
			const std::size_t remainder = value % alignment;
			const std::size_t padding = remainder == 0U ? 0U : alignment - remainder;
			if(value > std::numeric_limits<std::size_t>::max() - padding) return false;
			output = value + padding;
			return true;
		}

		bool IsValidCompatibilityKey(const AnimationWorkCompatibilityKey& key) noexcept
		{
			const std::uint8_t mode = static_cast<std::uint8_t>(key.effectiveMode);
			return key.programGeneration != 0U
				&& key.skeletonGeneration != 0U
				&& key.clipBindingGeneration != 0U
				&& key.outputRequirementsFingerprint != 0U
				&& key.resourceLayoutFingerprint != 0U
				&& mode <= static_cast<std::uint8_t>(AnimationEffectiveExecutionMode::CpuDeterministicFallback);
		}

		AnimationStatus ValidateConfiguration(
			const AnimationWorkSchedulerConfiguration& configuration) noexcept
		{
			if(configuration.targetItemsPerRange == 0U
				|| configuration.maximumRangeCount == 0U
				|| configuration.maximumItemCount == 0U
				|| configuration.maximumScratchBytes == 0U
				|| !IsPowerOfTwo(configuration.scratchAlignment)) {
				return AnimationStatus::InvalidArgument;
			}
			if(configuration.maximumItemCount > kAbsoluteMaximumAnimationWorkItems
				|| configuration.maximumRangeCount > kAbsoluteMaximumAnimationWorkRanges
				|| configuration.scratchAlignment > kAbsoluteMaximumScratchAlignment) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		AnimationWorkPlan MakeRejectedPlan(
			AnimationStatus status,
			std::size_t scratchAlignment,
			AnimationWorkCancellationToken cancellation)
		{
			AnimationWorkPlan plan;
			plan.status = status;
			plan.executionPolicy = status == AnimationStatus::Cancelled
				? AnimationWorkExecutionPolicy::Cancelled
				: AnimationWorkExecutionPolicy::Rejected;
			plan.scratchAlignment = scratchAlignment;
			plan.cancellation = std::move(cancellation);
			return plan;
		}

		struct WorkGroup {
			std::size_t begin = 0U;
			std::size_t end = 0U;
			std::size_t desiredRangeCount = 1U;
			std::size_t assignedRangeCount = 1U;
		};

		// This split calculation avoids multiplying a potentially large group size
		// by the part ordinal. Remainder items are assigned to the earliest ranges.
		std::size_t PartitionBoundary(
			std::size_t groupBegin,
			std::size_t groupSize,
			std::size_t part,
			std::size_t partCount) noexcept
		{
			const std::size_t baseSize = groupSize / partCount;
			const std::size_t remainder = groupSize % partCount;
			return groupBegin + baseSize * part + std::min(part, remainder);
		}
	}

	AnimationWorkCancellationToken::AnimationWorkCancellationToken(
		std::shared_ptr<AnimationWorkCancellationState> state,
		std::uint64_t generation) noexcept
		: state_(std::move(state)), generation_(generation)
	{
	}

	bool AnimationWorkCancellationToken::IsCancellationRequested() const noexcept
	{
		if(state_ == nullptr) return false;
		return state_->cancellationRequested.load(std::memory_order_acquire)
			|| state_->generation.load(std::memory_order_acquire) != generation_;
	}

	AnimationWorkCancellationSource::AnimationWorkCancellationSource()
		: state_(std::make_shared<AnimationWorkCancellationState>())
	{
	}

	AnimationWorkCancellationToken AnimationWorkCancellationSource::CaptureToken() const noexcept
	{
		return AnimationWorkCancellationToken(
			state_, state_->generation.load(std::memory_order_acquire));
	}

	void AnimationWorkCancellationSource::RequestCancellation() noexcept
	{
		state_->cancellationRequested.store(true, std::memory_order_release);
	}

	AnimationWorkScheduler::AnimationWorkScheduler(
		AnimationWorkSchedulerConfiguration configuration) noexcept
		: configuration_(configuration)
	{
	}

	AnimationWorkPlan AnimationWorkScheduler::BuildPlan(
		std::span<const AnimationWorkItem> sourceItems,
		std::size_t availableWorkerCount,
		AnimationWorkCancellationToken cancellation) const try
	{
		const AnimationStatus configurationStatus = ValidateConfiguration(configuration_);
		if(configurationStatus != AnimationStatus::Ok) {
			return MakeRejectedPlan(configurationStatus, configuration_.scratchAlignment, std::move(cancellation));
		}
		if(cancellation.IsCancellationRequested()) {
			return MakeRejectedPlan(AnimationStatus::Cancelled, configuration_.scratchAlignment, std::move(cancellation));
		}
		if(sourceItems.size() > configuration_.maximumItemCount) {
			return MakeRejectedPlan(AnimationStatus::HardLimitExceeded, configuration_.scratchAlignment, std::move(cancellation));
		}

		AnimationWorkPlan plan;
		plan.scratchAlignment = configuration_.scratchAlignment;
		// Keep the caller token available to the function-level failure handler. A
		// shared cancellation token is a cheap copy and preserves the stale-work
		// identity if any planning allocation fails after this point.
		plan.cancellation = cancellation;
		if(sourceItems.empty()) return plan;

		plan.items.reserve(sourceItems.size());
		for(std::size_t index = 0U; index < sourceItems.size(); ++index) {
			const AnimationWorkItem& source = sourceItems[index];
			if(!source.animator.IsValid() || source.sourceGeneration == 0U
				|| !IsValidCompatibilityKey(source.compatibility)) {
				return MakeRejectedPlan(AnimationStatus::InvalidArgument,
					configuration_.scratchAlignment, plan.cancellation);
			}
			plan.items.push_back({source, index, 0U});
		}

		// Sort first by evaluator compatibility so each eventual range has exactly
		// one program/layout contract. The stable ordinal and handle break all ties.
		std::sort(plan.items.begin(), plan.items.end(), [](const auto& left, const auto& right) {
			return std::tie(left.item.compatibility, left.item.stablePublicationOrder,
				left.item.animator, left.item.sourceGeneration, left.item.scratchBytes)
				< std::tie(right.item.compatibility, right.item.stablePublicationOrder,
					right.item.animator, right.item.sourceGeneration, right.item.scratchBytes);
		});

		// An identical publication identity would make deterministic merge intent
		// ambiguous. Detect it independently of compatibility grouping.
		std::vector<std::pair<std::uint64_t, AnimatorInstanceHandle>> identities;
		identities.reserve(plan.items.size());
		for(const AnimationScheduledWorkItem& scheduled : plan.items) {
			identities.emplace_back(scheduled.item.stablePublicationOrder, scheduled.item.animator);
		}
		std::sort(identities.begin(), identities.end());
		if(std::adjacent_find(identities.begin(), identities.end()) != identities.end()) {
			return MakeRejectedPlan(AnimationStatus::Rejected,
				configuration_.scratchAlignment, plan.cancellation);
		}

		std::size_t scratchCursor = 0U;
		for(AnimationScheduledWorkItem& scheduled : plan.items) {
			std::size_t alignedOffset = 0U;
			if(!TryAlignUp(scratchCursor, configuration_.scratchAlignment, alignedOffset)
				|| scheduled.item.scratchBytes > configuration_.maximumScratchBytes
				|| alignedOffset > configuration_.maximumScratchBytes - scheduled.item.scratchBytes) {
				return MakeRejectedPlan(AnimationStatus::HardLimitExceeded,
					configuration_.scratchAlignment, plan.cancellation);
			}
			scheduled.scratchOffset = alignedOffset;
			scratchCursor = alignedOffset + scheduled.item.scratchBytes;
		}
		plan.totalScratchBytes = scratchCursor;

		std::vector<WorkGroup> groups;
		for(std::size_t groupBegin = 0U; groupBegin < plan.items.size();) {
			std::size_t groupEnd = groupBegin + 1U;
			while(groupEnd < plan.items.size()
				&& plan.items[groupEnd].item.compatibility == plan.items[groupBegin].item.compatibility) {
				++groupEnd;
			}
			const std::size_t size = groupEnd - groupBegin;
			const std::size_t desired = 1U + (size - 1U) / configuration_.targetItemsPerRange;
			groups.push_back({groupBegin, groupEnd, desired, 1U});
			groupBegin = groupEnd;
		}
		if(groups.size() > configuration_.maximumRangeCount) {
			return MakeRejectedPlan(AnimationStatus::HardLimitExceeded,
				configuration_.scratchAlignment, plan.cancellation);
		}

		std::size_t desiredTotal = 0U;
		for(const WorkGroup& group : groups) desiredTotal += group.desiredRangeCount;
		const std::size_t assignedTarget = std::min(desiredTotal, configuration_.maximumRangeCount);
		std::size_t assignedTotal = groups.size();
		while(assignedTotal < assignedTarget) {
			std::size_t selectedGroup = groups.size();
			std::size_t selectedProjectedSize = 0U;
			for(std::size_t index = 0U; index < groups.size(); ++index) {
				const WorkGroup& candidate = groups[index];
				if(candidate.assignedRangeCount >= candidate.desiredRangeCount) continue;
				const std::size_t groupSize = candidate.end - candidate.begin;
				const std::size_t nextCount = candidate.assignedRangeCount + 1U;
				const std::size_t projectedSize = 1U + (groupSize - 1U) / nextCount;
				if(selectedGroup == groups.size() || projectedSize > selectedProjectedSize) {
					selectedGroup = index;
					selectedProjectedSize = projectedSize;
				}
			}
			if(selectedGroup == groups.size()) break;
			++groups[selectedGroup].assignedRangeCount;
			++assignedTotal;
		}

		plan.ranges.reserve(assignedTotal);
		for(const WorkGroup& group : groups) {
			const std::size_t groupSize = group.end - group.begin;
			for(std::size_t part = 0U; part < group.assignedRangeCount; ++part) {
				const std::size_t itemBegin = PartitionBoundary(
					group.begin, groupSize, part, group.assignedRangeCount);
				const std::size_t itemEnd = PartitionBoundary(
					group.begin, groupSize, part + 1U, group.assignedRangeCount);
				const AnimationScheduledWorkItem& last = plan.items[itemEnd - 1U];
				plan.ranges.push_back({
					itemBegin,
					itemEnd,
					plan.items[itemBegin].scratchOffset,
					last.scratchOffset + last.item.scratchBytes,
					plan.items[itemBegin].item.compatibility});
			}
		}

		plan.publicationOrder.resize(plan.items.size());
		std::iota(plan.publicationOrder.begin(), plan.publicationOrder.end(), 0U);
		std::sort(plan.publicationOrder.begin(), plan.publicationOrder.end(), [&plan](
			std::size_t leftIndex, std::size_t rightIndex) {
			const AnimationWorkItem& left = plan.items[leftIndex].item;
			const AnimationWorkItem& right = plan.items[rightIndex].item;
			return std::tie(left.stablePublicationOrder, left.animator)
				< std::tie(right.stablePublicationOrder, right.animator);
		});

		if(sourceItems.size() <= configuration_.inlineItemThreshold || availableWorkerCount <= 1U) {
			plan.executionPolicy = AnimationWorkExecutionPolicy::Inline;
			plan.inlineSelected = true;
		} else {
			plan.executionPolicy = AnimationWorkExecutionPolicy::WorkerTasks;
			plan.selectedWorkerCount = std::min(availableWorkerCount, plan.ranges.size());
		}

		if(plan.cancellation.IsCancellationRequested()) {
			return MakeRejectedPlan(AnimationStatus::Cancelled,
				configuration_.scratchAlignment, plan.cancellation);
		}
		const AnimationStatus validation = ValidateAnimationWorkPlan(plan);
		if(validation != AnimationStatus::Ok) {
			return MakeRejectedPlan(validation, configuration_.scratchAlignment, plan.cancellation);
		}
		return plan;
	}
	catch(const std::bad_alloc&) {
		return MakeRejectedPlan(
			AnimationStatus::InternalError, configuration_.scratchAlignment, std::move(cancellation));
	}
	catch(...) {
		return MakeRejectedPlan(
			AnimationStatus::InternalError, configuration_.scratchAlignment, std::move(cancellation));
	}

	AnimationStatus ValidateAnimationWorkPlan(const AnimationWorkPlan& plan) try
	{
		if(plan.status != AnimationStatus::Ok) return plan.status;
		if(!IsPowerOfTwo(plan.scratchAlignment)) return AnimationStatus::InvalidArgument;
		if(plan.items.empty()) {
			return plan.ranges.empty() && plan.publicationOrder.empty()
				&& plan.totalScratchBytes == 0U
				&& plan.executionPolicy == AnimationWorkExecutionPolicy::Omitted
				? AnimationStatus::Ok
				: AnimationStatus::Rejected;
		}
		if(plan.ranges.empty() || plan.publicationOrder.size() != plan.items.size()) {
			return AnimationStatus::Rejected;
		}
		if(plan.executionPolicy != AnimationWorkExecutionPolicy::Inline
			&& plan.executionPolicy != AnimationWorkExecutionPolicy::WorkerTasks) {
			return AnimationStatus::Rejected;
		}

		std::size_t expectedItemBegin = 0U;
		std::size_t previousScratchEnd = 0U;
		for(const AnimationWorkRange& range : plan.ranges) {
			if(range.itemBegin != expectedItemBegin || range.itemBegin >= range.itemEnd
				|| range.itemEnd > plan.items.size()) return AnimationStatus::Rejected;
			const AnimationScheduledWorkItem& first = plan.items[range.itemBegin];
			const AnimationScheduledWorkItem& last = plan.items[range.itemEnd - 1U];
			if(range.scratchBegin != first.scratchOffset
				|| range.scratchEnd != last.scratchOffset + last.item.scratchBytes
				|| range.scratchBegin < previousScratchEnd
				|| range.scratchEnd > plan.totalScratchBytes) return AnimationStatus::Rejected;
			for(std::size_t index = range.itemBegin; index < range.itemEnd; ++index) {
				const AnimationScheduledWorkItem& item = plan.items[index];
				if(item.item.compatibility != range.compatibility
					|| item.scratchOffset % plan.scratchAlignment != 0U
					|| item.scratchOffset > plan.totalScratchBytes
					|| item.item.scratchBytes > plan.totalScratchBytes - item.scratchOffset) {
					return AnimationStatus::Rejected;
				}
			}
			expectedItemBegin = range.itemEnd;
			previousScratchEnd = range.scratchEnd;
		}
		if(expectedItemBegin != plan.items.size() || previousScratchEnd != plan.totalScratchBytes) {
			return AnimationStatus::Rejected;
		}

		std::vector<bool> seen(plan.items.size(), false);
		for(const std::size_t itemIndex : plan.publicationOrder) {
			if(itemIndex >= plan.items.size() || seen[itemIndex]) return AnimationStatus::Rejected;
			seen[itemIndex] = true;
		}
		for(std::size_t index = 1U; index < plan.publicationOrder.size(); ++index) {
			const AnimationWorkItem& previous = plan.items[plan.publicationOrder[index - 1U]].item;
			const AnimationWorkItem& current = plan.items[plan.publicationOrder[index]].item;
			if(std::tie(current.stablePublicationOrder, current.animator)
				< std::tie(previous.stablePublicationOrder, previous.animator)) {
				return AnimationStatus::Rejected;
			}
		}
		return AnimationStatus::Ok;
	}
	catch(const std::bad_alloc&) {
		return AnimationStatus::InternalError;
	}
	catch(...) {
		return AnimationStatus::InternalError;
	}

	AnimationStatus BuildDeterministicAnimationPublicationOrder(
		const AnimationWorkPlan& plan,
		std::span<const AnimationWorkCompletion> completions,
		std::vector<std::size_t>& output) try
	{
		if(plan.cancellation.IsCancellationRequested()) return AnimationStatus::Cancelled;
		if(ValidateAnimationWorkPlan(plan) != AnimationStatus::Ok
			|| completions.size() != plan.items.size()) return AnimationStatus::Rejected;

		std::vector<bool> completed(plan.items.size(), false);
		for(const AnimationWorkCompletion& completion : completions) {
			if(plan.cancellation.IsCancellationRequested()
				|| completion.status == AnimationStatus::Cancelled) return AnimationStatus::Cancelled;
			if(completion.status != AnimationStatus::Ok
				|| completion.scheduledItemIndex >= plan.items.size()
				|| completed[completion.scheduledItemIndex]) return AnimationStatus::Rejected;
			const AnimationWorkItem& expected = plan.items[completion.scheduledItemIndex].item;
			if(completion.animator != expected.animator
				|| completion.sourceGeneration != expected.sourceGeneration) return AnimationStatus::Rejected;
			completed[completion.scheduledItemIndex] = true;
		}

		// Stage before the final cancellation poll so the caller's prior complete
		// publication remains untouched if this batch becomes stale at the boundary.
		std::vector<std::size_t> staged = plan.publicationOrder;
		if(plan.cancellation.IsCancellationRequested()) return AnimationStatus::Cancelled;
		output.swap(staged);
		return AnimationStatus::Ok;
	}
	catch(const std::bad_alloc&) {
		return AnimationStatus::InternalError;
	}
	catch(...) {
		return AnimationStatus::InternalError;
	}

	struct AnimationWorkRangeLedger::Impl {
		explicit Impl(const AnimationWorkPlan& plan)
			: cancellation(plan.cancellation), rangeCount(plan.ranges.size()),
			states(std::make_unique<std::atomic_uint8_t[]>(rangeCount)),
			owners(std::make_unique<std::atomic_uint32_t[]>(rangeCount))
		{
			for(std::size_t index = 0U; index < rangeCount; ++index) {
				states[index].store(0U, std::memory_order_relaxed);
				owners[index].store(0U, std::memory_order_relaxed);
			}
		}

		AnimationWorkCancellationToken cancellation;
		std::size_t rangeCount = 0U;
		std::unique_ptr<std::atomic_uint8_t[]> states;
		std::unique_ptr<std::atomic_uint32_t[]> owners;
		std::atomic_size_t completedCount = 0U;
	};

	AnimationWorkRangeLedger::AnimationWorkRangeLedger(const AnimationWorkPlan& plan)
		: impl_(std::make_unique<Impl>(plan))
	{
	}

	AnimationWorkRangeLedger::~AnimationWorkRangeLedger() = default;
	AnimationWorkRangeLedger::AnimationWorkRangeLedger(AnimationWorkRangeLedger&&) noexcept = default;
	AnimationWorkRangeLedger& AnimationWorkRangeLedger::operator=(AnimationWorkRangeLedger&&) noexcept = default;

	bool AnimationWorkRangeLedger::TryBegin(
		std::size_t rangeIndex, std::uint32_t workerOrdinal) noexcept
	{
		if(impl_ == nullptr || rangeIndex >= impl_->rangeCount || workerOrdinal == 0U
			|| impl_->cancellation.IsCancellationRequested()) return false;
		std::uint8_t expected = 0U;
		if(!impl_->states[rangeIndex].compare_exchange_strong(
			expected, 1U, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
		impl_->owners[rangeIndex].store(workerOrdinal, std::memory_order_release);
		return true;
	}

	bool AnimationWorkRangeLedger::Complete(
		std::size_t rangeIndex, std::uint32_t workerOrdinal) noexcept
	{
		if(impl_ == nullptr || rangeIndex >= impl_->rangeCount || workerOrdinal == 0U
			|| impl_->cancellation.IsCancellationRequested()
			|| impl_->owners[rangeIndex].load(std::memory_order_acquire) != workerOrdinal) return false;
		std::uint8_t expected = 1U;
		if(!impl_->states[rangeIndex].compare_exchange_strong(
			expected, 2U, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
		impl_->completedCount.fetch_add(1U, std::memory_order_acq_rel);
		return !impl_->cancellation.IsCancellationRequested();
	}

	void AnimationWorkRangeLedger::CancelUnclaimed() noexcept
	{
		if(impl_ == nullptr) return;
		for(std::size_t index = 0U; index < impl_->rangeCount; ++index) {
			std::uint8_t expected = 0U;
			(void)impl_->states[index].compare_exchange_strong(
				expected, 3U, std::memory_order_acq_rel, std::memory_order_acquire);
		}
	}

	std::size_t AnimationWorkRangeLedger::CompletedCount() const noexcept
	{
		return impl_ == nullptr ? 0U : impl_->completedCount.load(std::memory_order_acquire);
	}

	bool AnimationWorkRangeLedger::AllCompleted() const noexcept
	{
		return impl_ != nullptr && !impl_->cancellation.IsCancellationRequested()
			&& CompletedCount() == impl_->rangeCount;
	}
}
