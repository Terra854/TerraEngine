#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

#include <algorithm>

namespace TerraEngine::Tests {
	namespace {
		const TerraEngine::Scheduling::HardBoundary* FindBoundary(const std::vector<TerraEngine::Scheduling::HardBoundary>& boundaries, const std::string& name)
		{
			const auto found = std::find_if(boundaries.begin(), boundaries.end(), [&](const TerraEngine::Scheduling::HardBoundary& boundary) {
				return boundary.boundaryName == name;
			});
			return found == boundaries.end() ? nullptr : &(*found);
		}
	}

	void RegisterWorkerHardBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorkerHardBoundary.InitialClassificationsMatchPlan", [](TestContext& context) {
			const auto boundaries = TerraEngine::Scheduling::InitialWorkerHardBoundaries();

			context.ExpectEqual(static_cast<std::size_t>(4), boundaries.size(), "The feature should report all four named hard boundaries.");
			context.Expect(FindBoundary(boundaries, "ManagedGameplayCallbacks")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Managed callbacks should be partially worker-safe.");
			context.Expect(FindBoundary(boundaries, "LiveGpuSubmission")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Live GPU submission should be partially worker-safe.");
			context.Expect(FindBoundary(boundaries, "WindowInputPolling")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected, "Window/input polling should stay protected.");
			context.Expect(FindBoundary(boundaries, "EditorReflectionMutation")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected, "Editor reflection mutation should stay protected.");
		});

		registry.Add("Engine.WorkerHardBoundary.FindingsNameSafeAndProtectedPortions", [](TestContext& context) {
			for (const TerraEngine::Scheduling::ConversionFinding& finding : TerraEngine::Scheduling::InitialWorkerConversionFindings())
			{
				context.Expect(!finding.targetName.empty(), "Findings should name their target.");
				context.Expect(!finding.evidence.empty(), "Findings should record worker-safe evidence or safe subwork.");
				context.Expect(!finding.blockingReason.empty(), "Findings should record protected blocking reasons.");
				context.Expect(!finding.recommendedAction.empty(), "Findings should record reevaluation conditions.");
			}
		});
	}
}
