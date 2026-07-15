#include "../TestHarness.h"

#include "FullParallelFrameTestFixtures.h"

#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Scheduling/WorkerSafety.h"

#include <atomic>

namespace TerraEngine::Tests {
	void RegisterFullParallelFrameExecutionTests(TestRegistry& registry)
	{
		registry.Add("Engine.FullParallelFrame.WorkerEligibleWorkOverlaps", [](TestContext& context) {
			FullParallelFrameFixtures::ThreadPoolSession session;
			std::atomic<int> activeWorkers = 0;
			std::atomic<int> peakWorkers = 0;
			auto workloads = FullParallelFrameFixtures::MakeOverlappingWorkerWorkloads(8, activeWorkers, peakWorkers);

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(std::move(workloads), "full-parallel-overlap");
			const auto violations = TerraEngine::Scheduling::DrainWorkerSafetyViolations();

			context.Expect(plan.valid, "Parallel worker workload plan should validate.");
			context.Expect(plan.completed, "Parallel worker workload plan should complete.");
			context.Expect(plan.parallelExecutionUsed, "Parallel execution flag should be set when worker work runs.");
			context.Expect(peakWorkers.load() > 1, "At least two worker callbacks should overlap.");
			context.Expect(violations.empty(), "Worker callbacks should receive a worker safety scope.");
		});

		registry.Add("Engine.FullParallelFrame.CompatibilityBaselineDoesNotUseWorkers", [](TestContext& context) {
			std::atomic<int> calls = 0;
			auto worker = FullParallelFrameFixtures::MakeWorkerWorkload("Worker", { "Scene" }, {});
			worker.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) {
				calls.fetch_add(1);
			};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility({ worker }, "compat-baseline");

			context.Expect(plan.valid, "Compatibility baseline should validate worker metadata.");
			context.Expect(plan.completed, "Compatibility baseline should still execute callbacks.");
			context.Expect(!plan.parallelExecutionUsed, "Compatibility baseline should not report worker execution.");
			context.ExpectEqual(1, calls.load(), "Compatibility baseline should run the callback once.");
			context.Expect(plan.formatted.find("mode=compatibility") != std::string::npos, "Formatted plan should expose compatibility mode.");
		});

		registry.Add("Engine.FullParallelFrame.DiagnosticsIncludeTimingAndUtilization", [](TestContext& context) {
			FullParallelFrameFixtures::ThreadPoolSession session;
			std::atomic<int> activeWorkers = 0;
			std::atomic<int> peakWorkers = 0;
			auto workloads = FullParallelFrameFixtures::MakeOverlappingWorkerWorkloads(4, activeWorkers, peakWorkers);

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(std::move(workloads), "full-parallel-diagnostics");

			context.Expect(plan.formatted.find("workerUtilization=") != std::string::npos, "Formatted diagnostics should include worker utilization.");
			context.Expect(plan.migrationSummary.workerPreparationTimeNs > 0, "Worker execution time should be recorded.");
		});

		registry.Add("Engine.FullParallelFrame.UnorderedWorkerWriteConflictIsRejected", [](TestContext& context) {
			auto writer = FullParallelFrameFixtures::MakeWorkerWorkload("Writer", {}, { "ObjectManager" });
			auto reader = FullParallelFrameFixtures::MakeWorkerWorkload("Reader", { "ObjectManager" }, {});
			writer.executionOrder = 4;
			reader.executionOrder = 4;

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ writer, reader }, "unordered-conflict");

			context.Expect(!plan.valid, "Same-order worker write/read conflicts should reject before execution.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.migrationSummary.rejectedCount, "Both conflicting worker workloads should be rejected.");
			context.Expect(plan.formatted.find("access-conflict") != std::string::npos, "Diagnostics should explain the rejected access conflict.");
		});

		registry.Add("Engine.FullParallelFrame.OrderedWorkerHandoffIsAccepted", [](TestContext& context) {
			auto writer = FullParallelFrameFixtures::MakeWorkerWorkload("Writer", {}, { "UI" });
			auto reader = FullParallelFrameFixtures::MakeWorkerWorkload("Reader", { "UI" }, {});
			writer.stableDescriptorId = "parallel/writer";
			reader.stableDescriptorId = "parallel/reader";
			reader.explicitDependencies = {writer.stableDescriptorId};

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ reader, writer }, "ordered-handoff");

			context.Expect(plan.valid, "An explicit descriptor dependency should form an ordered handoff instead of a conflict.");
			context.ExpectEqual(static_cast<std::size_t>(0), plan.migrationSummary.rejectedCount, "Ordered handoffs should not reject worker workloads.");
		});

		registry.Add("Engine.FullParallelFrame.SmallWorkerWorkRunsInlineWithoutThreadPool", [](TestContext& context) {
			std::atomic<int> calls = 0;
			auto workload = FullParallelFrameFixtures::MakeWorkerWorkload("TinyWorker", { "SceneGraph" }, {});
			workload.estimatedWorkItemCount = 4;
			workload.inlineWorkItemThreshold = 8;
			workload.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) {
				calls.fetch_add(1);
			};

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads({ workload }, "tiny-inline");

			context.Expect(plan.valid, "Small worker-capable workloads should still validate.");
			context.Expect(plan.completed, "Small worker-capable workloads should execute inline without requiring ThreadPool startup.");
			context.Expect(!plan.parallelExecutionUsed, "Inline worker-capable workloads should not report worker dispatch.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.inlineCount, "Inline threshold should be visible in migration diagnostics.");
			context.ExpectEqual(1, calls.load(), "Inline workload should run once on the caller thread.");
			context.Expect(plan.formatted.find("policy=inline") != std::string::npos, "Formatted diagnostics should expose inline policy selection.");
		});
	}
}
