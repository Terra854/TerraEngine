#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"

namespace TerraEngine::EditorTests {
	void RegisterArchitectureHardeningPlayModeTests(TestRegistry& registry)
	{
		registry.Add("Editor.ArchitectureHardening.ProtectedFallbackReasonIsVisible", [](TestContext& context) {
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			workload.workloadName = "EditorPlayMode";
			workload.ownerSystem = "Editor";
			workload.phase = "Editor";
			workload.reads = { "Editor" };
			workload.protectedReason = "editor play-mode compatibility";

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "editor-play-mode");

			context.Expect(plan.valid, "Editor protected fallback should validate.");
			context.Expect(plan.formatted.find("editor play-mode compatibility") != std::string::npos, "Protected fallback reason should be visible.");
		});
	}
}
