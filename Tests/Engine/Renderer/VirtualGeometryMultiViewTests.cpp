#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryMultiViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Render.MultiView.KeepsSelectionStateIsolatedPerView", [](TestContext& context) {
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();

			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 501u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.draws.push_back({ objectId, meshId, { 0u, 0u, 16u, true, false, {}, true }, true });

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 1u;
			sceneState.targetSize = glm::uvec2(1280u, 720u);
			sceneState.draws.push_back({ objectId, meshId, { 2u, 24u, 4u, true, false, {}, true }, true });

			snapshot.virtualGeometryViewSelections = { gameState, sceneState };
			const std::vector<TerraEngine::Renderer::VirtualGeometryViewSelectionState> states =
				TerraEngine::Renderer::BuildVirtualGeometryViewSelectionStates(snapshot);

			const auto gameIt = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != states.end(), "Game view virtual geometry state should be present.");
			context.Expect(sceneIt != states.end(), "Scene view virtual geometry state should be present.");
			context.ExpectEqual(16u, gameIt->draws.front().range.meshletCount, "Game view should keep its own selected meshlet range.");
			context.ExpectEqual(4u, sceneIt->draws.front().range.meshletCount, "Scene view should keep its own selected meshlet range.");
			context.Expect(gameIt->draws.front().range.meshletOffset != sceneIt->draws.front().range.meshletOffset, "Different views should not share one mutable LOD range.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.BuildsExportRuntimeDiagnosticsSeparately", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 777u;
			snapshot.viewRequests.push_back({ RenderOutputKind::MainRuntimeView, RenderViewType::Game, glm::uvec2(1280u, 720u), 9u });
			snapshot.renderables.push_back({
				TerraEngine::GenerateUUID(),
				TerraEngine::GenerateUUID(),
				TerraEngine::GenerateUUID(),
				glm::mat4(1.0f),
				{ 1u, 12u, 6u, true, false, {}, true },
				true,
				false
			});

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.hiZOcclusionEnabled = true;
			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					quality,
					VirtualGeometryInactiveReason::None);

			context.ExpectEqual(static_cast<std::size_t>(1u), diagnostics.size(), "One runtime output should create one virtual geometry diagnostics snapshot.");
			context.Expect(diagnostics.front().viewType == RenderViewType::ExportRuntime, "Main runtime output should be reported separately from editor Game View.");
			context.ExpectEqual(6u, diagnostics.front().selectedMeshlets, "Export runtime diagnostics should use the runtime view's selected range.");
			context.Expect(!diagnostics.front().warnings.empty(), "Export runtime diagnostics should explain that the state is isolated from editor views.");
		});
	}
}
