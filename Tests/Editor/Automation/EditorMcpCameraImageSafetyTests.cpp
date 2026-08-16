#include "../TestHarness.h"
#include "EditorAutomationService.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Core/Object/ObjectManager.h"

namespace TerraEngine::EditorTests
{
	nlohmann::json CanonicalPosition(double x, double y, double z)
	{
		return {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {x, y, z}}};
	}
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	void RegisterEditorMcpCameraImageSafetyTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.CameraImages.CustomCaptureDoesNotMutateSceneObjects", [](TestContext& context) {
			ObjectManager::instance().clear();
			EditorAutomationService service;
			Object existing = ObjectManager::instance().createObject("Stable Object");
			const TerraEngine::UUID stableId = ObjectManager::instance().getUUID(existing);
			const std::size_t rootCountBefore = ObjectManager::instance().getRootObjects().size();

			const nlohmann::json result = service.HandleTool(
				"camera.capture_custom",
				{{"camera", {{"position", CanonicalPosition(0.0, 2.0, 5.0)}, {"lookAt", CanonicalPosition(0.0, 0.0, 0.0)}}}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"safety-custom");

			context.Expect(result.value("ok", false), "Custom camera capture should succeed in read-only mode.");
			context.ExpectEqual(rootCountBefore, ObjectManager::instance().getRootObjects().size(), "Custom camera capture should not create scene objects.");
			context.Expect(ObjectManager::instance().getObject(stableId).has_value(), "Custom camera capture should not remove existing objects.");
			context.Expect(ObjectManager::instance().isObjectActive(existing), "Custom camera capture should not change active state.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.DisabledCameraCaptureDoesNotEnableSource", [](TestContext& context) {
			ObjectManager::instance().clear();
			EditorAutomationService service;
			Object cameraObject = ObjectManager::instance().createObject("Safety Disabled Camera");
			CameraComponent* camera = ObjectManager::instance().AddComponent<CameraComponent>(cameraObject);
			context.Expect(camera != nullptr, "Fixture should add camera component.");
			camera->active = false;
			ObjectManager::instance().setObjectActive(cameraObject, false);

			const nlohmann::json result = service.HandleTool(
				"camera.capture_object",
				{{"object", TerraEngine::ToHexString(ObjectManager::instance().getUUID(cameraObject))}, {"includeDisabled", true}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"safety-disabled-camera");

			context.Expect(result.value("ok", false), "Disabled camera capture should succeed when includeDisabled is true.");
			context.Expect(!ObjectManager::instance().isObjectActive(cameraObject), "Disabled camera capture should not activate the object.");
			context.Expect(!camera->active, "Disabled camera capture should not enable the CameraComponent.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.RejectedCapturePublishesNoImageContent", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"format", "image/jpeg"}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"safety-unsupported-format");

			context.Expect(!result.value("ok", true), "Unsupported capture formats should be rejected.");
			context.ExpectEqual(std::string("unsupported_readback"), result["error"].value("category", std::string()), "Unsupported format should use unsupported-readback category.");
			context.Expect(!result.contains("mcpContent"), "Rejected capture should not publish MCP image content.");
		});
	}
}
