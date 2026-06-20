#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <algorithm>

namespace TerraEngine::Tests {
namespace {
	TerraEngine::Renderer::RenderFrameSnapshot MakeSnapshot()
	{
		TerraEngine::Renderer::RenderFrameSnapshot snapshot;
		snapshot.frameId = 7u;
		snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1920u, 1080u), 2u });
		snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(1280u, 720u), 1u });
		snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
		snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
		snapshot.meshes.push_back({ TerraEngine::GenerateUUID(), "mesh", true, false });
		snapshot.materials.push_back({ TerraEngine::GenerateUUID(), "material", true, false });
		snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "texture", false, true });
		return snapshot;
	}
}

	void RegisterRenderFrameSnapshotTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Snapshot.SortingIsDeterministic", [](TestContext& context) {
			auto left = MakeSnapshot();
			auto right = left;
			std::reverse(right.viewRequests.begin(), right.viewRequests.end());
			std::reverse(right.renderables.begin(), right.renderables.end());
			TerraEngine::Renderer::SortSnapshotDeterministically(left);
			TerraEngine::Renderer::SortSnapshotDeterministically(right);
			context.ExpectEqual(left.viewRequests.front().outputId, right.viewRequests.front().outputId, "View request order should be deterministic.");
			context.ExpectEqual(TerraEngine::ToHexString(left.renderables.front().objectId), TerraEngine::ToHexString(right.renderables.front().objectId), "Renderable order should be deterministic.");
		});

		registry.Add("Renderer.Snapshot.FallbackResourcesAreResolved", [](TestContext& context) {
			auto snapshot = MakeSnapshot();
			context.Expect(TerraEngine::Renderer::SnapshotUsesOnlyResolvedResources(snapshot), "Fallback textures should be valid worker inputs.");
			snapshot.meshes.front().resident = false;
			snapshot.meshes.front().usingFallback = false;
			context.Expect(!TerraEngine::Renderer::SnapshotUsesOnlyResolvedResources(snapshot), "Missing resources without fallbacks must be rejected.");
		});

		registry.Add("Renderer.Snapshot.CarriesSceneAndResourceRevisions", [](TestContext& context) {
			auto snapshot = MakeSnapshot();
			snapshot.sceneRevision = 101u;
			snapshot.resourceRevision = 202u;
			context.ExpectEqual(static_cast<std::uint64_t>(101u), snapshot.sceneRevision, "Snapshots should carry scene revision tokens.");
			context.ExpectEqual(static_cast<std::uint64_t>(202u), snapshot.resourceRevision, "Snapshots should carry resource revision tokens.");
		});
	}

}
