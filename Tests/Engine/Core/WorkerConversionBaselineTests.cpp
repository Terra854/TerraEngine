#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::Scheduling::FrameWorkloadDeclaration ProtectedBaselineWork(std::string name)
		{
			auto declaration = TerraEngine::Scheduling::MakeFallbackPhase(
				std::move(name),
				"Baseline",
				{ "SceneGraph" },
				{ "SceneGraph" },
				"Compatibility baseline keeps the workload protected.");
			return declaration;
		}
	}

	void RegisterWorkerConversionBaselineTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorkerConversionBaseline.ProtectedCompatibilityIsReported", [](TestContext& context) {
			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({
				ProtectedBaselineWork("SceneBaseline")
			}, "baseline");
			const auto report = TerraEngine::Scheduling::BuildWorkerCoverageReport(plan);

			context.Expect(plan.valid, "Protected compatibility work should remain a valid baseline.");
			context.ExpectEqual(static_cast<std::size_t>(1), report.protectedCategories.size(), "Baseline work should be counted as protected.");
			context.ExpectEqual(static_cast<std::size_t>(1), report.fallbackCategories.size(), "Baseline work should carry a fallback reason.");
			context.Expect(report.formattedSummary.find("Compatibility baseline") != std::string::npos, "Formatted baseline report should explain the compatibility path.");
		});
	}
}
