#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/Graph/RenderWorkerTaskGroup.h"

namespace TerraEngine::Tests {

	void RegisterRenderSmallSceneOverheadTests(TestRegistry& registry)
	{
		registry.Add("Renderer.SmallScene.InlinesBelowThreshold", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(6u, 2u);
			auto profile = MakeThreadedRendererProfile();
			profile.workerSchedulingThreshold = 32u;
			const auto group = TerraEngine::Renderer::BuildRenderWorkerTaskGroup(snapshot, profile, "small", 2u);
			context.Expect(!TerraEngine::Renderer::ShouldRunRenderGroupOnWorkers(group), "Small scenes should avoid worker scheduling overhead.");
			context.Expect(group.inlinedForSmallWork, "Small scene serial fallback should be reported as threshold inlining.");
		});
	}

}
