#include "../TestHarness.h"
#include "EditorAutomationService.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "Tests/Support/DecodedCaptureAssertions.h"

#include <limits>

namespace TerraEngine::EditorTests
{
	nlohmann::json CanonicalPosition(double x, double y, double z)
	{
		return {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {x, y, z}}};
	}
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		// Keep the executor alive until renderer cleanup has settled its owned
		// image jobs. A pre-existing executor remains owned by its original fixture.
		class CameraSafetySession final
		{
		public:
			CameraSafetySession() : ownsExecutor_(ThreadPool::instance().getThreadCount() == 0U)
			{
				RenderSystem::Instance().Clear();
				if(ownsExecutor_) ThreadPool::instance().initialize(2U);
			}
			~CameraSafetySession()
			{
				RenderSystem::Instance().Clear();
				if(ownsExecutor_) ThreadPool::instance().shutdown();
				ObjectManager::instance().clear();
			}
		private:
			bool ownsExecutor_;
		};

		void ExpectDecodedSafetyImage(TestContext& context, const nlohmann::json& result)
		{
			context.Expect(result.value("ok", false), "A healthy read-only capture must return success.");
			context.Expect(result.contains("mcpContent") && result["mcpContent"].is_array()
				&& result["mcpContent"].size() == 1U, "A healthy capture must publish one actual MCP image.");
			const auto& image = result["mcpContent"][0];
			context.ExpectEqual(std::string("image/png"), image.value("mimeType", std::string()), "Safety checks decode the actual PNG item.");
			const auto decoded = TerraEngine::TestSupport::DecodeCaptureBase64(image.value("data", std::string()));
			context.ExpectEqual(64U, decoded.width, "Read-only capture bytes must retain their requested width.");
			context.ExpectEqual(64U, decoded.height, "Read-only capture bytes must retain their requested height.");
			context.ExpectEqual(static_cast<std::size_t>(64U * 64U * 4U), decoded.rgba.size(), "Decoded image storage must cover the requested extent.");
		}
	}

	void RegisterEditorMcpCameraImageSafetyTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.CameraImages.CustomCaptureDoesNotMutateSceneObjects", [](TestContext& context) {
			CameraSafetySession captureSession;
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
			ExpectDecodedSafetyImage(context, result);
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.DisabledCameraCaptureDoesNotEnableSource", [](TestContext& context) {
			CameraSafetySession captureSession;
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
			ExpectDecodedSafetyImage(context, result);
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.RejectedCapturePublishesNoImageContent", [](TestContext& context) {
			CameraSafetySession captureSession;
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
		registry.Add("Editor.MCP.CameraImages.FailedCaptureHasNoImageThenHealthyCaptureDecodes", [](TestContext& context) {
			CameraSafetySession captureSession;
			EditorAutomationService service;
			// Reject arithmetic overflow without allocating a giant image. A failed
			// request must leave capacity for the next ordinary, independently decoded capture.
			const nlohmann::json failed = service.HandleTool(
				"camera.capture_custom",
				{{"camera", {{"position", CanonicalPosition(0.0, 0.0, 5.0)}}},
				 {"width", std::numeric_limits<std::uint32_t>::max()},
				 {"height", std::numeric_limits<std::uint32_t>::max()}},
				WriteMode::ReadOnly, "safety-size-overflow");
			context.Expect(!failed.value("ok", true), "Overflowing image storage must be rejected explicitly.");
			context.Expect(!failed.contains("mcpContent"), "Failed image preparation must not expose MCP content.");
			const nlohmann::json healthy = service.HandleTool(
				"camera.capture_custom",
				{{"camera", {{"position", CanonicalPosition(0.0, 0.0, 5.0)}}}, {"width", 64U}, {"height", 64U}},
				WriteMode::ReadOnly, "safety-after-size-overflow");
			ExpectDecodedSafetyImage(context, healthy);
		});

	}
}
