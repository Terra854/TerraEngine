#include "../TestHarness.h"

#include "FullParallelFrameTestFixtures.h"
#include "../Scene/SceneGraphTestFixtures.h"
#include "Scene/HierarchySnapshot.h"
#include "Scene/RuntimeLayout.h"
#include "Scene/SceneGraphRuntime.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <set>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>

namespace TerraEngine::Tests {
	void RegisterFullParallelFrameStressTests(TestRegistry& registry)
	{
		registry.Add("Engine.FullParallelFrame.RepeatedParallelSummariesStayDeterministic", [](TestContext& context) {
			auto first = FullParallelFrameFixtures::MakeWorkerWorkload("A", { "A" }, {});
			auto second = FullParallelFrameFixtures::MakeWorkerWorkload("B", { "B" }, {});
			const std::string baseline = TerraEngine::Scheduling::ValidateFrameWorkloads({ first, second }, "deterministic").formatted;

			for (int index = 0; index < 256; ++index)
			{
				context.ExpectEqual(baseline, TerraEngine::Scheduling::ValidateFrameWorkloads({ second, first }, "deterministic").formatted, "Validated summaries should remain deterministic across input order.");
			}
		});

		registry.Add("Engine.FullParallelFrame.StressFixtureBuildsTenThousandObjectIds", [](TestContext& context) {
			const std::vector<TerraEngine::UUID> ids = FullParallelFrameFixtures::MakeStressObjectIds(10000u);
			std::set<std::string> uniqueIds;
			for (const TerraEngine::UUID& id : ids)
			{
				uniqueIds.insert(TerraEngine::ToHexString(id));
			}

			context.ExpectEqual(static_cast<std::size_t>(10000u), ids.size(), "Stress fixtures should create the requested object count.");
			context.ExpectEqual(ids.size(), uniqueIds.size(), "Stress fixture object ids should be unique enough for deterministic chunk accounting.");
		});

		registry.Add("Engine.FullParallelFrame.LargeEcsQueryUsesChunkedWorkerPolicyByDefault", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "stress-ecs", "Stress ECS" });
			graph.AddQuery(FullParallelFrameFixtures::MakeStressComponentQuery("StressSystem", "StressQuery", 10000u));

			const auto& summary = graph.Validate();
			const auto& plan = summary.queryPlans.front();

