#include "../TestHarness.h"

#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Core/Object/ObjectManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class ThreadPoolSession {
		public:
			ThreadPoolSession()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession()
			{
				ThreadPool::instance().shutdown();
			}
		};

		std::vector<TerraEngine::UUID> MakeIds(std::size_t count)
		{
			std::vector<TerraEngine::UUID> ids;
			for (std::size_t index = 0; index < count; ++index)
			{
				ids.push_back(TerraEngine::GenerateUUID());
			}
			return ids;
		}

		TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeQuery(std::string systemName, std::string queryName, std::vector<TerraEngine::UUID> objects)
		{
			TerraEngine::Scheduling::EcsComponentQueryDeclaration query;
			query.queryName = queryName;
			query.access.ownerSystem = systemName;
			query.access.queryName = queryName;
			query.access.workerEligible = true;
			query.access.readComponents = { "Transform" };
			query.matchingObjects = std::move(objects);
			query.workerThreshold = 4;
			query.preferredChunkSize = 2;
			return query;
		}

		bool SummaryHasDiagnostic(const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& summary, std::string_view code)
		{
			return std::any_of(summary.diagnostics.begin(), summary.diagnostics.end(), [&](const TerraEngine::Scheduling::EcsRaceDiagnostic& diagnostic) {
				return diagnostic.code == code;
			});
		}
	}

	void RegisterEcsQueryTaskGraphTests(TestRegistry& registry)
	{
		registry.Add("Engine.EcsQueryTaskGraph.ReadReadQueriesRunAsWorkerGroups", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "read-read", "Read Read" });
			std::atomic<int> active = 0;
			std::atomic<int> maxActive = 0;
			auto work = [&active, &maxActive](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&) {
				const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
				int observed = maxActive.load(std::memory_order_acquire);
				while (now > observed && !maxActive.compare_exchange_weak(observed, now, std::memory_order_acq_rel))
				{
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
				active.fetch_sub(1, std::memory_order_acq_rel);
			};
			auto queryA = MakeQuery("A", "ReadA", MakeIds(12));
			queryA.work = work;
			auto queryB = MakeQuery("B", "ReadB", MakeIds(12));
			queryB.work = work;
			graph.AddQuery(std::move(queryA));
			graph.AddQuery(std::move(queryB));

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Read/read ECS query graph should execute.");
			context.Expect(maxActive.load(std::memory_order_acquire) > 1, "Read/read worker chunks should overlap.");
			context.ExpectEqual(static_cast<std::size_t>(2), summary.queryPlans.size(), "Both queries should be planned.");
			context.ExpectEqual(std::string("worker-task-group"), TerraEngine::Scheduling::ToString(summary.queryPlans.front().executionPolicy), "Large read-only query should use workers.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.WriteConflictsAreDiagnosedAndOrdered", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "write-conflict", "Write Conflict" });
			std::vector<std::string> order;
			std::mutex orderMutex;
			auto reader = MakeQuery("Reader", "Reader", MakeIds(8));
			reader.work = [&order, &orderMutex](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&) {
				std::scoped_lock lock(orderMutex);
				order.push_back("reader");
			};
			auto writer = MakeQuery("Writer", "Writer", MakeIds(8));
			writer.access.readComponents.clear();
			writer.access.writeComponents = { "Transform" };
			writer.work = [&order, &orderMutex](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&) {
				std::scoped_lock lock(orderMutex);
				order.push_back("writer");
			};
			graph.AddQuery(std::move(reader));
			graph.AddQuery(std::move(writer));

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Write-conflicting queries should execute after graph ordering.");
			context.Expect(SummaryHasDiagnostic(summary, "access-conflict"), "Write conflict should produce an ordering diagnostic.");
			context.Expect(!order.empty(), "Chunk work should run.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.MissingWorkerDeclarationsStayProtected", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "missing", "Missing" });
			auto query = MakeQuery("Missing", "MissingQuery", MakeIds(8));
			query.access.readComponents.clear();
			graph.AddQuery(std::move(query));

			const auto& summary = graph.Validate();

			context.Expect(summary.valid, "Missing worker declaration should be a protected fallback, not a validation crash.");
			context.ExpectEqual(std::string("protected"), TerraEngine::Scheduling::ToString(summary.queryPlans.front().executionPolicy), "Missing access declarations should select protected execution.");
			context.Expect(SummaryHasDiagnostic(summary, "missing-worker-declaration"), "Missing declaration should be diagnosed.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.DiagnosticsAreStableAcrossRepeatedRuns", [](TestContext& context) {
			std::string first;
			for (int run = 0; run < 100; ++run)
			{
				TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "stable", "Stable" });
				auto query = MakeQuery("Missing", "MissingQuery", MakeIds(2));
				query.access.readComponents.clear();
				graph.AddQuery(std::move(query));
				const std::string formatted = graph.Validate().formatted;
				if (run == 0)
				{
					first = formatted;
				}
				context.ExpectEqual(first, formatted, "ECS access diagnostics should be deterministic across repeated runs.");
			}
		});

		registry.Add("Engine.EcsQueryTaskGraph.LargeQueryChunksProcessEachObjectOnce", [](TestContext& context) {
			ThreadPoolSession session;
			const std::vector<TerraEngine::UUID> ids = MakeIds(10000);
			std::mutex mutex;
			std::set<std::string> processed;
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "large", "Large Query" });
			auto query = MakeQuery("LargeSystem", "LargeQuery", ids);
			query.workerThreshold = 16;
			query.preferredChunkSize = 257;
			query.work = [&mutex, &processed](const TerraEngine::Scheduling::EcsQueryChunk& chunk, TerraEngine::Scheduling::JobGraphContext&) {
				std::scoped_lock lock(mutex);
				for (const TerraEngine::UUID& id : chunk.objects)
				{
					processed.insert(TerraEngine::ToHexString(id));
				}
			};
			graph.AddQuery(std::move(query));

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Large query graph should complete.");
			context.Expect(summary.queryPlans.front().chunks.size() > 1, "Large query should be split into multiple chunks.");
			context.ExpectEqual(ids.size(), summary.queryPlans.front().processedObjectCount, "Chunk accounting should process every object exactly once.");
			context.ExpectEqual(ids.size(), processed.size(), "Chunk work should visit every object exactly once.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.SmallAndEmptyQueriesAvoidWorkerDispatch", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "small", "Small" });
			auto empty = MakeQuery("EmptySystem", "EmptyQuery", {});
			auto small = MakeQuery("SmallSystem", "SmallQuery", MakeIds(2));
			small.workerThreshold = 8;
			graph.AddQuery(std::move(empty));
			graph.AddQuery(std::move(small));

			const auto& summary = graph.Validate();

			context.ExpectEqual(static_cast<std::size_t>(0), summary.queryPlans[0].chunks.size(), "Empty query should have no chunks.");
			context.ExpectEqual(std::string("inline"), TerraEngine::Scheduling::ToString(summary.queryPlans[1].executionPolicy), "Small worker-safe query should stay inline to avoid worker overhead.");
			context.ExpectEqual(static_cast<std::size_t>(1), summary.queryPlans[1].chunks.size(), "Small query should use one deterministic chunk.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.ChunksCarryCompactObjectReferences", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			std::vector<TerraEngine::UUID> ids;
			for (std::size_t index = 0; index < 6; ++index)
			{
				Object object = objects.createObject("ChunkObject");
				ids.push_back(objects.getUUID(object.EntityId()));
			}
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "compact", "Compact References" });
			auto query = MakeQuery("CompactSystem", "CompactQuery", ids);
			query.workerThreshold = 4;
			query.preferredChunkSize = 3;
			graph.AddQuery(std::move(query));

			const auto& summary = graph.Validate();

			context.Expect(summary.valid, "Compact reference query fixture should validate.");
			context.ExpectEqual(ids.size(), summary.queryPlans.front().compactMatchingObjects.size(), "Query plan should snapshot one compact reference per object.");
			context.ExpectEqual(static_cast<std::size_t>(3), summary.queryPlans.front().chunks.front().compactObjects.size(), "Chunk should carry compact references beside UUIDs.");
			context.Expect(objects.validateCompactReference(summary.queryPlans.front().chunks.front().compactObjects.front()).validity == TerraEngine::CompactObjectReferenceValidity::Valid, "Chunk compact reference should validate against live ObjectManager state.");
			objects.clear();
		});

		registry.Add("Engine.EcsQueryTaskGraph.WorkerChunksCarryComponentAccessGrants", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "grants", "Access Grants" });
			auto query = MakeQuery("GrantSystem", "GrantQuery", MakeIds(8));
			query.workerThreshold = 4;
			query.preferredChunkSize = 2;
			query.access.writeComponents = { "WorldTransform" };
			graph.AddQuery(std::move(query));

			const auto& summary = graph.Validate();

			context.Expect(summary.valid, "Worker grant query fixture should validate.");
			context.Expect(summary.queryPlans.front().accessGrant.granted, "Worker-safe query plan should receive a scheduler-validated component access grant.");
			context.Expect(summary.queryPlans.front().chunks.front().accessGrant.granted, "Each worker chunk should carry the query access grant.");
			context.Expect(summary.formatted.find("grant=granted") != std::string::npos, "Formatted ECS summary should expose grant state.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.WorkerQueriesCarryPreparePhaseMetadata", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "phase", "Phase Metadata" });
			auto query = MakeQuery("PhaseSystem", "PhaseQuery", MakeIds(8));
			query.workerThreshold = 4;
			query.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
			graph.AddQuery(std::move(query));

			const auto& summary = graph.Validate();

			context.Expect(summary.valid, "Phase metadata fixture should validate.");
			context.Expect(summary.queryPlans.front().phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare, "Worker query should retain worker-prepare metadata.");
			context.Expect(summary.formatted.find("worker-prepare") != std::string::npos, "Formatted ECS summary should expose phase metadata.");
		});

		registry.Add("Engine.EcsQueryTaskGraph.DeferredMutationFlushInvalidatesPlans", [](TestContext& context) {
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "mutation", "Mutation" });
			graph.AddQuery(MakeQuery("Scene", "SceneQuery", MakeIds(2)));
			graph.Validate();
			graph.EnqueueMutation({ "create", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Created" });
			const auto state = graph.FlushMutations("After Scripting");
			const auto& summary = graph.LatestSummary();

			context.ExpectEqual(static_cast<std::size_t>(0), state.pendingAfterFlush, "Valid mutation flush should empty the queue.");
			context.Expect(summary.queryPlans.front().invalidatedByMutation, "Mutation flush should invalidate existing query plans.");
			ObjectManager::instance().clear();
		});

		registry.Add("Engine.EcsQueryTaskGraph.SchedulerConversionSummaryCanBeForwarded", [](TestContext& context) {
			TerraEngine::Scheduling::ClearPublishedEcsGraphFrameSummaries();
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "published", "Published" });
			auto query = MakeQuery("PublishedSystem", "PublishedQuery", MakeIds(2));
			query.workerThreshold = 8;
			graph.AddQuery(std::move(query));
			TerraEngine::Scheduling::PublishEcsGraphFrameSummary(graph.Execute());

			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.SetLatestComponentQueryGroups(TerraEngine::Scheduling::PublishedEcsComponentQueryGroups());
			scheduler.SetLatestEcsGraphSummary(TerraEngine::Scheduling::FormatPublishedEcsGraphFrameSummaries());
			const auto& summary = scheduler.LatestSummary();

			context.ExpectEqual(static_cast<std::size_t>(1), summary.componentQueryGroups.size(), "Scheduler summary should expose published ECS query groups.");
			context.Expect(summary.formatted.find("Published ECS graphs:") != std::string::npos, "Scheduler formatted summary should include published ECS graph text.");
			TerraEngine::Scheduling::ClearPublishedEcsGraphFrameSummaries();
		});
	}
}
