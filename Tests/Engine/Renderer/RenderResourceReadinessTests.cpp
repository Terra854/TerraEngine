#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderResourceReadinessTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ResourceReadiness.FallbackResourcesAreFrameSafe", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(9u, 1u);
			InjectResourceDelay(snapshot, "streaming texture");
			context.Expect(TerraEngine::Renderer::SnapshotUsesOnlyResolvedResources(snapshot), "Pending optional resources with fallbacks should remain frame safe.");
			context.ExpectEqual(std::string("streaming texture"), snapshot.textures.front().fallbackReason, "Fallback reasons should be preserved for diagnostics.");
		});
	}

}
