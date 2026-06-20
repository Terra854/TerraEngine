#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderResourceCancellationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ResourceCancellation.StaleTokenBlocksPublication", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(11u, 8u);
			auto profile = MakeThreadedRendererProfile();
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			auto packets = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			context.Expect(!packets.packets.empty(), "Synthetic workload should create packets.");
			context.Expect(TerraEngine::Renderer::IsRenderWorkPacketStale(packets.packets.front(), snapshot.sceneRevision + 1u, snapshot.frameId), "Changed owner revision should make queued work stale.");
		});
	}

}
