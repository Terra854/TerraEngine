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
			// Frame submission owns the visibility decision after the Vulkan implementation was split into focused source files.
			const std::string vulkanFrameSource = ReadSceneViewOverlaySourceForTest("Engine/Renderer/RHI/Vulkan/VulkanFrameRendering.cpp");
			const std::string vulkanBuiltinPassesSource = ReadSceneViewOverlaySourceForTest("Engine/Renderer/RHI/Vulkan/VulkanBuiltinPasses.cpp");
			context.Expect(vulkanFrameSource.find("view == ViewType::Scene && presentation.sceneViewGridVisible") != std::string::npos, "Vulkan grid pass should respect the sealed Scene View grid visibility.");
			context.Expect(vulkanBuiltinPassesSource.find("orthographicProjection ? 1.0f") != std::string::npos, "Orthographic Scene View sky should not stretch from ortho zoom.");
			context.Expect(vulkanBuiltinPassesSource.find("presentation.sceneViewOrthographic") != std::string::npos, "Scene View orthographic mode should reach Vulkan through the immutable presentation input.");
			context.Expect(vulkanBuiltinPassesSource.find("skyFlags |= 4u") != std::string::npos, "Scene View orthographic mode should select the editor background shader path.");
			const std::string skyShaderSource = ReadSceneViewOverlaySourceForTest("Engine/Shaders/Sky/SkyBackground.frag.hlsl");
			context.Expect(skyShaderSource.find("BuildSceneViewOrthographicBackground") != std::string::npos, "Scene View orthographic mode should use a dedicated editor gradient.");
			const std::string gridShaderSource = ReadSceneViewOverlaySourceForTest("Editor/Shaders/SceneViewGrid.hlsl");
			context.Expect(gridShaderSource.find("BuildOrthographicGridColor") != std::string::npos, "Orthographic Scene View should use a dedicated grid color path.");
			context.Expect(gridShaderSource.find("ChooseOrthographicGridSpacing") != std::string::npos, "Orthographic Scene View grid should choose spacing from zoom/pixel scale.");
			context.Expect(gridShaderSource.find("TryResolveGridWorldPosition") != std::string::npos, "Orthographic Scene View grid should resolve ground intersections without near-plane cutoffs.");
			context.Expect(gridShaderSource.find("!IsOrthographicGrid() && t < 0.0") != std::string::npos, "Only perspective grid rays should be discarded when the ground is behind the near ray.");
			context.Expect(gridShaderSource.find("const float kGridPlaneY = -0.02") != std::string::npos,
				"The construction grid should sit below authored ground geometry so flat terrain does not z-fight through it.");
			context.Expect(gridShaderSource.find("projection scale, not a movement through space") != std::string::npos, "Orthographic grid should not fade out using camera distance.");
			context.Expect(gridShaderSource.find("ChoosePerspectiveGridSpacing()") != std::string::npos,
				"Perspective grid spacing should be selected once from camera height instead of discontinuous per-fragment derivatives.");
			const std::size_t perspectiveGridStart = gridShaderSource.find("float4 BuildPerspectiveGridColor");
			const std::size_t orthographicGridStart = gridShaderSource.find("float4 BuildOrthographicGridColor");
			const std::size_t derivativeSpacingCall = gridShaderSource.find(
				"float spacing = ChooseOrthographicGridSpacing(coord);", perspectiveGridStart);
			context.Expect(perspectiveGridStart != std::string::npos
				&& orthographicGridStart != std::string::npos
				&& (derivativeSpacingCall == std::string::npos || derivativeSpacingCall >= orthographicGridStart),
				"Perspective grid lines must not switch spacing across neighbouring fragments.");
			const std::string gridPassSource = ReadSceneViewOverlaySourceForTest("Engine/Renderer/RHI/Vulkan/VulkanBuiltinPasses.cpp");
			context.Expect(gridPassSource.find("LocalizeRenderPosition") != std::string::npos,
				"The Scene View grid should localize canonical world origin into the active render reference frame.");
			context.Expect(gridPassSource.find("presentation.viewMatrix * gridCoordinatesToViewFrame") != std::string::npos,
				"The grid view matrix should anchor shader y=0 to canonical world y=0 instead of the camera plane.");
			const std::string sceneViewSource = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");
			context.Expect(sceneViewSource.find("mesh->hasLocalBounds") != std::string::npos,
				"Framing a selected renderable should use mesh bounds rather than only its transform pivot.");
			context.Expect(sceneViewSource.find("radius / std::sin(limitingHalfAngle)") != std::string::npos,
				"Bounds-aware framing should fit the selected object inside both viewport axes.");
			const std::string inspectorSource = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp");
			context.Expect(inspectorSource.find("(Generated material)") != std::string::npos,
				"The Inspector should distinguish a live generated terrain material from a missing asset.");
			context.Expect(inspectorSource.find("Canonical Transform") == std::string::npos
				&& inspectorSource.find("Cell X") == std::string::npos
				&& inspectorSource.find("Parent Relative") == std::string::npos,
				"Canonical cell storage must remain hidden behind the conventional Transform authoring surface.");
			context.Expect(inspectorSource.find("beginRow(\"Position\")") != std::string::npos
				&& inspectorSource.find("beginRow(\"Rotation\")") != std::string::npos
				&& inspectorSource.find("beginRow(\"Scale\")") != std::string::npos,
				"The Inspector should expose the established Position, Rotation, and Scale rows.");
			context.Expect(inspectorSource.find("ImGui::TextUnformatted(\"Terrain instance\")") == std::string::npos
				&& inspectorSource.find("Edit shared terrain asset") == std::string::npos
				&& inspectorSource.find("Create independent copy") == std::string::npos,
				"The Terrain Inspector should not expose redundant implementation prose or model an action as a persistent radio choice.");
			context.Expect(inspectorSource.find("ImGui::TextUnformatted(\"Terrain Asset\")") != std::string::npos
				&& inspectorSource.find("ImGui::Button(\"Edit Terrain\"") != std::string::npos
				&& inspectorSource.find("OpenTerrainForEditing(objectId, openError)") != std::string::npos
				&& inspectorSource.find("ImGui::Button(\"Make Independent\"") != std::string::npos,
				"The Terrain Inspector should use compact author-facing fields, bind the selected terrain, and expose full-width actions.");
			const std::string terrainWindowSource = ReadSceneViewOverlaySourceForTest(
				"Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp");
			context.Expect(terrainWindowSource.find("CreateRegisteredTerrainService()") != std::string::npos
				&& terrainWindowSource.find("assets.ImportAsset(absolutePath)") != std::string::npos
				&& terrainWindowSource.find("GetRegisteredAssetPath") != std::string::npos
				&& terrainWindowSource.find("GetAssetRoot().parent_path() / destination") != std::string::npos,
				"New terrain publication must make the descriptor visible to both the editor asset database and runtime resource lookup.");
		});
	}
}
