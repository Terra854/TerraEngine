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
			context.Expect(TerraEngine::Editor::SceneViewOverlay::FindIconDefinition(icons, "toolbar.projection") != nullptr, "Projection mode should have icon metadata.");
			context.ExpectEqual(std::string("axis-arrows"), std::string(TerraEngine::Editor::SceneViewOverlay::ToolbarIconShape(TerraEngine::Editor::SceneViewOverlay::SceneViewTool::Move)), "Move tool should render as an icon instead of a text button.");

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
			context.Expect(sceneViewSource.find("sceneViewAcceptsViewportInput") != std::string::npos, "Scene View scroll zoom should require topmost hover ownership before consuming wheel input.");
			context.Expect(sceneViewSource.find("viewportOwnsPointer && !orientationWidgetBusy") != std::string::npos, "Scene View should not zoom from raw viewport coordinates when another editor window covers it.");
			context.Expect(sceneViewSource.find("DrawOverlayIconButton") != std::string::npos, "Scene View toolbar should use icon-only buttons.");
			context.Expect(sceneViewSource.find("DrawOverlayToggleButton") == std::string::npos, "Scene View toolbar should not draw text-first debug buttons.");
		});

		registry.Add("SceneViewOverlay.Toolbar.ProjectionRoundTripRestoresPerspectivePose", [](TestContext& context) {
			const std::string sceneViewSource = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");
			const std::string sceneViewHeader = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/SceneViewWindow.h");

			context.Expect(sceneViewHeader.find("struct PerspectiveCameraState") != std::string::npos, "Scene View should retain a dedicated perspective camera snapshot while orthographic mode is active.");
			context.Expect(sceneViewSource.find("m_PerspectiveCameraState.position = m_CameraPos;") != std::string::npos, "Entering orthographic mode should save the exact perspective position.");
			context.Expect(sceneViewSource.find("m_PerspectiveCameraState.rotation = m_CameraRot;") != std::string::npos, "Entering orthographic mode should save the exact perspective look direction.");
			context.Expect(sceneViewSource.find("m_PerspectiveCameraState.focus = m_CameraFocus;") != std::string::npos, "Entering orthographic mode should save the perspective orbit pivot.");
			context.Expect(sceneViewSource.find("m_CameraPos = m_PerspectiveCameraState.position;") != std::string::npos, "Returning to perspective should restore position without reconstructing it from orthographic state.");
			context.Expect(sceneViewSource.find("m_CameraRot = m_PerspectiveCameraState.rotation;") != std::string::npos, "Returning to perspective should restore orientation without normalization drift.");
			context.Expect(sceneViewSource.find("SetSceneViewProjectionMode(projectionMode);") != std::string::npos, "Automation projection changes should use the same camera-preserving transition as the toolbar.");
			context.Expect(sceneViewSource.find("SetSceneViewProjectionMode(switchingToOrthographic") != std::string::npos, "The visible projection button should use the shared camera-preserving transition.");
		});
	}
}
