// WorkspaceLayoutSectionRestoreTests.cpp -- Independent visibility/camera restore.
#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Core/WorkspaceLayoutDecode.h"
#include "Editor/TerraEngine-Editor/Framework/EditorWindow.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewCameraState.h"

namespace TerraEngine::EditorTests
{
	namespace SceneView = TerraEngine::Editor::SceneView;
	namespace SceneViewOverlay = TerraEngine::Editor::SceneViewOverlay;
	namespace
	{
		class CurrentWindow final : public EditorWindow
		{
		public:
			CurrentWindow() : EditorWindow("native-window", false) {}
		protected:
			void OnGui() override {}
		};
		struct WorkspaceFixture
		{
			CurrentWindow window;
			bool managedVisible = false;
			SceneView::CameraWorkspaceState camera;
			int inventoryWarnings = 0, entryWarnings = 0, cameraWarnings = 0;
			void Apply(const nlohmann::json& layout)
			{
				// This is the same section dispatcher called by EditorSystem. Actual
				// window state and the production camera decoder supply its destinations.
				Editor::ApplyWorkspaceLayoutSections(layout,
					[&](const Editor::WorkspaceWindowVisibility& value) {
						if(value.managed && value.title == "managed-window") managedVisible = value.visible;
						else if(!value.managed && value.title == window.GetTitle()) window.SetVisible(value.visible);
					},
					[&](const nlohmann::json& value) {
						std::string error;
						if(const auto decoded = SceneView::DeserializeCameraWorkspaceState(value, &error)) camera = *decoded;
						else ++cameraWarnings;
					},
					[&](Editor::WorkspaceLayoutIssue issue) {
						if(issue == Editor::WorkspaceLayoutIssue::InvalidWindowInventory) ++inventoryWarnings;
						else ++entryWarnings;
					});
			}
		};
		nlohmann::json DesiredCamera()
		{
			SceneView::CameraWorkspaceState camera;
			camera.position = {{4, -2, 7}, {12.0, 3.0, 4.0}};
			camera.rotationRadians = {0.2F, 0.4F, 0.1F};
			camera.fieldOfViewDegrees = 73.0F;
			camera.focusDistance = 42.0F;
			camera.orthographicHeight = 15.0F;
			camera.projectionMode = SceneViewOverlay::ProjectionMode::Orthographic;
			return SceneView::SerializeCameraWorkspaceState(camera);
		}
	}

	void RegisterWorkspaceLayoutSectionRestoreTests(TestRegistry& registry)
	{
		registry.Add("Editor.WorkspaceSections.MissingInventoryStillRestoresCamera", [](TestContext& context) {
			WorkspaceFixture fixture;
			const auto desired = DesiredCamera();
			fixture.Apply({{"sceneViewCamera", desired}});
			context.Expect(SceneView::SerializeCameraWorkspaceState(fixture.camera) == desired,
				"A missing windows section must still restore the exact production-decoded camera state.");
			context.Expect(!fixture.window.IsVisible() && !fixture.managedVisible && fixture.inventoryWarnings == 1,
				"Missing visibility inventory must leave current windows unchanged and report its warning.");
		});
		registry.Add("Editor.WorkspaceSections.InvalidInventoryStillRestoresCamera", [](TestContext& context) {
			const auto desired = DesiredCamera();
			for(const auto& windows : {nlohmann::json(nullptr), nlohmann::json(42),
				nlohmann::json("wrong"), nlohmann::json::object()})
			{
				WorkspaceFixture fixture;
				fixture.Apply({{"windows", windows}, {"sceneViewCamera", desired}});
				context.Expect(SceneView::SerializeCameraWorkspaceState(fixture.camera) == desired,
					"Every malformed visibility inventory must preserve independent valid camera restoration.");
				context.Expect(!fixture.window.IsVisible() && !fixture.managedVisible && fixture.inventoryWarnings == 1,
					"Invalid inventory must not change current native or managed visibility.");
			}
		});
		registry.Add("Editor.WorkspaceSections.ValidVisibilityAndCameraRestoreTogether", [](TestContext& context) {
			WorkspaceFixture fixture;
			const auto desired = DesiredCamera();
			fixture.Apply({{"windows", nlohmann::json::array({nullptr,
				{{"title", "native-window"}, {"visible", true}},
				{{"title", "managed-window"}, {"visible", true}, {"managed", true}}})},
				{"sceneViewCamera", desired}});
			context.Expect(fixture.window.IsVisible() && fixture.managedVisible && fixture.entryWarnings == 1,
				"Valid native/managed visibility entries must still apply after an invalid neighboring entry.");
			context.Expect(SceneView::SerializeCameraWorkspaceState(fixture.camera) == desired && fixture.cameraWarnings == 0,
				"Valid visibility must remain compatible with complete camera restoration.");
		});
		registry.Add("Editor.WorkspaceSections.InvalidRootAndCameraPreserveCurrentState", [](TestContext& context) {
			WorkspaceFixture fixture;
			const auto initial = SceneView::SerializeCameraWorkspaceState(fixture.camera);
			for(const auto& malformed : {nlohmann::json(nullptr), nlohmann::json(7), nlohmann::json::array()})
				fixture.Apply(malformed);
			context.Expect(!fixture.window.IsVisible() && !fixture.managedVisible
				&& SceneView::SerializeCameraWorkspaceState(fixture.camera) == initial,
				"Malformed overall input must leave current window and camera state untouched.");
			fixture.Apply({{"windows", nlohmann::json::array({{{"title", "native-window"}, {"visible", true}}})},
				{"sceneViewCamera", "bad camera"}});
			context.Expect(fixture.window.IsVisible() && fixture.cameraWarnings == 1
				&& SceneView::SerializeCameraWorkspaceState(fixture.camera) == initial,
				"A rejected camera must retain its current state while an independent valid window change still applies.");
		});
	}
}
