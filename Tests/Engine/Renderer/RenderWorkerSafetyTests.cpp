#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderWorkerSafetyTests(TestRegistry& registry)
	{
		registry.Add("Renderer.WorkerSafety.LiveStateLeakInvalidatesSnapshot", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(16u, 16u);
			snapshot.containsLiveMutableState = true;
			context.Expect(TerraEngine::Renderer::SnapshotContainsLiveMutableState(snapshot), "Worker safety validation should flag live mutable state consumption.");
			context.Expect(!TerraEngine::Renderer::SnapshotUsesOnlyResolvedResources(snapshot), "Unsafe snapshots should not be worker-ready.");
		});
	}

}
