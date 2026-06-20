#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderRandomizedSchedulingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.RandomizedScheduling.CompletionOrderDoesNotChangePacketCount", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(13u, 32u);
			auto profile = MakeThreadedRendererProfile();
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			auto plan = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			const auto before = plan.packets.size();
			RandomizePacketCompletionOrder(plan.packets);
			context.ExpectEqual(before, plan.packets.size(), "Randomized completion should not create or drop packets.");
			context.Expect(plan.packets.front().state == TerraEngine::Renderer::RenderPacketState::Completed, "Stress helper should mark packets completed in randomized order.");
		});
	}

}
