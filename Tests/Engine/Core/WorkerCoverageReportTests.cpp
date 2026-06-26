#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

namespace TerraEngine::Tests {
	void RegisterWorkerCoverageReportTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorkerCoverageReport.MixedCategoriesAreFormattedDeterministically", [](TestContext& context) {
			auto worker = TerraEngine::Scheduling::MakeWorkerPreparePhase("Worker", "Coverage", { "SceneGraph" }, {}, {});
			auto protectedCommit = TerraEngine::Scheduling::MakeProtectedCommitPhase("Commit", "Coverage", { "SceneGraph" }, { "SceneGraph" }, "commit boundary", {});
			auto rejected = TerraEngine::Scheduling::MakeWorkerPreparePhase("Rejected", "Coverage", { "ObjectManager" }, { "ObjectManager" }, {});
			auto conflicting = TerraEngine::Scheduling::MakeWorkerPreparePhase("Conflicting", "Coverage", { "ObjectManager" }, {}, {});

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ protectedCommit, rejected, worker, conflicting }, "coverage");
			const auto report = TerraEngine::Scheduling::BuildWorkerCoverageReport(plan);

			context.Expect(!plan.valid, "The fixture intentionally contains a worker conflict.");
			context.Expect(!report.workerCategories.empty(), "Report should keep safe worker categories.");
			context.Expect(!report.protectedCategories.empty(), "Report should keep protected categories.");
			context.Expect(!report.rejectedCategories.empty(), "Report should keep rejected categories.");
			context.Expect(report.formattedSummary.find("Worker coverage report") != std::string::npos, "Report should have a stable formatted summary.");
		});
	}
}
