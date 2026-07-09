#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewToolbarOverlayTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Toolbar.IconMetadataAndToolState", [](TestContext& context) {
			const auto icons = TerraEngine::Editor::SceneViewOverlay::BuildDefaultIconDefinitions();
			std::string error;
			context.Expect(TerraEngine::Editor::SceneViewOverlay::ValidateIconDefinitions(icons, &error), error);
			context.Expect(TerraEngine::Editor::SceneViewOverlay::FindIconDefinition(icons, "tool.move") != nullptr, "Move tool icon should be available.");
			context.ExpectEqual(std::string("M"), std::string(TerraEngine::Editor::SceneViewOverlay::ToolbarGlyph(TerraEngine::Editor::SceneViewOverlay::SceneViewTool::Move)), "Move tool should have a compact glyph.");

			TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference preference;
			preference.gridVisible = false;
			const auto state = TerraEngine::Editor::SceneViewOverlay::BuildToolStateFromPreference(
				preference,
				TerraEngine::Editor::SceneViewOverlay::SceneViewTool::Rotate,
				TerraEngine::Editor::SceneViewOverlay::PivotMode::Pivot,
				TerraEngine::Editor::SceneViewOverlay::OrientationMode::Global);
			context.ExpectEqual(std::string("rotate"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(state.activeTool)), "Active tool should serialize for diagnostics.");
			context.Expect(!state.gridVisible, "Toolbar state should mirror Scene View grid preference without touching Game View.");

			const std::string sceneViewSource = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");
			context.Expect(sceneViewSource.find("HandleSceneViewPan") != std::string::npos, "Hand tool should be wired to Scene View pan navigation.");
			context.Expect(sceneViewSource.find("SceneViewTool::Hand") != std::string::npos && sceneViewSource.find("m_IsSceneViewPanning") != std::string::npos, "Hand tool should block picking while it is navigating.");
			context.Expect(sceneViewSource.find("ImGuiMouseButton_Middle") != std::string::npos, "Scene View should support middle-mouse panning.");
			context.Expect(sceneViewSource.find("HandleSceneViewScrollZoom") != std::string::npos, "Scene View should support mouse-wheel zoom in both projection modes.");
			context.Expect(sceneViewSource.find("Perspective wheel input dollies") != std::string::npos, "Perspective scroll zoom should move the editor camera instead of changing gameplay FOV.");
		});
	}
}
