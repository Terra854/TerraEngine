#include "../TestHarness.h"
#include "EditorAutomationService.h"
#include "EditorMcpBridge.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "Tests/Support/DecodedCaptureAssertions.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <limits>

namespace TerraEngine::EditorTests
{
	static nlohmann::json CanonicalPosition(double x, double y, double z)
	{
		return {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {x, y, z}}};
	}
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::EditorMcpBridge;
	using TerraEngine::Editor::Automation::MutationMode;
	using TerraEngine::Editor::Automation::AutomationDispatchResult;
	using TerraEngine::Editor::Automation::AutomationPermission;
	using TerraEngine::Editor::Automation::AutomationRequestContext;
	using TerraEngine::Editor::Automation::ErrorCategory;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		// Synchronous MCP capture polls owned worker results itself. Create an
		// executor only when this test owns its lifetime; never stop another fixture's pool.
		class CameraCaptureSession final
		{
		public:
			CameraCaptureSession()
				: ownsExecutor_(ThreadPool::instance().getThreadCount() == 0U),
				  running_(ScriptingSystem::Instance().IsRunning()),
				  paused_(ScriptingSystem::Instance().IsPaused())
			{
				RenderSystem::Instance().Clear();
				if(ownsExecutor_) ThreadPool::instance().initialize(2U);
			}
			~CameraCaptureSession()
			{
				// Release renderer jobs while the executor that can settle them is alive.
				RenderSystem::Instance().Clear();
				if(ownsExecutor_) ThreadPool::instance().shutdown();
				ScriptingSystem::Instance().SetRunning(running_);
				ScriptingSystem::Instance().SetPaused(paused_);
				ObjectManager::instance().clear();
			}
		private:
			bool ownsExecutor_;
			bool running_;
			bool paused_;
		};

		void ExpectSyntheticImage(TestContext& context, const nlohmann::json& result,
			std::uint32_t width, std::uint32_t height, bool transparent = false)
		{
			context.Expect(result.value("ok", false), "Synthetic MCP capture must report success before publishing an image.");
			context.Expect(result.contains("mcpContent") && result["mcpContent"].is_array()
				&& result["mcpContent"].size() == 1U, "A successful capture must publish exactly one MCP image item.");
			const auto& image = result["mcpContent"][0];
			context.ExpectEqual(std::string("image"), image.value("type", std::string()), "Capture content must be an MCP image.");
			context.ExpectEqual(std::string("image/png"), image.value("mimeType", std::string()), "The actual image item must advertise PNG.");
			// Decode the transmitted bytes independently; metadata alone cannot prove
			// extent, alpha, checksums, or that the image is anything other than a placeholder.
			const auto decoded = TerraEngine::TestSupport::DecodeCaptureBase64(image.value("data", std::string()));
			context.ExpectEqual(width, decoded.width, "PNG width must match the requested extent.");
			context.ExpectEqual(height, decoded.height, "PNG height must match the requested extent.");
			context.ExpectEqual(static_cast<std::size_t>(width) * height * 4U, decoded.rgba.size(), "Decoded RGBA storage must cover every promised pixel.");
			for(std::size_t offset = 0U; offset < decoded.rgba.size(); offset += 4U)
			{
				context.Expect(decoded.rgba[offset] == 0U && decoded.rgba[offset + 1U] == 0U
					&& decoded.rgba[offset + 2U] == 0U, "Every synthetic pixel must be black.");
				context.ExpectEqual(static_cast<std::uint8_t>(transparent ? 0U : 255U), decoded.rgba[offset + 3U], "Synthetic alpha must honor transparentBackground.");
			}
			const auto& data = result["data"];
			context.ExpectEqual(width, data.value("width", 0U), "Capture metadata width must agree with decoded pixels.");
			context.ExpectEqual(height, data.value("height", 0U), "Capture metadata height must agree with decoded pixels.");
			context.Expect(!data.value("backendReadback", true), "Headless synthetic pixels must not claim backend readback.");
			context.Expect(data.value("deterministicFallback", false), "Synthetic pixels must disclose deterministic fallback provenance.");
			context.Expect(!data.value("overlaysIncluded", true), "Black synthetic pixels cannot claim rendered overlays.");
			context.Expect(!data.contains("backendTarget"), "Synthetic image metadata must not invent a native target identity.");
		}

		void ExpectNoImageContent(TestContext& context, const nlohmann::json& result)
		{
			context.Expect(!result.contains("mcpContent"), "Failed captures must not publish any MCP image payload.");
		}
	}

	void RegisterEditorMcpCameraImageContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.CameraImages.AvailabilityResourceHasContractShape", [](TestContext& context) {
			CameraCaptureSession captureSession;
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
				{{"camera", {{"position", CanonicalPosition(0.0, 0.0, 5.0)}}}, {"width", 4096u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"availability-large-resolution");
			ExpectSyntheticImage(context, largeCapture, 4096U, 64U);
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
			CameraCaptureSession captureSession;
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
			ExpectNoImageContent(context, missing);
			context.Expect(!missing.value("ok", true), "Game View capture should fail clearly when no Game View camera is available.");
			context.ExpectEqual(std::string("missing_target"), missing["error"].value("category", std::string()), "Missing Game View camera should use missing-target category.");

			RenderSystem::Instance().SetGameViewTargetSize(glm::uvec2(64u, 64u));
			TerraEngine::Scene::WorldPosition gameViewPosition;
			gameViewPosition.offsetMetres = {0.0, 0.0, 5.0};
			RenderSystem::Instance().SetGameViewCamera(
				glm::mat4(1.0f),
				glm::perspectiveZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f),
				gameViewPosition);
			const nlohmann::json captured = service.HandleTool(
				"game_view.capture",
				{{"width", 64u}, {"height", 64u}, {"freshness", "latestCompleted"}},
				WriteMode::ReadOnly,
				"game-view-success");

			ExpectSyntheticImage(context, captured, 64U, 64U);
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
			TerraEngine::Scene::WorldPose authoredPose;
			authoredPose.position.offsetMetres = {0.0, 1.0, 5.0};
			context.Expect(
				ObjectManager::instance().SetCanonicalWorldPose(authoredCamera, authoredPose) == TerraEngine::Scene::WorldCoordinateError::None,
				"The authored capture camera must accept its canonical pose.");
			authoredCameraComponent->aspectRatio = 1.0f;

			const nlohmann::json nextFrame = service.HandleTool(
				"game_view.capture",
				{{"width", 64u}, {"height", 64u}, {"freshness", "nextFrame"}},
				WriteMode::ReadOnly,
				"game-view-next-frame-hidden");
			ExpectSyntheticImage(context, nextFrame, 64U, 64U);
			context.Expect(nextFrame.value("ok", false), "nextFrame Game View capture should use the authored camera when the dock has not published a frame.");
			context.ExpectEqual(std::string("gameView"), nextFrame["data"]["source"].value("kind", std::string()), "Dedicated nextFrame capture should preserve Game View source identity.");
			context.ExpectEqual(std::string("nextFrame"), nextFrame["data"].value("freshness", std::string()), "Dedicated capture should preserve nextFrame freshness metadata.");
			context.Expect(nextFrame["data"].value("deterministicFallback", false), "Headless contract validation should disclose deterministic fallback pixels.");

			RenderSystem::Instance().Clear();
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.CameraObjectCaptureContracts", [](TestContext& context) {
			CameraCaptureSession captureSession;
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
			ExpectSyntheticImage(context, success, 64U, 64U);
			context.Expect(success.value("ok", false), "Camera object capture should accept a unique UUID target.");
			context.ExpectEqual(std::string("cameraObject"), success["data"]["source"].value("kind", std::string()), "Camera object capture should report cameraObject source kind.");

			camera->active = false;
			ObjectManager::instance().setObjectActive(cameraObject, false);
			const nlohmann::json disabled = service.HandleTool(
				"camera.capture_object",
				{{"object", TerraEngine::ToHexString(ObjectManager::instance().getUUID(cameraObject))}, {"includeDisabled", true}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-disabled");
			ExpectSyntheticImage(context, disabled, 64U, 64U);
			context.Expect(disabled.value("ok", false), "Camera object capture should allow disabled authored cameras when requested.");
			context.Expect(disabled["data"]["source"].value("disabled", false), "Disabled camera object metadata should report disabled source state.");

			const nlohmann::json excluded = service.HandleTool(
				"camera.capture_object",
				{{"object", TerraEngine::ToHexString(ObjectManager::instance().getUUID(cameraObject))}, {"includeDisabled", false}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-disabled-reject");
			ExpectNoImageContent(context, excluded);
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
			ExpectNoImageContent(context, ambiguous);
			context.Expect(!ambiguous.value("ok", true), "Ambiguous camera object names should be rejected.");
			context.ExpectEqual(std::string("conflict"), ambiguous["error"].value("category", std::string()), "Ambiguous camera object should use conflict category.");

			const nlohmann::json missing = service.HandleTool(
				"camera.capture_object",
				{{"object", "Missing Camera"}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"camera-object-missing");
			ExpectNoImageContent(context, missing);
			context.Expect(!missing.value("ok", true), "Missing camera object should be rejected.");
			context.ExpectEqual(std::string("missing_target"), missing["error"].value("category", std::string()), "Missing camera object should use missing-target category.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.MCP.CameraImages.CustomCameraValidationContracts", [](TestContext& context) {
			CameraCaptureSession captureSession;
			EditorAutomationService service;
			const nlohmann::json nestedContract = service.HandleTool(
				"camera.capture_custom",
				{
					{"camera", {
						{"position", CanonicalPosition(0.0, 2.0, 6.0)},
						{"lookAt", CanonicalPosition(0.0, 1.0, 0.0)},
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
			ExpectSyntheticImage(context, nestedContract, 64U, 64U);
			context.Expect(nestedContract.value("ok", false), "Custom camera capture should accept the documented nested camera descriptor.");
			context.ExpectEqual(std::string("customCamera"), nestedContract["data"]["source"].value("kind", std::string()), "Nested custom capture should retain its semantic source kind.");

			const nlohmann::json valid = service.HandleTool(
				"camera.capture_custom",
				{{"camera", {{"position", CanonicalPosition(0.0, 1.0, 5.0)}, {"lookAt", CanonicalPosition(0.0, 0.0, 0.0)}, {"projection", "orthographic"}, {"orthographicHeight", 8.0f}}}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-valid");
			ExpectSyntheticImage(context, valid, 64U, 64U);
			context.Expect(valid.value("ok", false), "Custom camera capture should accept valid orthographic ad-hoc settings.");
			context.ExpectEqual(std::string("customCamera"), valid["data"]["source"].value("kind", std::string()), "Custom camera capture should report customCamera source kind.");

			const nlohmann::json sceneCapture = service.HandleTool(
				"camera.capture_custom",
				{{"camera", {{"position", CanonicalPosition(0.0, 1.0, 5.0)}, {"lookAt", CanonicalPosition(0.0, 0.0, 0.0)}, {"viewType", "Scene"}}}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-scene-capture");
			ExpectSyntheticImage(context, sceneCapture, 64U, 64U);
			context.Expect(sceneCapture.value("ok", false)
				&& sceneCapture["data"]["camera"].value("viewType", std::string()) == "Scene",
				"A valid custom capture must retain its requested Scene identity for capture-only diagnostics and picking.");

			const nlohmann::json invalidClip = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"nearPlane", 10.0f}, {"farPlane", 1.0f}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-invalid-clip");
			ExpectNoImageContent(context, invalidClip);
			context.Expect(!invalidClip.value("ok", true), "Custom camera should reject invalid clipping ranges.");

			const nlohmann::json nonFinite = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({std::numeric_limits<double>::infinity(), 0.0, 5.0})}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-non-finite");
			ExpectNoImageContent(context, nonFinite);
			context.Expect(!nonFinite.value("ok", true), "Custom camera should reject non-finite transforms.");

			const nlohmann::json unsupportedProjection = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 5.0f})}, {"projection", "fisheye"}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"custom-unsupported-projection");
			ExpectNoImageContent(context, unsupportedProjection);
			context.Expect(!unsupportedProjection.value("ok", true), "Custom camera should reject unsupported projection modes.");
		});

		registry.Add("Editor.MCP.CameraImages.CaptureRemainsCompatibleAcrossPlayStates", [](TestContext& context) {
			CameraCaptureSession captureSession;
			EditorAutomationService service;
			ScriptingSystem& scripting = ScriptingSystem::Instance();
			const auto capture = [&service, &context](std::string_view state, bool running, bool paused) {
				ScriptingSystem::Instance().SetRunning(running);
				ScriptingSystem::Instance().SetPaused(paused);
				const std::string correlationId = "capture-" + std::string(state);
				const nlohmann::json result = service.HandleTool(
					"camera.capture_custom",
					{{"camera", {{"position", CanonicalPosition(0.0, 0.0, 5.0)}}}, {"width", 32u}, {"height", 32u}, {"correlationId", correlationId}},
					WriteMode::ReadOnly,
					"request-" + std::string(state));
				ExpectSyntheticImage(context, result, 32U, 32U);
				context.Expect(result.value("ok", false), "Read-only camera capture should remain available while " + std::string(state) + ".");
				context.ExpectEqual(correlationId, result.value("correlationId", std::string()), "Capture results should preserve the diagnostic correlation identifier.");
			};

			capture("stopped", false, false);
			capture("playing", true, false);
			capture("paused", true, true);
			scripting.SetPaused(false);
			scripting.SetRunning(false);
		});

		registry.Add("Editor.MCP.CameraImages.DecodedSyntheticExtentAlphaAndOverlayTruth", [](TestContext& context) {
			CameraCaptureSession captureSession;
			EditorAutomationService service;
			// Cover every promised extent with both alpha and overlay request modes.
			// The decoder validates transmitted pixels independently of JSON metadata.
			for(const glm::uvec2 extent : {glm::uvec2(1U, 1U), glm::uvec2(7U, 3U), glm::uvec2(17U, 9U), glm::uvec2(640U, 360U), glm::uvec2(1280U, 720U), glm::uvec2(4096U, 4096U)})
			{
				for(const bool transparent : {false, true})
				{
					for(const bool overlays : {false, true})
					{
						const std::string requestId = "decoded-" + std::to_string(extent.x) + "x" + std::to_string(extent.y)
							+ (transparent ? "-transparent" : "-opaque") + (overlays ? "-overlays" : "-plain");
						const nlohmann::json result = service.HandleTool(
							"camera.capture_custom",
							{{"camera", {{"position", CanonicalPosition(0.0, 0.0, 5.0)}}},
							 {"width", extent.x}, {"height", extent.y}, {"transparentBackground", transparent},
							 {"includeOverlays", overlays}, {"freshness", "latestCompleted"}, {"timeoutMs", 30000U}}, WriteMode::ReadOnly,
							requestId);
						ExpectSyntheticImage(context, result, extent.x, extent.y, transparent);
						// Bind the decoded bytes to this tool invocation as well as its
						// dimensions, so a stale image cannot satisfy another request.
						context.ExpectEqual(requestId,
							result.value("requestId", std::string()), "Every matrix response must preserve its tool request identity.");
						const auto& data = result["data"];
						context.Expect(data.value("requestId", std::string()) == requestId
							&& data.value("sourceKind", std::string()) == TerraEngine::Renderer::ToString(TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera)
							&& data.value("sourceName", std::string()) == "Custom Camera"
							&& data.value("freshness", std::string()) == "latestCompleted",
							"Every decoded image must preserve the requested camera source and freshness.");
						if(overlays)
						{
							context.Expect(result["data"].contains("warnings")
								&& !result["data"]["warnings"].empty(), "Unavailable synthetic overlays must produce a warning.");
						}
						std::cout << "[CAPTURE_MATRIX] editor " << extent.x << 'x' << extent.y
							<< " transparent=" << transparent << " overlays=" << overlays << " decoded=passed\n";
					}
				}
			}
		});

		registry.Add("Editor.MCP.CameraImages.RendererUnavailableIsRejectedBeforeHandler", [](TestContext& context) {
			CameraCaptureSession captureSession;
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
