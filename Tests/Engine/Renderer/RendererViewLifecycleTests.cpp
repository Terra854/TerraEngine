#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

namespace TerraEngine::Tests
{
	void RegisterRendererViewLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ViewLifecycle.HandlesEmptyAndZeroSizedViews", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderFrameSnapshot empty;
			TerraEngine::Renderer::SortSnapshotDeterministically(empty);
			context.Expect(empty.viewRequests.empty(), "A frame with no active view must remain an explicit empty snapshot.");
			TerraEngine::Renderer::RenderFrameSnapshot zero;
			zero.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(0u), 1u });
			TerraEngine::Renderer::SortSnapshotDeterministically(zero);
			context.ExpectEqual(0u, zero.viewRequests.front().targetSize.x, "A zero-sized target must not inherit a prior view extent.");
		});

		registry.Add("Renderer.ViewLifecycle.MultipleViewsRemainDeterministicAcrossRecovery", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 9u;
			snapshot.sceneRevision = 3u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1920u, 1080u), 2u });
			snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(1280u, 720u), 1u });
			TerraEngine::Renderer::SortSnapshotDeterministically(snapshot);
			context.ExpectEqual(static_cast<std::size_t>(2), snapshot.viewRequests.size(), "Scene and Game views must remain separate during recovery and replay.");
			context.Expect(snapshot.viewRequests[0].outputId != snapshot.viewRequests[1].outputId, "Each view must retain an independent output identity.");
		});
	}
}
