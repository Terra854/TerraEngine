#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

namespace TerraEngine::Tests {
	void RegisterArchitectureMigrationSummaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.ArchitectureMigrationSummary.CountsWorkerProtectedAndRejected", [](TestContext& context) {
			TerraEngine::Scheduling::FrameWorkloadDeclaration worker;
			worker.workloadName = "Worker";
			worker.ownerSystem = "Runtime";
			worker.phase = "Scene";
			worker.reads = { "A" };
			worker.workerEligible = true;
			worker.mainThreadRequired = false;

			TerraEngine::Scheduling::FrameWorkloadDeclaration protectedWork;
			protectedWork.workloadName = "Protected";
			protectedWork.ownerSystem = "Runtime";
			protectedWork.phase = "Scene";
			protectedWork.reads = { "B" };
			protectedWork.protectedReason = "fixture";

			TerraEngine::Scheduling::FrameWorkloadDeclaration rejected;
			rejected.workloadName = "Rejected";
			rejected.ownerSystem = "Runtime";
			rejected.phase = "Scene";
			rejected.workerEligible = true;
			rejected.mainThreadRequired = false;

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ worker, protectedWork, rejected }, "migration");

			context.Expect(!plan.valid, "Missing worker access should reject the plan.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.workerSafeCount, "One workload should be worker-safe.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.protectedCount, "One workload should be protected.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.rejectedCount, "One workload should be rejected.");
		});
	}
}
