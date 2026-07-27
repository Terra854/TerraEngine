// AnimationSchedulingStressTests.cpp - deterministic animation worker planning stress coverage.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationWorkScheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationWorkCompatibilityKey MakeCompatibilityKey(std::size_t group)
		{
			AnimationWorkCompatibilityKey key;
			key.programGeneration = 10U + group;
			key.skeletonGeneration = 20U + group;
			key.clipBindingGeneration = 30U + group;
			key.outputRequirementsFingerprint = 40U + group;
			key.resourceLayoutFingerprint = 50U + group;
			key.effectiveMode = static_cast<AnimationEffectiveExecutionMode>(group % 3U);
			return key;
		}

		std::vector<AnimationWorkItem> MakeWorkItems(std::size_t count)
		{
			std::vector<AnimationWorkItem> items;
			items.reserve(count);
			for(std::size_t index = 0U; index < count; ++index) {
				AnimationWorkItem item;
				item.animator = {static_cast<std::uint32_t>(index + 1U), 1U};
				item.compatibility = MakeCompatibilityKey(index % 5U);
				item.stablePublicationOrder = static_cast<std::uint64_t>((index * 73U) % std::max<std::size_t>(1U, count));
				item.sourceGeneration = 1000U + index;
				item.scratchBytes = 16U + (index % 7U) * 13U;
				items.push_back(item);
			}
			return items;
		}

		AnimationWorkPlan BuildStressPlan(
			std::vector<AnimationWorkItem> items,
			std::size_t availableWorkerCount = 16U)
		{
			AnimationWorkSchedulerConfiguration configuration;
			configuration.inlineItemThreshold = 8U;
			configuration.targetItemsPerRange = 17U;
			configuration.maximumRangeCount = 48U;
			configuration.maximumItemCount = 4096U;
			configuration.maximumScratchBytes = 4U * 1024U * 1024U;
			configuration.scratchAlignment = 64U;
			AnimationWorkScheduler scheduler(configuration);
			AnimationWorkCancellationSource cancellation;
			return scheduler.BuildPlan(items, availableWorkerCount, cancellation.CaptureToken());
		}

		bool PlansHaveSameDeterministicShape(
			const AnimationWorkPlan& left,
			const AnimationWorkPlan& right)
		{
			if(left.status != right.status
				|| left.totalScratchBytes != right.totalScratchBytes
				|| left.items.size() != right.items.size()
				|| left.ranges.size() != right.ranges.size()
				|| left.publicationOrder.size() != right.publicationOrder.size()) return false;
			for(std::size_t index = 0U; index < left.items.size(); ++index) {
				const AnimationScheduledWorkItem& a = left.items[index];
				const AnimationScheduledWorkItem& b = right.items[index];
				if(a.item.animator != b.item.animator
					|| a.item.compatibility != b.item.compatibility
					|| a.item.stablePublicationOrder != b.item.stablePublicationOrder
					|| a.item.sourceGeneration != b.item.sourceGeneration
					|| a.item.scratchBytes != b.item.scratchBytes
					|| a.scratchOffset != b.scratchOffset) return false;
			}
			for(std::size_t index = 0U; index < left.ranges.size(); ++index) {
				const AnimationWorkRange& a = left.ranges[index];
				const AnimationWorkRange& b = right.ranges[index];
				if(a.itemBegin != b.itemBegin || a.itemEnd != b.itemEnd
					|| a.scratchBegin != b.scratchBegin || a.scratchEnd != b.scratchEnd
					|| a.compatibility != b.compatibility) return false;
			}
			for(std::size_t index = 0U; index < left.publicationOrder.size(); ++index) {
				const auto& a = left.items[left.publicationOrder[index]].item;
				const auto& b = right.items[right.publicationOrder[index]].item;
				if(a.animator != b.animator || a.stablePublicationOrder != b.stablePublicationOrder) return false;
			}
			return true;
		}
	}

	void RegisterAnimationSchedulingStressTests(TestRegistry& registry)
	{
		registry.Add("Animation.Scheduling.RandomizedInputsKeepCompatibleDisjointRanges", [](TestContext& context) {
			const std::vector<AnimationWorkItem> canonicalItems = MakeWorkItems(257U);
			const AnimationWorkPlan canonical = BuildStressPlan(canonicalItems);
			context.Expect(canonical.status == AnimationStatus::Ok
				&& canonical.executionPolicy == AnimationWorkExecutionPolicy::WorkerTasks,
				"A large compatible workload must select bounded worker ranges.");
			context.Expect(ValidateAnimationWorkPlan(canonical) == AnimationStatus::Ok,
				"The scheduler must produce complete, gap-free, range-disjoint ownership.");

			for(const AnimationWorkRange& range : canonical.ranges) {
				context.Expect(range.itemBegin < range.itemEnd && range.itemEnd <= canonical.items.size(),
					"Every scheduled range must own at least one valid item.");
				for(std::size_t index = range.itemBegin; index < range.itemEnd; ++index) {
					context.Expect(canonical.items[index].item.compatibility == range.compatibility,
						"A worker range must never mix incompatible programs or resource layouts.");
					context.Expect(canonical.items[index].scratchOffset % 64U == 0U,
						"Every worker-owned scratch slice must honor the configured alignment.");
				}
			}

			for(std::uint32_t seed = 1U; seed <= 32U; ++seed) {
				std::vector<AnimationWorkItem> randomized = canonicalItems;
				std::mt19937 generator(seed);
				std::shuffle(randomized.begin(), randomized.end(), generator);
				const AnimationWorkPlan candidate = BuildStressPlan(std::move(randomized));
				context.Expect(PlansHaveSameDeterministicShape(canonical, candidate),
					"Input and worker discovery order must not change batching, scratch ownership, or publication order.");
			}

			// ThreadPool availability changes only where the prepared plan runs. It
			// must never change compatibility batches, ownership, or protected merge.
			for(const std::size_t workerCount : std::array<std::size_t, 4U>{1U, 4U, 16U, 31U}) {
				const AnimationWorkPlan candidate = BuildStressPlan(canonicalItems, workerCount);
				const AnimationWorkExecutionPolicy expectedPolicy = workerCount == 1U
					? AnimationWorkExecutionPolicy::Inline
					: AnimationWorkExecutionPolicy::WorkerTasks;
				const std::size_t expectedWorkers = workerCount == 1U
					? 0U
					: std::min(workerCount, candidate.ranges.size());
				context.Expect(PlansHaveSameDeterministicShape(canonical, candidate)
					&& candidate.executionPolicy == expectedPolicy
					&& candidate.selectedWorkerCount == expectedWorkers,
					"The 1/4/16/31 worker matrix must change dispatch choice without changing deterministic geometry.");
			}
		});

		registry.Add("Animation.Scheduling.SmallWorkStaysInlineWithoutWeakeningCompatibility", [](TestContext& context) {
			AnimationWorkSchedulerConfiguration configuration;
			configuration.inlineItemThreshold = 8U;
			configuration.targetItemsPerRange = 4U;
			AnimationWorkScheduler scheduler(configuration);
			AnimationWorkCancellationSource cancellation;
			const AnimationWorkPlan inlinePlan = scheduler.BuildPlan(
				MakeWorkItems(8U), 31U, cancellation.CaptureToken());
			context.Expect(inlinePlan.status == AnimationStatus::Ok
				&& inlinePlan.executionPolicy == AnimationWorkExecutionPolicy::Inline
				&& inlinePlan.inlineSelected
				&& inlinePlan.selectedWorkerCount == 0U,
				"Small animation workloads must avoid dispatch overhead and report inline granularity.");
			context.Expect(ValidateAnimationWorkPlan(inlinePlan) == AnimationStatus::Ok,
				"Inline execution must keep the same compatible range and scratch rules as worker execution.");

			const AnimationWorkPlan emptyPlan = scheduler.BuildPlan({}, 31U, cancellation.CaptureToken());
			context.Expect(emptyPlan.status == AnimationStatus::Ok
				&& emptyPlan.executionPolicy == AnimationWorkExecutionPolicy::Omitted
				&& emptyPlan.ranges.empty(),
				"Zero animators must produce no task, range, or scratch allocation.");
		});

		registry.Add("Animation.Scheduling.PublicationOrderIgnoresCompletionOrderAndRejectsPartialResults", [](TestContext& context) {
			AnimationWorkCancellationSource cancellation;
			AnimationWorkScheduler scheduler;
			const AnimationWorkPlan plan = scheduler.BuildPlan(
				MakeWorkItems(193U), 16U, cancellation.CaptureToken());
			std::vector<AnimationWorkCompletion> completions;
			completions.reserve(plan.items.size());
			for(std::size_t index = 0U; index < plan.items.size(); ++index) {
				completions.push_back({index, plan.items[index].item.animator,
					plan.items[index].item.sourceGeneration, AnimationStatus::Ok});
			}
			std::mt19937 generator(0x5a17U);
			std::shuffle(completions.begin(), completions.end(), generator);

			std::vector<std::size_t> publication;
			context.Expect(BuildDeterministicAnimationPublicationOrder(plan, completions, publication)
				== AnimationStatus::Ok,
				"Complete worker results must merge successfully regardless of completion order.");
			context.Expect(publication == plan.publicationOrder,
				"The protected publisher must receive the plan's stable animator order, not worker finish order.");

			std::vector<std::size_t> preserved{999U};
			completions.pop_back();
			context.Expect(BuildDeterministicAnimationPublicationOrder(plan, completions, preserved)
				== AnimationStatus::Rejected && preserved == std::vector<std::size_t>{999U},
				"A missing worker result must reject atomically without replacing the prior complete publication.");
		});

		registry.Add("Animation.Scheduling.RangeClaimsRemainSingleOwnerUnderRaceStress", [](TestContext& context) {
			AnimationWorkCancellationSource cancellation;
			AnimationWorkSchedulerConfiguration configuration;
			configuration.inlineItemThreshold = 1U;
			configuration.targetItemsPerRange = 7U;
			configuration.maximumRangeCount = 128U;
			AnimationWorkScheduler scheduler(configuration);
			const AnimationWorkPlan plan = scheduler.BuildPlan(
				MakeWorkItems(512U), 16U, cancellation.CaptureToken());
			AnimationWorkRangeLedger ledger(plan);
			auto executionCounts = std::make_unique<std::atomic_uint32_t[]>(plan.items.size());
			for(std::size_t index = 0U; index < plan.items.size(); ++index) {
				executionCounts[index].store(0U, std::memory_order_relaxed);
			}

			std::vector<std::thread> workers;
			for(std::uint32_t worker = 1U; worker <= 8U; ++worker) {
				workers.emplace_back([worker, &ledger, &plan, &executionCounts]() {
					// Every worker deliberately contests every range in a different order.
					// The atomic ledger grants exactly one owner without protecting live scene state.
					for(std::size_t step = 0U; step < plan.ranges.size(); ++step) {
						const std::size_t rangeIndex = (step + worker * 11U) % plan.ranges.size();
						if(!ledger.TryBegin(rangeIndex, worker)) continue;
						const AnimationWorkRange& range = plan.ranges[rangeIndex];
						for(std::size_t item = range.itemBegin; item < range.itemEnd; ++item) {
							executionCounts[item].fetch_add(1U, std::memory_order_acq_rel);
						}
						(void)ledger.Complete(rangeIndex, worker);
					}
				});
			}
			for(std::thread& worker : workers) worker.join();

			context.Expect(ledger.CompletedCount() == plan.ranges.size() && ledger.AllCompleted(),
				"Every valid range must complete once despite duplicate worker claims.");
			for(std::size_t index = 0U; index < plan.items.size(); ++index) {
				context.Expect(executionCounts[index].load(std::memory_order_acquire) == 1U,
					"Range-disjoint scratch/output ownership must evaluate every animator exactly once.");
			}
		});

		registry.Add("Animation.Scheduling.CancellationPreventsLateMergeWithoutPartialPublication", [](TestContext& context) {
			AnimationWorkCancellationSource cancellation;
			AnimationWorkScheduler scheduler;
			const AnimationWorkPlan plan = scheduler.BuildPlan(
				MakeWorkItems(96U), 8U, cancellation.CaptureToken());
			AnimationWorkRangeLedger ledger(plan);
			context.Expect(ledger.TryBegin(0U, 1U),
				"A current worker must be able to claim an unowned range before cancellation.");
			cancellation.RequestCancellation();
			context.Expect(!ledger.Complete(0U, 1U),
				"A range that finishes after cancellation must not become publishable.");
			ledger.CancelUnclaimed();
			context.Expect(!ledger.AllCompleted(),
				"Cancelled and completed ranges must remain observably distinct.");

			std::vector<AnimationWorkCompletion> completions;
			for(std::size_t index = 0U; index < plan.items.size(); ++index) {
				completions.push_back({index, plan.items[index].item.animator,
					plan.items[index].item.sourceGeneration, AnimationStatus::Ok});
			}
			std::vector<std::size_t> priorPublication{42U};
			context.Expect(BuildDeterministicAnimationPublicationOrder(plan, completions, priorPublication)
				== AnimationStatus::Cancelled
				&& priorPublication == std::vector<std::size_t>{42U},
				"Cancellation must reject the whole prepared batch and preserve the prior publication.");
		});
	}
}
