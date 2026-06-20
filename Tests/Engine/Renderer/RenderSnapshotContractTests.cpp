#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderSnapshotContractTests(TestRegistry& registry)
	{
		registry.Add("Renderer.SnapshotContract.RejectsLiveMutableState", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(1u, 4u);
			snapshot.containsLiveMutableState = true;
			context.Expect(TerraEngine::Renderer::SnapshotContainsLiveMutableState(snapshot), "Snapshot validation should expose live mutable state leaks.");
			context.Expect(!TerraEngine::Renderer::SnapshotUsesOnlyResolvedResources(snapshot), "Worker snapshots with live mutable state should not be considered resolved.");
		});

		registry.Add("Renderer.SnapshotContract.HashIsDeterministic", [](TestContext& context) {
			auto left = MakeRendererThreadingSnapshot(2u, 8u, 2u);
			auto right = left;
			context.ExpectEqual(TerraEngine::Renderer::BuildSnapshotDeterministicHash(left), TerraEngine::Renderer::BuildSnapshotDeterministicHash(right), "Equal immutable snapshots should hash identically.");
		});
	}

}
