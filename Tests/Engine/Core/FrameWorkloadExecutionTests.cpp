#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

#include <atomic>
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

		registry.Add("Engine.FrameWorkload.WriteConflictRejectedBeforeExecution", [](TestContext& context) {
			auto first = MakeWorkload("A", {}, { "ObjectManager" });
			auto second = MakeWorkload("B", { "ObjectManager" }, {});

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ first, second }, "conflict");

			context.Expect(!plan.valid, "Unordered worker read/write conflicts should reject the frame plan.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.migrationSummary.rejectedCount, "Both conflicting workloads should be rejected.");
			context.Expect(plan.formatted.find("access-conflict") != std::string::npos, "Summary should explain the access conflict.");
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
	}
}
