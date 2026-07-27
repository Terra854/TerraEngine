#include "../TestHarness.h"
#include "EditorAutomationService.h"
#include "EditorMcpBridge.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <limits>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::EditorMcpBridge;
	using TerraEngine::Editor::Automation::MutationMode;
	using TerraEngine::Editor::Automation::AutomationDispatchResult;
	using TerraEngine::Editor::Automation::AutomationPermission;
	using TerraEngine::Editor::Automation::AutomationRequestContext;
	using TerraEngine::Editor::Automation::ErrorCategory;
	using TerraEngine::Editor::Automation::WriteMode;

	void RegisterEditorMcpCameraImageContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.CameraImages.AvailabilityResourceHasContractShape", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			const nlohmann::json availability = bridge.BuildCaptureAvailabilityJson();

			context.Expect(availability.contains("available"), "Capture availability should expose a top-level availability flag.");
			context.Expect(availability.contains("state"), "Capture availability should expose a state string.");
			context.Expect(availability["limits"].value("defaultWidth", 0u) > 0u, "Capture limits should include a positive default width.");
			context.Expect(availability["limits"].value("defaultHeight", 0u) > 0u, "Capture limits should include a positive default height.");
			context.Expect(!availability["limits"].contains("maxWidth"), "Capture limits should omit a fixed maximum width.");
			context.Expect(!availability["limits"].contains("maxHeight"), "Capture limits should omit a fixed maximum height.");
			context.Expect(availability["sources"].value("gameView", false), "Capture availability should advertise Game View capture.");
			context.Expect(availability["sources"].value("disabledCameraObject", false), "Capture availability should advertise disabled camera-object capture.");

			EditorAutomationService service;
			const nlohmann::json largeCapture = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"width", 4096u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"availability-large-resolution");
			context.Expect(largeCapture.value("ok", false), "Large captures should not be rejected by a fixed maximum resolution policy.");
			context.ExpectEqual(4096u, largeCapture["data"].value("width", 0u), "Large capture result should preserve the requested width.");

			RenderSystem& renderer = RenderSystem::Instance();
			renderer.Clear();
			const TerraEngine::Renderer::RenderCaptureLimits limits = renderer.GetCaptureLimits();
			for (std::uint32_t index = 0u; index < limits.maxQueuedCaptures; ++index)
			{
				TerraEngine::Renderer::RenderCaptureRequest request;
				request.requestId = "availability-queue-" + std::to_string(index);
				request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
				request.sourceName = "Availability Queue Camera";
				request.targetSize = glm::uvec2(32u, 32u);
				std::string error;
				context.Expect(!renderer.QueueCaptureRequest(request, error).empty(), "Fixture should fill capture queue.");
			}
			const nlohmann::json queueFull = service.BuildCaptureAvailability();
			context.Expect(!queueFull.value("available", true), "Availability should become false when the queue is full.");
			context.ExpectEqual(std::string("queueFull"), queueFull.value("state", std::string()), "Availability should report queueFull state.");
			renderer.Clear();
		});

		registry.Add("Editor.MCP.CameraImages.GameViewCaptureReadOnlySuccessAndMissingCamera", [](TestContext& context) {
			ObjectManager::instance().clear();
			RenderSystem::Instance().Clear();
			EditorAutomationService service;
			const auto* capability = service.FindCapability("game_view.capture");
			context.Expect(capability != nullptr, "Game View capture capability should be registered.");
			context.ExpectEqual(std::string("capture"), capability->category, "Game View capture should be categorized as capture.");
			context.Expect(capability->mutationMode == MutationMode::ReadOnly, "Game View capture should not require write mode.");

			const nlohmann::json missing = service.HandleTool(
				"game_view.capture",
				{{"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"game-view-missing");
			context.Expect(!missing.value("ok", true), "Game View capture should fail clearly when no Game View camera is available.");
			context.ExpectEqual(std::string("missing_target"), missing["error"].value("category", std::string()), "Missing Game View camera should use missing-target category.");

			RenderSystem::Instance().SetGameViewTargetSize(glm::uvec2(64u, 64u));
			RenderSystem::Instance().SetGameViewCamera(
				glm::mat4(1.0f),
				glm::perspectiveZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f),
				glm::vec3(0.0f, 0.0f, 5.0f));
			const nlohmann::json captured = service.HandleTool(
				"game_view.capture",
				{{"width", 64u}, {"height", 64u}, {"freshness", "latestCompleted"}},
				WriteMode::ReadOnly,
				"game-view-success");

			context.Expect(captured.value("ok", false), "Game View capture should return an image when a camera descriptor exists.");
			context.ExpectEqual(std::string("gameView"), captured["data"]["source"].value("kind", std::string()), "Game View capture should report gameView source kind.");
			context.ExpectEqual(std::string("image"), captured["mcpContent"][0].value("type", std::string()), "Game View capture should include an MCP image item.");

			// A hidden Game View has no freshly published dock camera. The service
			// must snapshot the authored scene camera and keep Game View identity
			// while the renderer uses a dedicated offscreen target.
			RenderSystem::Instance().Clear();
			Object authoredCamera = ObjectManager::instance().createObject("Hidden Game View Camera");
			CameraComponent* authoredCameraComponent = ObjectManager::instance().AddComponent<CameraComponent>(authoredCamera);
			Transform* authoredTransform = ObjectManager::instance().GetComponent<Transform>(authoredCamera);
			context.Expect(authoredCameraComponent != nullptr, "Fixture should add an authored camera component.");
			context.Expect(authoredTransform != nullptr, "Authored camera objects should include a Transform.");
			authoredTransform->position = glm::vec3(0.0f, 1.0f, 5.0f);
			authoredCameraComponent->aspectRatio = 1.0f;

			const nlohmann::json nextFrame = service.HandleTool(
				"game_view.capture",
				{{"width", 64u}, {"height", 64u}, {"freshness", "nextFrame"}},
				WriteMode::ReadOnly,
				"game-view-next-frame-hidden");
			context.Expect(nextFrame.value("ok", false), "nextFrame Game View capture should use the authored camera when the dock has not published a frame.");
			context.ExpectEqual(std::string("gameView"), nextFrame["data"]["source"].value("kind", std::string()), "Dedicated nextFrame capture should preserve Game View source identity.");
			context.ExpectEqual(std::string("nextFrame"), nextFrame["data"].value("freshness", std::string()), "Dedicated capture should preserve nextFrame freshness metadata.");
			context.Expect(nextFrame["data"].value("deterministicFallback", false), "Headless contract validation should disclose deterministic fallback pixels.");

			RenderSystem::Instance().Clear();
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.CameraObjectCaptureContracts", [](TestContext& context) {
			ObjectManager::instance().clear();
			EditorAutomationService service;
			Object cameraObject = ObjectManager::instance().createObject("Contract Camera");
			CameraComponent* camera = ObjectManager::instance().AddComponent<CameraComponent>(cameraObject);
			context.Expect(camera != nullptr, "Fixture should add camera component.");

			const nlohmann::json success = service.HandleTool(
				"camera.capture_object",
				{{"object", TerraEngine::ToHexString(ObjectManager::instance().getUUID(cameraObject))}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-success");
			context.Expect(success.value("ok", false), "Camera object capture should accept a unique UUID target.");
			context.ExpectEqual(std::string("cameraObject"), success["data"]["source"].value("kind", std::string()), "Camera object capture should report cameraObject source kind.");

			camera->active = false;
			ObjectManager::instance().setObjectActive(cameraObject, false);
			const nlohmann::json disabled = service.HandleTool(
				"camera.capture_object",
				{{"object", TerraEngine::ToHexString(ObjectManager::instance().getUUID(cameraObject))}, {"includeDisabled", true}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-disabled");
			context.Expect(disabled.value("ok", false), "Camera object capture should allow disabled authored cameras when requested.");
			context.Expect(disabled["data"]["source"].value("disabled", false), "Disabled camera object metadata should report disabled source state.");

			const nlohmann::json excluded = service.HandleTool(
				"camera.capture_object",
				{{"object", TerraEngine::ToHexString(ObjectManager::instance().getUUID(cameraObject))}, {"includeDisabled", false}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-disabled-reject");
			context.Expect(!excluded.value("ok", true), "includeDisabled=false should reject disabled camera objects.");
			context.ExpectEqual(std::string("invalid_camera_object"), excluded["error"].value("category", std::string()), "Disabled camera rejection should use invalid-camera category.");

			ObjectManager::instance().setObjectActive(cameraObject, true);
			camera->active = true;
			Object duplicateA = ObjectManager::instance().createObject("Ambiguous Camera");
			Object duplicateB = ObjectManager::instance().createObject("Ambiguous Camera");
			ObjectManager::instance().AddComponent<CameraComponent>(duplicateA);
			ObjectManager::instance().AddComponent<CameraComponent>(duplicateB);
			const nlohmann::json ambiguous = service.HandleTool(
				"camera.capture_object",
				{{"object", "Ambiguous Camera"}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-ambiguous");
			context.Expect(!ambiguous.value("ok", true), "Ambiguous camera object names should be rejected.");
			context.ExpectEqual(std::string("conflict"), ambiguous["error"].value("category", std::string()), "Ambiguous camera object should use conflict category.");

			const nlohmann::json missing = service.HandleTool(
				"camera.capture_object",
				{{"object", "Missing Camera"}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-missing");
			context.Expect(!missing.value("ok", true), "Missing camera object should be rejected.");
			context.ExpectEqual(std::string("missing_target"), missing["error"].value("category", std::string()), "Missing camera object should use missing-target category.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.CustomCameraValidationContracts", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json nestedContract = service.HandleTool(
				"camera.capture_custom",
				{
					{"camera", {
						{"position", nlohmann::json::array({0.0f, 2.0f, 6.0f})},
						{"lookAt", nlohmann::json::array({0.0f, 1.0f, 0.0f})},
						{"up", nlohmann::json::array({0.0f, 1.0f, 0.0f})},
						{"projection", "perspective"},
						{"verticalFovDegrees", 60.0f},
						{"nearPlane", 0.1f},
						{"farPlane", 1000.0f}
					}},
					{"width", 64u},
					{"height", 64u}
				},
				WriteMode::ReadOnly,
				"custom-nested-contract");
			context.Expect(nestedContract.value("ok", false), "Custom camera capture should accept the documented nested camera descriptor.");
			context.ExpectEqual(std::string("customCamera"), nestedContract["data"]["source"].value("kind", std::string()), "Nested custom capture should retain its semantic source kind.");

			const nlohmann::json valid = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 1.0f, 5.0f})}, {"target", nlohmann::json::array({0.0f, 0.0f, 0.0f})}, {"projection", "orthographic"}, {"orthographicHeight", 8.0f}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-valid");
			context.Expect(valid.value("ok", false), "Custom camera capture should accept valid orthographic ad-hoc settings.");
			context.ExpectEqual(std::string("customCamera"), valid["data"]["source"].value("kind", std::string()), "Custom camera capture should report customCamera source kind.");

			const nlohmann::json invalidClip = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"nearPlane", 10.0f}, {"farPlane", 1.0f}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-invalid-clip");
			context.Expect(!invalidClip.value("ok", true), "Custom camera should reject invalid clipping ranges.");

			const nlohmann::json nonFinite = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({std::numeric_limits<double>::infinity(), 0.0, 5.0})}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-non-finite");
			context.Expect(!nonFinite.value("ok", true), "Custom camera should reject non-finite transforms.");

			const nlohmann::json unsupportedProjection = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"projection", "fisheye"}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-unsupported-projection");
			context.Expect(!unsupportedProjection.value("ok", true), "Custom camera should reject unsupported projection modes.");
		});

		registry.Add("Editor.MCP.CameraImages.CaptureRemainsCompatibleAcrossPlayStates", [](TestContext& context) {
			EditorAutomationService service;
			ScriptingSystem& scripting = ScriptingSystem::Instance();
			const auto capture = [&service, &context](std::string_view state, bool running, bool paused) {
				ScriptingSystem::Instance().SetRunning(running);
				ScriptingSystem::Instance().SetPaused(paused);
				const std::string correlationId = "capture-" + std::string(state);
				const nlohmann::json result = service.HandleTool(
					"camera.capture_custom",
					{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"width", 32u}, {"height", 32u}, {"correlationId", correlationId}},
					WriteMode::ReadOnly,
					"request-" + std::string(state));
				context.Expect(result.value("ok", false), "Read-only camera capture should remain available while " + std::string(state) + ".");
				context.ExpectEqual(correlationId, result.value("correlationId", std::string()), "Capture results should preserve the diagnostic correlation identifier.");
			};

			capture("stopped", false, false);
			capture("playing", true, false);
			capture("paused", true, true);
			scripting.SetPaused(false);
			scripting.SetRunning(false);
		});

		registry.Add("Editor.MCP.CameraImages.RendererUnavailableIsRejectedBeforeHandler", [](TestContext& context) {
			EditorAutomationService service;
			AutomationRequestContext request;
			request.requestId = "capture-renderer-unavailable";
			request.correlationId = "renderer-down";
			request.arguments = {{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"width", 32u}, {"height", 32u}};
			request.state.rendererAvailable = false;
			request.grantedPermission = AutomationPermission::Read;

			const AutomationDispatchResult result = service.GetRegistry().Dispatch("camera.capture_custom", 1, request);
			context.Expect(!result.ok, "A renderer-dependent capture must not run when discovery reports the renderer unavailable.");
			context.Expect(result.error == ErrorCategory::Unavailable, "Renderer unavailability should use the typed unavailable category.");
			context.Expect(!result.message.empty(), "Unavailable capture should explain which prerequisite is missing.");
		});
	}
}