			context.Expect(summary.valid, "Stress ECS query should validate with complete access metadata.");
			context.ExpectEqual(std::string("worker-task-group"), TerraEngine::Scheduling::ToString(plan.executionPolicy), "Large component query should default to worker chunking.");
			context.Expect(plan.chunks.size() > 1u, "Large component query should split into multiple chunks.");
			context.ExpectEqual(static_cast<std::size_t>(10000u), plan.matchingObjects.size(), "Stress query should preserve every matched object.");
		});

		registry.Add("Engine.FullParallelFrame.AdaptiveEcsChunkingBoundsWorkerTaskCount", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "bounded-ecs", "Bounded ECS" });
			auto query = FullParallelFrameFixtures::MakeStressComponentQuery("BoundedSystem", "BoundedQuery", 10000u);
			query.preferredChunkSize = 1u;
			query.minChunkSize = 32u;
			query.maxChunkSize = 512u;
			query.maxWorkerChunks = 16u;
			graph.AddQuery(std::move(query));

			const auto& summary = graph.Validate();
			const auto& plan = summary.queryPlans.front();

			context.Expect(summary.valid, "Bounded ECS query should validate.");
			context.Expect(plan.chunks.size() <= 16u, "Adaptive chunking should honor the configured maximum worker chunk count.");
			context.Expect(plan.chunks.size() > 1u, "Bounded worker query should still expose useful parallel chunks.");
			context.Expect(plan.chunks.front().end - plan.chunks.front().begin >= 625u, "Chunk size should grow enough to keep 10,000 objects within 16 worker tasks.");
		});

		registry.Add("Engine.FullParallelFrame.SmallSceneWorkInlinesBelowWorkerThreshold", [](TestContext& context) {
			std::atomic<int> calls = 0;
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload = FullParallelFrameFixtures::MakeWorkerWorkload("SmallScenePrep", { "SceneGraph" }, { "RuntimeLayout" });
			workload.estimatedWorkItemCount = 4u;
			workload.inlineWorkItemThreshold = 32u;
			workload.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) {
				calls.fetch_add(1);
			};
			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ExecuteFrameWorkloads({ workload }, "small-scene-overhead");

			context.Expect(plan.valid, "Small-scene workload declarations should validate.");
			context.Expect(plan.completed, "Small-scene worker-eligible work should still execute.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.migrationSummary.inlineCount, "Small scenes should inline worker-eligible work below the threshold.");
			context.Expect(plan.migrationSummary.workerSafeCount == 0u, "Small-scene guard should avoid scheduling worker work that would cost more than it saves.");
			context.ExpectEqual(1, calls.load(), "Small-scene inline work should run once on the caller thread.");
		});

		registry.Add("Engine.FullParallelFrame.WorkerMatrixPreservesThresholdsAndDeterministicPartitions", [](TestContext& context) {
			ThreadPool& pool = ThreadPool::instance();
			pool.shutdown();
			const std::size_t maximumWorkers = std::max<std::size_t>(1U, std::thread::hardware_concurrency());
			const std::array<std::size_t, 4> requestedWorkers{1U, 2U, 4U, maximumWorkers};
			std::uint64_t baselineHash = 0U;
			for (std::size_t matrixIndex = 0U; matrixIndex < requestedWorkers.size(); ++matrixIndex)
			{
				const std::size_t workers = requestedWorkers[matrixIndex];
				if (std::find(requestedWorkers.begin(), requestedWorkers.begin() + matrixIndex, workers)
					!= requestedWorkers.begin() + matrixIndex) continue;
				pool.initialize(workers);
				context.ExpectEqual(workers, pool.getThreadCount(),
					"The 1/2/4/max comparison must use the exact requested worker identity without a hidden cap.");

				pool.ResetTelemetry();
				auto small = FullParallelFrameFixtures::MakeWorkerWorkload("MatrixSmall", {"SceneGraph"}, {"RuntimeLayout"});
				small.estimatedWorkItemCount = 4U;
				small.inlineWorkItemThreshold = 32U;
				small.work = [](TerraEngine::Scheduling::JobGraphContext&) {};
				const auto smallPlan = TerraEngine::Scheduling::ExecuteFrameWorkloads({small}, "matrix-small");
				const auto smallTelemetry = pool.GetTelemetrySnapshot();
				std::uint64_t smallDispatches = 0U;
				for (const ThreadPoolWorkerTelemetry& worker : smallTelemetry->workers) smallDispatches += worker.dispatchCount;
				context.Expect(smallPlan.completed && smallPlan.migrationSummary.inlineCount == 1U && smallDispatches == 0U,
					"Work below the measured threshold must inline at every worker configuration.");

				pool.ResetTelemetry();
				std::vector<std::uint64_t> values(8192U, 0U);
				auto large = FullParallelFrameFixtures::MakeWorkerWorkload("MatrixLarge", {"SceneGraph"}, {"RuntimeLayout"});
				large.stableDescriptorId = "matrix/large";
				large.estimatedWorkItemCount = values.size();
				large.inlineWorkItemThreshold = 32U;
				large.partitionPolicy = TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges;
				large.partitionWorkItemCount = 512U;
				large.rangeWork = [&values](const TerraEngine::Scheduling::FrameWorkloadPartition& range, TerraEngine::Scheduling::JobGraphContext&) {
					for (std::size_t index = range.itemBegin; index < range.itemEnd; ++index)
					{
						values[index] = static_cast<std::uint64_t>(index) * 11400714819323198485ULL + 137U;
					}
					// Keep ranges observable long enough for multi-worker scheduling
					// without turning this deterministic gate into a timing benchmark.
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				};
				const auto largePlan = TerraEngine::Scheduling::ExecuteFrameWorkloads({large}, "matrix-large");
				std::uint64_t hash = 1469598103934665603ULL;
				for (const std::uint64_t value : values)
				{
					hash ^= value;
					hash *= 1099511628211ULL;
				}
				const auto telemetry = pool.GetTelemetrySnapshot();
				std::uint64_t dispatches = 0U;
				std::uint64_t usefulItems = 0U;
				std::size_t participatingWorkers = 0U;
				for (const ThreadPoolWorkerTelemetry& worker : telemetry->workers)
				{
					dispatches += worker.dispatchCount;
					usefulItems += worker.usefulItemCount;
					if (worker.dispatchCount > 0U) ++participatingWorkers;
				}
				context.Expect(largePlan.valid && largePlan.completed && largePlan.results.front().partitionCount == 16U,
					"Large work must select sixteen deterministic 512-item ranges.");
				context.ExpectEqual(static_cast<std::uint64_t>(16U), dispatches,
					"Every deterministic range must produce exactly one useful dispatch.");
				context.ExpectEqual(static_cast<std::uint64_t>(values.size()), usefulItems,
					"Worker telemetry must account for every item owned by the range dispatches.");
				context.Expect(participatingWorkers >= 1U && participatingWorkers <= workers,
					"Worker utilization evidence must name a valid subset of the resolved pool.");
				if (baselineHash == 0U) baselineHash = hash;
				context.ExpectEqual(baselineHash, hash,
					"One, two, four, and maximum workers must publish the same deterministic result hash.");
				pool.shutdown();
			}
		});

		registry.Add("Engine.FullParallelFrame.TransformPropagationUsesWorkersForLargeHierarchy", [](TestContext& context) {
			FullParallelFrameFixtures::ThreadPoolSession session;
			TerraEngine::Scene::SceneGraph graph = SceneFixtures::MakeLargeFiveLevelGraph(10000u);
			const TerraEngine::Scene::SceneDirtyTransformState dirtyState = TerraEngine::Scene::BuildDirtyTransformState(graph);
			const TerraEngine::Scene::SceneTransformPropagationResult result = TerraEngine::Scene::PropagateWorldTransforms(graph, dirtyState);

			context.Expect(result.valid, "Large hierarchy transform propagation should stay valid.");
			context.ExpectEqual(10000u, result.processedObjectCount, "Full-refresh stress propagation should process every object.");
			context.Expect(result.workerJobCount > 0u, "Large hierarchy transform propagation should execute worker jobs.");
			context.Expect(result.workerUtilization > 0.0, "Worker propagation should report non-zero utilization.");
		});

		registry.Add("Engine.FullParallelFrame.DirtyRuntimeLayoutFiltersOnlyChangedBranches", [](TestContext& context) {
			TerraEngine::Scene::SceneGraph graph = SceneFixtures::MakeLargeFiveLevelGraph(10000u);
			const TerraEngine::UUID dirtyRoot = graph.objects.at(graph.rootObjects.front()).children.front();
			const TerraEngine::Scene::SceneDirtyTransformState dirtyState = TerraEngine::Scene::BuildDirtyTransformState(
				graph,
				{ dirtyRoot },
				TerraEngine::Scene::SceneTransformDirtyReason::LocalTransform);
			TerraEngine::Scene::HierarchySnapshot snapshot;
			snapshot.revision = graph.revision;
			snapshot.publishedRevision = graph.revision;
			for (const TerraEngine::Scene::SceneObjectId& objectId : TerraEngine::Scene::BuildTraversalOrder(graph))
			{
				const TerraEngine::Scene::SceneGraphObject& object = graph.objects.at(objectId);
				TerraEngine::Scene::HierarchySnapshotEntry entry;
				entry.objectId = objectId;
				entry.object.entityValue = static_cast<std::uint32_t>(object.entity);
				entry.depth = object.parentId == TerraEngine::EMPTY_UUID ? 0u : 1u;
				snapshot.orderedObjects.push_back(entry);
			}
			snapshot.valid = true;
			TerraEngine::Scene::FlatHierarchyLayout layout;
			layout.valid = true;
			layout.totalObjectCount = snapshot.orderedObjects.size();
			layout.activeObjectCount = snapshot.orderedObjects.size();
			layout.entries.reserve(snapshot.orderedObjects.size());
			std::unordered_map<TerraEngine::UUID, std::size_t> indexByObject;
			for (std::size_t index = 0; index < snapshot.orderedObjects.size(); ++index)
			{
				const TerraEngine::Scene::SceneObjectId objectId = snapshot.orderedObjects[index].objectId;
				const TerraEngine::Scene::SceneGraphObject& object = graph.objects.at(objectId);
				TerraEngine::Scene::FlatHierarchyEntry entry;
				entry.objectId = objectId;
				entry.entity = object.entity;
				if (object.parentId != TerraEngine::EMPTY_UUID)
				{
					entry.parentIndex = indexByObject[object.parentId];
					entry.depth = layout.entries[*entry.parentIndex].depth + 1u;
				}
				layout.entries.push_back(entry);
				indexByObject[objectId] = index;
			}

			const std::vector<std::size_t> dirtyIndices = TerraEngine::Scene::BuildDirtyTransformLayoutIndices(layout, dirtyState);

			context.Expect(!dirtyIndices.empty(), "Dirty runtime layout should find the edited branch.");
			context.Expect(dirtyIndices.size() < layout.entries.size(), "Dirty runtime layout should avoid full-layout propagation for one edited branch.");
			context.ExpectEqual(dirtyState.expandedDirtyObjects.size(), dirtyIndices.size(), "Dirty layout filtering should match scene dirty branch expansion.");
		});
	}
}
