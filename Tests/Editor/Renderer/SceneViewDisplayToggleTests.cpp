#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewDisplayToggleTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.DisplayToggles.DoNotAffectGameViewDiagnostics", [](TestContext& context) {
			TerraEngine::Editor::SceneViewOverlay::SceneViewDiagnosticsSnapshot snapshot;
			snapshot.toolState.iconsVisible = false;
			snapshot.toolState.gridVisible = false;
			snapshot.gameViewAffected = false;
			const auto json = TerraEngine::Editor::SceneViewOverlay::SerializeDiagnosticsSnapshot(snapshot);
			context.Expect(!json.value("gameViewAffected", true), "Scene View overlay toggles must report Game View independence.");

			const std::string gameViewSource = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp");
			context.Expect(gameViewSource.find("SceneViewOverlay") == std::string::npos, "Game View should not include Scene View overlay state.");
			const std::string renderSystemHeader = ReadSceneViewOverlaySourceForTest("Engine/Renderer/RenderSystem.h");
			context.Expect(renderSystemHeader.find("SetSceneViewGridVisible") != std::string::npos, "Scene View grid toggle should reach the renderer.");
			const std::string vulkanSource = ReadSceneViewOverlaySourceForTest("Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			context.Expect(vulkanSource.find("view == ViewType::Scene && presentation.sceneViewGridVisible") != std::string::npos, "Vulkan grid pass should respect the sealed Scene View grid visibility.");
			context.Expect(vulkanSource.find("orthographicProjection ? 1.0f") != std::string::npos, "Orthographic Scene View sky should not stretch from ortho zoom.");
			context.Expect(vulkanSource.find("presentation.sceneViewOrthographic") != std::string::npos, "Scene View orthographic mode should reach Vulkan through the immutable presentation input.");
			context.Expect(vulkanSource.find("skyFlags |= 4u") != std::string::npos, "Scene View orthographic mode should select the editor background shader path.");
			const std::string skyShaderSource = ReadSceneViewOverlaySourceForTest("Engine/Shaders/Sky/SkyBackground.frag.hlsl");
			context.Expect(skyShaderSource.find("BuildSceneViewOrthographicBackground") != std::string::npos, "Scene View orthographic mode should use a dedicated editor gradient.");
			const std::string gridShaderSource = ReadSceneViewOverlaySourceForTest("Editor/Shaders/SceneViewGrid.hlsl");
			context.Expect(gridShaderSource.find("BuildOrthographicGridColor") != std::string::npos, "Orthographic Scene View should use a dedicated grid color path.");
			context.Expect(gridShaderSource.find("ChooseOrthographicGridSpacing") != std::string::npos, "Orthographic Scene View grid should choose spacing from zoom/pixel scale.");
			context.Expect(gridShaderSource.find("TryResolveGridWorldPosition") != std::string::npos, "Orthographic Scene View grid should resolve ground intersections without near-plane cutoffs.");
			context.Expect(gridShaderSource.find("!IsOrthographicGrid() && t < 0.0") != std::string::npos, "Only perspective grid rays should be discarded when the ground is behind the near ray.");
			context.Expect(gridShaderSource.find("projection scale, not a movement through space") != std::string::npos, "Orthographic grid should not fade out using camera distance.");
		});
	}
}
