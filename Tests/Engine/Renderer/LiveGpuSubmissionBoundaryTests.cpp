#include "../TestHarness.h"

#include "Renderer/RenderSystem.h"

namespace TerraEngine::Tests {
	void RegisterLiveGpuSubmissionBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.LiveGpuSubmissionBoundary.RenderSystemIsPartialWorkerSafe", [](TestContext& context) {
			const TerraEngine::Scheduling::SystemDeclaration declaration = RenderSystem::Instance().SchedulerDeclaration();

			context.Expect(declaration.hardBoundary, "RenderSystem should advertise the live GPU hard boundary.");
			context.Expect(declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Live GPU submission should be partial worker-safe.");
			context.Expect(!declaration.mainThread, "Render preparation should remain worker eligible.");
			context.Expect(declaration.safeSubwork.find("snapshots") != std::string::npos, "Safe subwork should describe render snapshot preparation.");
			context.Expect(declaration.protectedCommit.find("present") != std::string::npos, "Protected commit should describe present ownership.");
		});
	}
}
