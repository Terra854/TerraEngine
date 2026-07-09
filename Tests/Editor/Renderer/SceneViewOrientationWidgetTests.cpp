#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewOrientationWidgetTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Orientation.AxisHitAndProjectionLabel", [](TestContext& context) {
			const auto state = TerraEngine::Editor::SceneViewOverlay::BuildOrientationWidgetState(
				glm::vec3(0.0f, 0.0f, -1.0f),
				glm::vec3(0.0f, 1.0f, 0.0f),
				TerraEngine::Editor::SceneViewOverlay::ProjectionMode::Orthographic,
				glm::vec4(10.0f, 10.0f, 110.0f, 110.0f));
			context.ExpectEqual(std::string("Ortho"), state.label, "Projection label should reflect orthographic mode.");
			const auto axis = TerraEngine::Editor::SceneViewOverlay::HitTestOrientationWidget(state, glm::vec2(100.0f, 60.0f));
			context.ExpectEqual(std::string("x"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(axis)), "Right-side hit should target positive X.");
			const auto zAxis = TerraEngine::Editor::SceneViewOverlay::HitTestOrientationWidget(state, glm::vec2(38.0f, 82.0f));
			context.ExpectEqual(std::string("z"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(zAxis)), "Lower-left hit should target positive Z.");
			const auto backAxis = TerraEngine::Editor::SceneViewOverlay::HitTestOrientationWidget(state, glm::vec2(82.0f, 38.0f));
			context.ExpectEqual(std::string("-z"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(backAxis)), "Upper-right hit should target negative Z.");
			const auto forgivingHit = TerraEngine::Editor::SceneViewOverlay::HitTestOrientationWidget(state, glm::vec2(68.0f, 72.0f));
			context.Expect(std::string(TerraEngine::Editor::SceneViewOverlay::ToString(forgivingHit)) != "none", "Orientation widget should snap from forgiving in-box clicks.");
			context.ExpectEqual(std::string("Top"), TerraEngine::Editor::SceneViewOverlay::SnapEulerForAxis(TerraEngine::Editor::SceneViewOverlay::OrientationAxis::PositiveY).label, "Top axis should expose a camera snap.");

			const std::string sceneViewSource = ReadSceneViewOverlaySourceForTest("Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");
			context.Expect(sceneViewSource.find("ImGuizmo::ViewManipulate") != std::string::npos, "Scene View should use the bundled ImGuizmo view manipulator.");
			context.Expect(sceneViewSource.find("ImGuizmo::IsUsingViewManipulate") != std::string::npos, "Scene View should observe active view manipulator drags and click interpolation.");
			context.Expect(sceneViewSource.find("ImGuizmo::IsViewManipulateHovered") != std::string::npos, "Scene View should route hover ownership through the ImGuizmo view manipulator.");
			context.Expect(sceneViewSource.find("RepositionCameraAroundFocus") != std::string::npos, "Scene View widget snaps should orbit around a stable focus.");
			context.Expect(sceneViewSource.find("CameraEulerFromForward") != std::string::npos, "Scene View should convert manipulated view matrices back into its camera state.");
			context.Expect(sceneViewSource.find("IsPointerInsideOrientationWidget") != std::string::npos, "Scene View should reserve the widget rectangle before camera movement and picking.");
		});
	}
}
