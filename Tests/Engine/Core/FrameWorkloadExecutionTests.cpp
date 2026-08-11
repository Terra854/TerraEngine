#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::Scheduling::FrameWorkloadDeclaration MakeWorkload(std::string name, std::vector<std::string> reads, std::vector<std::string> writes, bool workerSafe = true)
		{
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			workload.workloadName = std::move(name);
			workload.ownerSystem = "FrameWorkloadTests";
			workload.phase = "Scene";
			workload.reads = std::move(reads);
			workload.writes = std::move(writes);
			workload.workerEligible = workerSafe;
			workload.mainThreadRequired = !workerSafe;
			workload.protectedReason = workerSafe ? std::string{} : "fixture protected path";
			return workload;
		}
	}

	void RegisterFrameWorkloadExecutionTests(TestRegistry& registry)
	{
		registry.Add("Engine.FrameWorkload.ReadReadCanRunWorkerSafe", [](TestContext& context) {
			std::atomic<int> calls = 0;
			auto first = MakeWorkload("A", { "Transform" }, {});
			auto second = MakeWorkload("B", { "Transform" }, {});
			first.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) { calls.fetch_add(1); };
			second.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) { calls.fetch_add(1); };

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads({ first, second }, "read-read");

			context.Expect(plan.valid, "Read/read worker-safe workloads should validate.");
			context.Expect(plan.completed, "Read/read worker-safe workloads should execute.");
			context.ExpectEqual(2, calls.load(), "Both worker-safe workload callbacks should run.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.migrationSummary.workerSafeCount, "Both workloads should be counted as worker-safe.");
		});

		registry.Add("Engine.FrameWorkload.CompactExecutionOmitsOptionalEvidence", [](TestContext& context) {
			auto workload = MakeWorkload("Compact", { "Snapshot" }, { "Prepared" });
			workload.work = [](TerraEngine::Scheduling::JobGraphContext&) {};
			TerraEngine::Scheduling::FrameWorkloadExecutionOptions options;
			options.collectExecutionRecords = false;
			options.formatDiagnostics = false;

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(
				{ workload }, "compact-execution", options);

			context.Expect(plan.valid && plan.completed,
				"Compact execution must preserve workload correctness and completion.");
			context.Expect(plan.executionRecords.empty(),
				"Compact execution must omit optional per-partition profiler evidence.");
			context.Expect(plan.formatted.empty() && plan.migrationSummary.formatted.empty(),
				"Compact execution must avoid rebuilding optional diagnostic strings.");
		});

		registry.Add("Engine.FrameWorkload.CompletedEvidenceConstructsAvailableClocksDirectly", [](TestContext& context) {
			auto workload = MakeWorkload("CompletedEvidence", { "Snapshot" }, { "Prepared" });
			workload.work = [](TerraEngine::Scheduling::JobGraphContext&) {};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(
				{ workload }, "completed-evidence");

			context.Expect(plan.valid && plan.completed,
				"A completed workload must still produce detailed scheduler evidence.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.executionRecords.size(),
				"The completed workload must retain exactly one execution record.");
			if (plan.executionRecords.empty())
			{
				return;
			}

			const auto& record = plan.executionRecords.front();
			context.Expect(record.queueDuration.availability
				== TerraEngine::Profiling::TelemetryAvailability::Available
				&& record.queueDuration.unavailableReason.empty(),
				"Completed queue timing must remain available without a stale unavailable reason.");
			context.Expect(record.runDuration.availability
				== TerraEngine::Profiling::TelemetryAvailability::Available
				&& record.runDuration.unavailableReason.empty(),
				"Completed run timing must remain available without a stale unavailable reason.");
			context.Expect(record.coordinatorWaitDuration.availability
				== TerraEngine::Profiling::TelemetryAvailability::Available
				&& record.coordinatorWaitDuration.unavailableReason.empty(),
				"Completed coordinator timing must remain available without a stale unavailable reason.");
			context.Expect(record.ownershipWaitDuration.availability
				== TerraEngine::Profiling::TelemetryAvailability::Unavailable
				&& !record.ownershipWaitDuration.unavailableReason.empty(),
				"Unmeasured ownership timing must retain its explicit unavailable explanation.");
		});

		registry.Add("Engine.FrameWorkload.ProfilerEvidenceTransfersExactlyOnce", [](TestContext& context) {
			auto workload = MakeWorkload("TransferEvidence", { "Snapshot" }, { "Prepared" });
			workload.work = [](TerraEngine::Scheduling::JobGraphContext&) {};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(
				{ workload }, "transfer-evidence");
			auto transferred = TerraEngine::Scheduling::TakeLatestFrameWorkloadExecutionRecordsForProfiling();
			auto repeated = TerraEngine::Scheduling::TakeLatestFrameWorkloadExecutionRecordsForProfiling();

			context.Expect(plan.valid && plan.completed,
				"The scheduler must complete before profiler evidence is transferred.");
			context.ExpectEqual(plan.executionRecords.size(), transferred.size(),
				"The sole profiler consumer must receive every published execution record.");
			context.Expect(!transferred.empty() && transferred.front().workloadName == "TransferEvidence",
				"Transferred records must preserve their stable workload identity.");
			context.Expect(repeated.empty(),
				"A published execution-record vector must transfer only once per scheduler frame.");
		});

		registry.Add("Engine.FrameWorkload.WriteConflictRejectedBeforeExecution", [](TestContext& context) {
			auto first = MakeWorkload("A", {}, { "ObjectManager" });
			auto second = MakeWorkload("B", { "ObjectManager" }, {});

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ first, second }, "conflict");

			context.Expect(!plan.valid, "Unordered worker read/write conflicts should reject the frame plan.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.migrationSummary.rejectedCount, "Both conflicting workloads should be rejected.");
			context.Expect(plan.formatted.find("access-conflict") != std::string::npos, "Summary should explain the access conflict.");
		});

		registry.Add("Engine.FrameWorkload.OrderedWriteReadHandoffIsValid", [](TestContext& context) {
			auto ui = MakeWorkload("UISystem", {}, { "UI" });
			auto render = MakeWorkload("RenderSystem", { "UI" }, {});
			ui.stableDescriptorId = "scheduler/ui";
			render.stableDescriptorId = "scheduler/render";
			render.explicitDependencies = {ui.stableDescriptorId};

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ render, ui }, "ordered-handoff");

			context.Expect(plan.valid, "Worker-safe handoffs with an explicit descriptor dependency should be valid.");
			context.ExpectEqual(static_cast<std::size_t>(0), plan.migrationSummary.rejectedCount, "Ordered handoffs should not be counted as rejected workloads.");
			context.Expect(plan.formatted.find("access-conflict") == std::string::npos, "Ordered handoffs should not report an access conflict.");
		});

		registry.Add("Engine.FrameWorkload.TransitiveReverseOrderDoesNotCreateConflictCycle", [](TestContext& context) {
			std::atomic<int> stage = 0;
			std::atomic<bool> ordered = true;
			auto consumer = MakeWorkload("AConsumer", { "SharedState" }, {});
			consumer.stableDescriptorId = "scheduler/a-consumer";
			consumer.explicitDependencies = { "scheduler/b-relay" };
			consumer.work = [&stage, &ordered](TerraEngine::Scheduling::JobGraphContext&) {
				ordered.store(ordered.load() && stage.load() == 2);
				stage.store(3);
			};
			auto relay = MakeWorkload("BRelay", { "RelayInput" }, { "RelayOutput" });
			relay.stableDescriptorId = "scheduler/b-relay";
			relay.explicitDependencies = { "scheduler/c-producer" };
			relay.work = [&stage, &ordered](TerraEngine::Scheduling::JobGraphContext&) {
				ordered.store(ordered.load() && stage.load() == 1);
				stage.store(2);
			};
			auto producer = MakeWorkload("CProducer", {}, { "SharedState", "RelayInput" });
			producer.stableDescriptorId = "scheduler/c-producer";
			producer.work = [&stage, &ordered](TerraEngine::Scheduling::JobGraphContext&) {
				ordered.store(ordered.load() && stage.load() == 0);
				stage.store(1);
			};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(
				{ consumer, relay, producer }, "transitive-reverse-order");

			context.Expect(plan.valid && plan.completed,
				"A transitive producer-to-consumer path must remain valid when descriptor sorting places the consumer first.");
			context.Expect(ordered.load() && stage.load() == 3,
				"The existing transitive dependency path must govern execution without a reverse resource edge.");
			context.Expect(plan.formatted.find("explicit-dependency-cycle") == std::string::npos,
				"Resource lowering must not introduce a cycle opposite an explicit transitive dependency path.");
		});

		registry.Add("Engine.FrameWorkload.WorkerPreparedDeferredMutationHandoffIsCovered", [](TestContext& context) {
			auto workerPrepare = MakeWorkload("WorkerTransactionPrepare", { "SceneGraph" }, { "SceneMutationQueue" });
			workerPrepare.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
			workerPrepare.mutationPolicy = TerraEngine::Scheduling::FrameWorkloadMutationPolicy::DeferredOnly;

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workerPrepare }, "worker-transaction-handoff");

			context.Expect(plan.valid, "Worker-prepared transaction payloads should validate when writes are deferred through SceneMutationQueue.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.workerSafeCount, "The worker preparation workload should stay worker-safe.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.mutationFlushCount, "Deferred transaction handoff should be counted for scheduler diagnostics.");
			context.Expect(plan.formatted.find("deferred-only") != std::string::npos, "Formatted frame plan should expose the deferred mutation policy.");
		});

		registry.Add("Engine.FrameWorkload.CompatibilityModeReportsInlinePolicy", [](TestContext& context) {
			std::atomic<int> calls = 0;
			auto tiny = MakeWorkload("Tiny", { "SceneGraph" }, {});
			tiny.estimatedWorkItemCount = 2;
			tiny.inlineWorkItemThreshold = 8;
			tiny.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) {
				calls.fetch_add(1);
			};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility({ tiny }, "compat-inline");

			context.Expect(plan.valid, "Compatibility execution should validate inline-capable worker metadata.");
			context.Expect(plan.completed, "Compatibility execution should still run the inline-capable workload.");
			context.ExpectEqual(1, calls.load(), "Compatibility execution should invoke the workload once.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.inlineCount, "Compatibility diagnostics should preserve inline threshold decisions.");
			context.Expect(plan.formatted.find("mode=compatibility") != std::string::npos, "Formatted plan should expose compatibility mode.");
			context.Expect(plan.formatted.find("policy=inline") != std::string::npos, "Formatted plan should expose inline policy selection.");
		});

		registry.Add("Engine.FrameWorkload.ProtectedFallbackIsReported", [](TestContext& context) {
			auto protectedWork = MakeWorkload("Protected", { "SceneGraph" }, {}, false);

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ protectedWork }, "protected");

			context.Expect(plan.valid, "Protected fallback should be a valid plan.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.protectedCount, "Protected workload should be counted.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.fallbackCount, "Fallback reason should be counted.");
		});

		registry.Add("Engine.FrameWorkload.RepeatedSummariesAreDeterministic", [](TestContext& context) {
			auto first = MakeWorkload("A", { "A" }, {});
			auto second = MakeWorkload("B", { "B" }, {});
			const std::string baseline = TerraEngine::Scheduling::ValidateFrameWorkloads({ first, second }, "repeat").formatted;
			for (int index = 0; index < 1000; ++index)
			{
				context.ExpectEqual(baseline, TerraEngine::Scheduling::ValidateFrameWorkloads({ second, first }, "repeat").formatted, "Frame workload summaries should not depend on input order.");
			}
		});

		registry.Add("Engine.FrameWorkload.InactiveAndNotReadyDeclarationsAreOmitted", [](TestContext& context) {
			std::atomic<int> calls = 0;
			auto inactive = MakeWorkload("Inactive", { "Scene" }, {});
			inactive.active = false;
			inactive.activationPredicateKey = "simulation-has-work";
			inactive.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) { calls.fetch_add(1); };
			auto notReady = MakeWorkload("NotReady", { "Scene" }, {});
			notReady.ready = false;
			notReady.readinessReason = "snapshot has not published";
			notReady.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) { calls.fetch_add(1); };

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads({ inactive, notReady }, "omitted-work");

			context.Expect(plan.valid, "Inactive and not-ready descriptors should not invalidate the remaining frame plan.");
			context.Expect(plan.completed, "A frame containing only omitted descriptors should complete without a graph job.");
			context.ExpectEqual(0, calls.load(), "Omitted descriptors must not invoke their callbacks.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.migrationSummary.omittedCount, "Both inactive and not-ready descriptors should be reported as omitted.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.executionRecords.size(), "Omitted descriptors should still publish bounded decision evidence.");
			context.Expect(plan.executionRecords[0].dispatchCount == 0u && plan.executionRecords[1].dispatchCount == 0u, "Omitted evidence must not report task dispatches.");
			for (const auto& record : plan.executionRecords)
			{
				context.Expect(TerraEngine::Profiling::ValidateWorkloadExecutionRecord(record).valid, "Omitted decision evidence must remain a complete value-only telemetry record.");
			}
		});

		registry.Add("Engine.FrameWorkload.ExplicitDependenciesGateExecution", [](TestContext& context) {
			std::atomic<bool> producerCompleted = false;
			std::atomic<bool> consumerObservedCompletion = false;
			auto producer = MakeWorkload("Producer", { "Input" }, { "Prepared" });
			producer.stableDescriptorId = "scheduler/producer";
			producer.work = [&producerCompleted](TerraEngine::Scheduling::JobGraphContext&) { producerCompleted.store(true); };
			auto consumer = MakeWorkload("Consumer", { "Prepared" }, { "Output" });
			consumer.stableDescriptorId = "scheduler/consumer";
			consumer.explicitDependencies = { "scheduler/producer" };
			consumer.work = [&producerCompleted, &consumerObservedCompletion](TerraEngine::Scheduling::JobGraphContext&) {
				consumerObservedCompletion.store(producerCompleted.load());
			};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads({ consumer, producer }, "explicit-dependencies");

			context.Expect(plan.valid && plan.completed, "Valid explicit descriptor dependencies should produce a completed frame.");
			context.Expect(consumerObservedCompletion.load(), "The consumer must execute only after its explicit producer dependency completes.");
			context.Expect(plan.formatted.find("explicit-dependency-cycle") == std::string::npos, "A directed dependency pair must not be diagnosed as a cycle.");
		});

		registry.Add("Engine.FrameWorkload.StableIdentityAndDeterministicRangesAreValidated", [](TestContext& context) {
			auto duplicateA = MakeWorkload("DuplicateA", { "A" }, {});
			duplicateA.stableDescriptorId = "cached/duplicate";
			auto duplicateB = MakeWorkload("DuplicateB", { "B" }, {});
			duplicateB.stableDescriptorId = "cached/duplicate";
			const auto invalidPlan = TerraEngine::Scheduling::ValidateFrameWorkloads({ duplicateA, duplicateB }, "duplicate-descriptor");
			context.Expect(!invalidPlan.valid, "Duplicate cached descriptor identities must reject the frame plan.");

			std::mutex rangeMutex;
			std::vector<TerraEngine::Scheduling::FrameWorkloadPartition> ranges;
			auto partitioned = MakeWorkload("Partitioned", { "Snapshot" }, { "Packets" });
			partitioned.stableDescriptorId = "cached/partitioned";
			partitioned.estimatedWorkItemCount = 10u;
			partitioned.partitionPolicy = TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges;
			partitioned.partitionWorkItemCount = 3u;
			partitioned.rangeWork = [&rangeMutex, &ranges](const TerraEngine::Scheduling::FrameWorkloadPartition& partition, TerraEngine::Scheduling::JobGraphContext&) {
				std::lock_guard lock(rangeMutex);
				ranges.push_back(partition);
			};
			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads({ partitioned }, "deterministic-ranges");
			std::stable_sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) { return left.partitionIndex < right.partitionIndex; });

			context.Expect(plan.valid && plan.completed, "A partitioned immutable range workload should complete.");
			context.ExpectEqual(static_cast<std::size_t>(4), ranges.size(), "A ten-item workload with three-item targets should use four stable ranges.");
			context.Expect(ranges.size() == 4u && ranges.front().itemBegin == 0u && ranges.back().itemEnd == 10u, "Deterministic ranges must cover the complete item domain.");
			for (std::size_t index = 1; index < ranges.size(); ++index)
			{
				context.Expect(ranges[index - 1u].itemEnd == ranges[index].itemBegin, "Deterministic ranges must be contiguous without overlaps or gaps.");
			}
			for (const auto& record : plan.executionRecords)
			{
				context.Expect(TerraEngine::Profiling::ValidateWorkloadExecutionRecord(record).valid, "Partition execution evidence must remain a valid bounded value record.");
				if(plan.executionMode
					== TerraEngine::Scheduling::FrameWorkloadExecutionMode::Parallel
					&& record.selectedPath
					== TerraEngine::Profiling::WorkloadExecutionPath::Worker
					&& record.status
						== TerraEngine::Profiling::WorkloadExecutionStatus::Completed)
				{
					context.Expect(record.completionSequence != 0u,
						"Completed worker range '" + record.descriptorId
							+ "' must expose its actual completion order (dispatch="
							+ std::to_string(record.dispatchCount) + ", wave="
							+ std::to_string(record.dependencyWave) + ").");
				}
				context.Expect(record.runStartNanoseconds != 0u
					&& record.runEndNanoseconds >= record.runStartNanoseconds,
					"Executed partition '" + record.workloadName
						+ "' must retain scheduler wall endpoints for non-additive profiler attribution (start="
						+ std::to_string(record.runStartNanoseconds) + ", end="
						+ std::to_string(record.runEndNanoseconds) + ", dispatch="
						+ std::to_string(record.dispatchCount) + ", run="
						+ std::to_string(record.runDuration.nanoseconds) + ").");
			}
			if(plan.executionMode
				== TerraEngine::Scheduling::FrameWorkloadExecutionMode::Parallel)
			{
				context.Expect(plan.migrationSummary.dependencyWaveCount != 0u
					&& plan.migrationSummary.workerCompletionWakeCount != 0u
					&& plan.migrationSummary.peakConcurrentWorkerCount != 0u
					&& plan.migrationSummary.criticalPathTimeNs != 0u,
					"Parallel frame evidence must retain dependency shape, completion wakes, concurrency, and the measured critical path.");
			}
		});
	}
}
