#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::Scheduling::FrameWorkloadDeclaration WorkerCategory(std::string name, std::string resource)
		{
			return TerraEngine::Scheduling::MakeWorkerPreparePhase(
				std::move(name),
				"Scenario",
				{ std::move(resource) },
				{},
				"worker-scenario");
		}
	}

	void RegisterWorkerConversionScenarioTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorkerConversionScenario.LargeSceneReportsFiveWorkerCategories", [](TestContext& context) {
			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({
				WorkerCategory("SceneGraphPrepare", "SceneGraph"),
				WorkerCategory("SpatialPrepare", "SpatialIndex"),
				WorkerCategory("RenderPrepare", "Renderer"),
				WorkerCategory("UiPrepare", "UI"),
				WorkerCategory("ScriptingPrepare", "Scripting")
			}, "large-scene");
			const auto report = TerraEngine::Scheduling::BuildWorkerCoverageReport(plan);

			context.Expect(plan.valid, "Independent worker-preparation categories should validate.");
			context.ExpectEqual(static_cast<std::size_t>(5), report.workerCategories.size(), "Large-scene fixture should report five worker categories.");
		});

		registry.Add("Engine.WorkerConversionScenario.RepeatedCoverageSummariesAreStable", [](TestContext& context) {
			const auto first = TerraEngine::Scheduling::BuildWorkerCoverageReport(TerraEngine::Scheduling::ValidateFrameWorkloads({
				WorkerCategory("B", "B"),
				WorkerCategory("A", "A")
			}, "repeat"));
			for (int index = 0; index < 1000; ++index)
			{
				const auto next = TerraEngine::Scheduling::BuildWorkerCoverageReport(TerraEngine::Scheduling::ValidateFrameWorkloads({
					WorkerCategory("A", "A"),
					WorkerCategory("B", "B")
				}, "repeat"));
				context.ExpectEqual(first.formattedSummary, next.formattedSummary, "Coverage summaries should not depend on declaration order.");
			}
		});

		registry.Add("Engine.WorkerConversionScenario.EmptySceneHasNoWorkerOverheadCategories", [](TestContext& context) {
			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({}, "empty-scene");
			const auto report = TerraEngine::Scheduling::BuildWorkerCoverageReport(plan);

			context.Expect(plan.valid, "Empty scenes should validate.");
			context.Expect(report.workerCategories.empty(), "Empty scenes should not invent worker categories.");
			context.Expect(report.protectedCategories.empty(), "Empty scenes should not invent protected categories.");
		});
	}
}
