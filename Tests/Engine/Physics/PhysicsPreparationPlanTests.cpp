#include "../TestHarness.h"

#include "Physics/PhysicsPreparationPlan.h"
#include "ThreadPool/ThreadPool.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Physics;

		class ThreadPoolSession final {
		public:
			explicit ThreadPoolSession(std::size_t workers) {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize(workers);
			}
			~ThreadPoolSession() { ThreadPool::instance().shutdown(); }

			ThreadPoolSession(const ThreadPoolSession&) = delete;
			ThreadPoolSession& operator=(const ThreadPoolSession&) = delete;
		};

		TerraEngine::UUID Id(std::uint32_t value) {
			TerraEngine::UUID id{};
			id[12] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
			id[13] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
			id[14] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
			id[15] = static_cast<std::uint8_t>(value & 0xFFU);
			return id;
		}

		PhysicsAuthoringGeneration Generation(
			PhysicsGeneration generation,
			std::size_t count,
			PhysicsAuthoringChangeReason reasons = PhysicsAuthoringChangeReason::BodyComponent) {
			PhysicsAuthoringGenerationData data;
			data.world = {{1U}, 1U};
			data.generation = generation;
			data.snapshotGeneration = generation;
			data.hierarchyGeneration = 1U;
			data.settingsHash = 1U;
			data.changes.reserve(count);
			for(std::size_t index = 0U; index < count; ++index) {
				data.changes.push_back({
					Id(static_cast<std::uint32_t>(index + 1U)),
					1U,
					static_cast<PhysicsDescriptorIndex>(index),
					reasons});
			}
			auto created = PhysicsAuthoringGeneration::Create(std::move(data));
			if(!created) throw TestFailure("The preparation fixture must create a valid authoring generation.");
			return std::move(*created);
		}
	}

	void RegisterPhysicsPreparationPlanTests(TestRegistry& registry) {
		registry.Add("Physics.PreparationPlan.UnchangedKinematicsSettledBodiesAndEmptyWorkUseFastPath", [](TestContext& context) {
			const PhysicsAuthoringGeneration unchanged = Generation(1U, 0U);
			const auto plan = PhysicsPreparationPlan::Build(unchanged, 42U);
			context.Expect(plan && plan->Empty()
				&& plan->Data().executionPath == PhysicsPreparationExecutionPath::Fast
				&& plan->Data().ranges.empty(),
				"Unchanged kinematics, settled bodies, and no commands or queries must create no preparation work.");

			ThreadPoolSession pool(2U);
			PhysicsPreparationScratch scratch(8U);
			std::atomic<std::size_t> calls = 0U;
			context.Expect(ExecutePhysicsPreparationPlan(*plan, ThreadPool::instance(), scratch,
				[&](const PhysicsPreparationWorkItem&) {
					calls.fetch_add(1U, std::memory_order_relaxed);
					return true;
				}), "An empty plan must complete successfully without dispatching work.");
			context.ExpectEqual(static_cast<std::size_t>(0U), calls.load(),
				"The empty fast path must not invoke the preparation callback.");
		});

		registry.Add("Physics.PreparationPlan.ReasonTaggedOperationsProduceDistinctStableRanges", [](TestContext& context) {
			PhysicsAuthoringGenerationData data;
			data.world = {{1U}, 1U};
			data.generation = 2U;
			data.snapshotGeneration = 2U;
			data.hierarchyGeneration = 1U;
			data.settingsHash = 1U;
			data.changes = {
				{Id(1U), 1U, 1U, PhysicsAuthoringChangeReason::Added},
				{Id(2U), 1U, kInvalidPhysicsDescriptorIndex, PhysicsAuthoringChangeReason::Removed},
				{Id(3U), 1U, 3U, PhysicsAuthoringChangeReason::Transform},
				{Id(4U), 1U, 4U, PhysicsAuthoringChangeReason::ColliderAsset},
				{Id(5U), 1U, 5U, PhysicsAuthoringChangeReason::MaterialAsset}};
			const auto generation = PhysicsAuthoringGeneration::Create(std::move(data));
			context.Expect(generation.has_value(), "The reason-tagged plan fixture must validate.");
			PhysicsPreparationPlanOptions options;
			options.parallelThreshold = 100U;
			options.availableWorkerCount = 4U;
			options.commands = {9U, 7U};
			options.queries = {{{1U}, 1U, 0U, 1U, 1U, 2U}};
			const auto plan = PhysicsPreparationPlan::Build(*generation, 5U, std::move(options));
			context.Expect(plan && plan->Data().creates.size() == 1U
				&& plan->Data().removals.size() == 1U
				&& plan->Data().updates.size() == 1U
				&& plan->Data().dependencyRebuilds.size() == 2U
				&& plan->Data().commands.size() == 2U
				&& plan->Data().queries.size() == 1U,
				"Create, remove, update, dependency rebuild, command, and query work must remain distinct.");
			context.Expect(plan && plan->Data().executionPath == PhysicsPreparationExecutionPath::Inline,
				"A small mixed plan must stay inline below the measured threshold.");
			for(std::size_t index = 0U; plan && index < plan->Data().workItems.size(); ++index) {
				context.ExpectEqual(index, plan->Data().workItems[index].stableOrdinal,
					"Every mixed work item must retain deterministic stable ordering.");
			}
		});

		registry.Add("Physics.PreparationPlan.LargeIndependentRangesExecuteDeterministicallyOnThreadPool", [](TestContext& context) {
			constexpr std::size_t kItemCount = 512U;
			const PhysicsAuthoringGeneration generation = Generation(3U, kItemCount);
			PhysicsPreparationPlanOptions options;
			options.parallelThreshold = 64U;
			options.rangeSize = 31U;
			options.availableWorkerCount = 4U;
			const auto plan = PhysicsPreparationPlan::Build(generation, 6U, options);
			context.Expect(plan
				&& plan->Data().executionPath == PhysicsPreparationExecutionPath::WorkerRanges
				&& plan->Data().ranges.size() > 1U,
				"A sufficiently large independent set must select deterministic worker ranges.");

			ThreadPoolSession pool(4U);
			PhysicsPreparationScratch scratch(kItemCount);
			std::vector<std::size_t> observed(kItemCount, kItemCount);
			const bool executed = ExecutePhysicsPreparationPlan(*plan, ThreadPool::instance(), scratch,
				[&](const PhysicsPreparationWorkItem& item) {
					// Every worker writes a disjoint ordinal selected by the immutable plan.
					observed[item.stableOrdinal] = item.stableOrdinal;
					return true;
				});
			context.Expect(executed, "Every deterministic worker range must complete successfully.");
			for(std::size_t index = 0U; index < observed.size(); ++index) {
				context.ExpectEqual(index, observed[index],
					"Worker completion order must not alter stable preparation output order.");
				context.Expect(scratch.Slots()[index].attempted && scratch.Slots()[index].accepted,
					"Scratch slots must record one successful result for every stable ordinal.");
			}
		});

		registry.Add("Physics.PreparationPlan.ScratchIsBoundedReusedAndGenerationCleared", [](TestContext& context) {
			ThreadPoolSession pool(2U);
			PhysicsPreparationScratch scratch(64U);
			PhysicsPreparationPlanOptions options;
			options.parallelThreshold = 8U;
			options.rangeSize = 8U;
			options.availableWorkerCount = 2U;
			const auto first = PhysicsPreparationPlan::Build(Generation(4U, 32U), 1U, options);
			context.Expect(first && ExecutePhysicsPreparationPlan(*first, ThreadPool::instance(), scratch,
				[](const PhysicsPreparationWorkItem&) { return true; }),
				"The first bounded scratch generation must execute.");
			const auto* firstAddress = scratch.Slots().data();
			const std::size_t firstAllocations = scratch.AllocationCount();

			const auto second = PhysicsPreparationPlan::Build(Generation(5U, 8U), 2U, options);
			context.Expect(second && ExecutePhysicsPreparationPlan(*second, ThreadPool::instance(), scratch,
				[](const PhysicsPreparationWorkItem&) { return true; }),
				"A smaller generation must reuse the bounded allocation.");
			context.Expect(firstAddress == scratch.Slots().data()
				&& scratch.AllocationCount() == firstAllocations && scratch.ReuseCount() > 0U,
				"Repeated preparation must reuse capacity instead of growing per frame.");
			for(const PhysicsPreparationScratchSlot& slot : scratch.Slots()) {
				context.Expect(slot.generation == 5U && slot.item.operation.objectId != TerraEngine::EMPTY_UUID,
					"Generation clearing must replace every active slot without retaining stale object identities.");
			}

			const auto tooLarge = PhysicsPreparationPlan::Build(Generation(6U, 65U), 3U, options);
			std::atomic<std::size_t> calls = 0U;
			context.Expect(tooLarge && !ExecutePhysicsPreparationPlan(*tooLarge, ThreadPool::instance(), scratch,
				[&](const PhysicsPreparationWorkItem&) {
					calls.fetch_add(1U, std::memory_order_relaxed);
					return true;
				}), "Work beyond the configured scratch bound must be rejected before dispatch.");
			context.ExpectEqual(static_cast<std::size_t>(0U), calls.load(),
				"A rejected oversized generation must not run partial preparation callbacks.");
			context.Expect(scratch.Slots().empty() && scratch.ActiveItems() == 0U,
				"Rejecting oversized work must clear the prior generation instead of exposing stale slots.");
		});
	}
}
