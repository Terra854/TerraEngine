// Runtime acceptance host for the packaged color controller. This code is linked
// only into BUILD_TESTING launchers; it adds no gameplay or managed capture API.
#include "PostProcessColorRuntimeFixture.h"

#include "Core/MainEngine.h"
#include "Components/CameraComponent.h"
#include "Components/Transform.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderGraph.h"
#include "DecodedCaptureAssertions.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <span>
#include <string_view>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using Json = nlohmann::json;
		using namespace TerraEngine::Renderer;
		using namespace TerraEngine::Renderer::GraphicsSettings;
		void Require(bool valid, const std::string& message) { if(!valid) throw std::runtime_error(message); }
		std::string Environment(const char* name)
		{
			const char* value = std::getenv(name);
			Require(value && *value, std::string("Missing fixture environment: ") + name);
			return value;
		}
		bool SafeToken(const std::string& value)
		{
			return !value.empty() && value.size() <= 100 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos;
		}
		void AtomicJson(const std::filesystem::path& path, const Json& document)
		{
			const auto pending = std::filesystem::path(path.native() + std::filesystem::path(".pending").native());
			{ std::ofstream output(pending, std::ios::binary | std::ios::trunc); output << document.dump(2) << '\n'; output.flush(); Require(output.good(), "Cannot flush fixture evidence."); }
#ifdef _WIN32
			Require(MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "Cannot atomically publish fixture evidence.");
#else
			std::filesystem::rename(pending, path);
#endif
		}
		std::vector<std::uint8_t> PngBytes(const std::string& base64)
		{
			// Capture is engine-produced, but validate the alphabet and PNG with the
			// independent maintained decoder before writing an evidence artifact.
			Require(base64.size() % 4 == 0, "Capture Base64 length is invalid.");
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::vector<std::uint8_t> bytes;
			unsigned bits = 0, count = 0;
			bool padding = false;
			for(char character : base64)
			{
				if(character == '=') { padding = true; continue; }
				const auto digit = alphabet.find(character);
				Require(!padding && digit != std::string_view::npos, "Capture Base64 alphabet is invalid.");
				bits = (bits << 6u) | static_cast<unsigned>(digit); count += 6;
				if(count >= 8) { count -= 8; bytes.push_back(static_cast<std::uint8_t>((bits >> count) & 255u)); }
			}
			return bytes;
		}
		const char* StageName(RenderCaptureImageStage stage)
		{
			switch(stage) { case RenderCaptureImageStage::Raw: return "raw"; case RenderCaptureImageStage::Processed: return "processed"; case RenderCaptureImageStage::Composited: return "composited"; default: return "legacy"; }
		}
		const char* EncodingName(RenderCaptureColorEncoding encoding)
		{
			switch(encoding) { case RenderCaptureColorEncoding::Linear: return "linear"; case RenderCaptureColorEncoding::Srgb: return "srgb"; default: return "legacy"; }
		}
		std::uint64_t Unsigned(const Json& document, const char* name)
		{
			const auto& value = document.at(name);
			return value.is_string() ? std::stoull(value.get<std::string>()) : value.get<std::uint64_t>();
		}
		struct AuthoredCamera
		{
			GraphicsCameraTarget identity;
			RenderCaptureRequest request;
		};
		AuthoredCamera ReadCamera(RenderSystem& renderer, const char* name, const Json& observed)
		{
			// Engine::Update has returned: no scheduler job is touching scene storage.
			// Copy camera and lifetime once before entering the renderer's protected
			// capture boundary. No scene/component writes are performed by this host.
			auto& objects = ObjectManager::instance();
			const auto object = objects.FindObjectByName(name);
			Require(object.has_value(), std::string("Missing authored camera: ") + name);
			const auto* transform = objects.GetComponent<Transform>(*object);
			const auto* camera = objects.GetComponent<CameraComponent>(*object);
			Require(transform && camera, "Authored capture needs Transform and CameraComponent.");
			const auto resolved = renderer.ResolveGraphicsSettingsCameraTarget(static_cast<std::uint32_t>(object->EntityId()));
			Require(resolved && resolved->camera, "Authored camera has no protected graphics lifetime.");
			const auto identity = *resolved->camera;
			Require(ToHexString(identity.cameraId) == observed.at("cameraId").get<std::string>()
				&& identity.sceneGeneration == Unsigned(observed, "sceneGeneration")
				&& identity.objectLifetimeGeneration == Unsigned(observed, "objectLifetimeGeneration")
				&& identity.componentGeneration == Unsigned(observed, "componentGeneration")
				&& identity.bindingGeneration == Unsigned(observed, "bindingGeneration"), "Managed checkpoint camera lifetime differs from protected native state.");
			RenderCaptureRequest request;
			request.sourceKind = RenderCaptureSourceKind::CameraObject; request.viewType = RenderViewType::Game;
			request.sourceName = name; request.sourceObjectId = identity.cameraId; request.graphicsCamera = identity;
			request.targetSize = {observed.at("outputWidth").get<std::uint32_t>(), observed.at("outputHeight").get<std::uint32_t>()};
			Require(request.targetSize.x && request.targetSize.y, "Checkpoint output extent is empty.");
			const glm::quat rotation = glm::normalize(transform->worldPose.rotation);
			const auto forward = rotation * glm::vec3(0, 0, -1);
			const auto up = rotation * camera->up;
			Require(std::isfinite(camera->verticalFovDegrees) && camera->verticalFovDegrees > 0 && camera->verticalFovDegrees < 179
				&& std::isfinite(camera->aspectRatio) && camera->aspectRatio > 0 && std::isfinite(camera->nearPlane) && std::isfinite(camera->farPlane)
				&& camera->nearPlane > 0 && camera->farPlane > camera->nearPlane && glm::length(up) > 0.0001f, "Authored camera projection is invalid.");
			request.cameraCanonicalPosition = transform->worldPose.position;
			request.viewMatrix = glm::lookAt(glm::vec3(0), forward, glm::normalize(up));
			request.projectionMatrix = glm::perspectiveZO(glm::radians(camera->verticalFovDegrees), camera->aspectRatio, camera->nearPlane, camera->farPlane);
			request.projectionMatrix[1][1] *= -1;
			request.freshness = "nextFrame"; request.allowBackendFallback = false; request.includeOverlays = false; request.timeoutMs = 10000;
			return {identity, std::move(request)};
		}
		void CaptureCamera(RenderSystem& renderer, const std::filesystem::path& root, const std::string& tag,
			const char* name, const Json& observed, Json& rows)
		{
			auto camera = ReadCamera(renderer, name, observed);
			GraphicsTarget target{GraphicsScope::Camera, camera.identity, {}};
			const auto snapshot = renderer.GetGraphicsSettingsSnapshot(target);
			Require(snapshot && snapshot->revision == Unsigned(observed, "revision"), "Checkpoint settings revision is not the native effective publication.");
			const auto plan = BuildPostProcessGraphSettings(*snapshot);
			Require(plan.executionAvailable || (!plan.hdrCamera && !plan.toneAndGrade), "Requested color stack lacks executable native resources.");
			// First-use dedicated views need a real render before explicit stage
			// readback can name their accepted output; never forge a certificate.
			auto warmRequest = camera.request;
			warmRequest.requestId = "color-warm-" + tag + "-" + name;
			const auto warm = renderer.CaptureImage(warmRequest);
			Require(warm.ok && warm.backendReadback && !warm.deterministicFallback && warm.backendTarget && warm.frameId,
				"Dedicated camera warm render failed: " + warm.message);
			const std::array active{RenderCaptureImageStage::Raw, RenderCaptureImageStage::Processed, RenderCaptureImageStage::Composited};
			const std::array disabled{RenderCaptureImageStage::Legacy};
			const auto stages = plan.executionAvailable ? std::span<const RenderCaptureImageStage>(active) : std::span<const RenderCaptureImageStage>(disabled);
			for(const auto stage : stages)
			{
				auto request = camera.request; request.imageStage = stage;
				const std::string stem = tag + "-" + name + "-" + StageName(stage);
				request.requestId = "color-" + stem;
				const auto capture = renderer.CaptureImage(request);
				Require(capture.ok && capture.backendReadback && !capture.deterministicFallback && !capture.pendingAsync && capture.backendTarget
					&& capture.imageStage == stage && capture.frameId >= Unsigned(observed, "frameId") && capture.frameId != 0 && capture.size == request.targetSize && capture.mimeType == "image/png",
					"Actual packaged capture failed: " + capture.message);
				const auto& backend = *capture.backendTarget;
				Require(backend.targetGeneration && backend.rendererSessionGeneration && backend.dedicatedCaptureTarget
					&& backend.sourceWidth == request.targetSize.x && backend.sourceHeight == request.targetSize.y, "Capture target provenance is incomplete.");
				if(stage != RenderCaptureImageStage::Legacy)
					Require(capture.graphicsSettingsRevision == snapshot->revision && capture.deliveredColorEncoding == RenderCaptureColorEncoding::Srgb
						&& (capture.sourceColorEncoding == RenderCaptureColorEncoding::Linear || capture.sourceColorEncoding == RenderCaptureColorEncoding::Srgb), "Capture color publication/transfer mismatches the checkpoint.");
				const auto decoded = TerraEngine::TestSupport::DecodeCaptureBase64(capture.base64Data);
				Require(decoded.width == request.targetSize.x && decoded.height == request.targetSize.y, "Decoded PNG extent differs from native capture.");
				const auto png = root / (stem + ".png"); Require(!std::filesystem::exists(png), "Duplicate capture artifact path.");
				const auto bytes = PngBytes(capture.base64Data);
				{ std::ofstream output(png, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); output.flush(); Require(output.good(), "Cannot flush captured PNG."); }
				rows.push_back({{"tag", tag}, {"camera", name}, {"cameraUUID", ToHexString(camera.identity.cameraId)},
					{"sceneGeneration", camera.identity.sceneGeneration}, {"objectLifetimeGeneration", camera.identity.objectLifetimeGeneration},
					{"componentGeneration", camera.identity.componentGeneration}, {"bindingGeneration", camera.identity.bindingGeneration},
					{"stage", StageName(stage)}, {"imageStage", StageName(stage)}, {"frameId", capture.frameId}, {"settingsRevision", capture.graphicsSettingsRevision},
					{"expectedSettingsRevision", snapshot->revision}, {"width", capture.size.x}, {"height", capture.size.y},
					{"sourceColorEncoding", EncodingName(capture.sourceColorEncoding)}, {"deliveredColorEncoding", EncodingName(capture.deliveredColorEncoding)},
					{"sourceEncoding", EncodingName(capture.sourceColorEncoding)}, {"deliveredEncoding", EncodingName(capture.deliveredColorEncoding)},
					{"backendReadback", capture.backendReadback}, {"deterministicFallback", capture.deterministicFallback}, {"overlaysIncluded", capture.overlaysIncluded},
					{"backendTarget", {{"rendererSessionGeneration", backend.rendererSessionGeneration}, {"targetGeneration", backend.targetGeneration},
						{"sourceWidth", backend.sourceWidth}, {"sourceHeight", backend.sourceHeight}, {"dedicatedCaptureTarget", backend.dedicatedCaptureTarget}}},
					{"png", png.filename().generic_string()}});
			}
		}
	}

	int RunPostProcessColorRuntimeFixture(const std::filesystem::path& evidenceDirectory)
	{
		Json report{{"schemaVersion", 1}, {"status", "running"}, {"captures", Json::array()}, {"checkpoints", Json::array()}};
		Engine engine;
		bool evidenceReady = false;
		try
		{
			const auto root = std::filesystem::absolute(evidenceDirectory);
			Require(std::filesystem::is_directory(root), "Capture evidence directory must exist.");
			Require(!std::filesystem::exists(root / "runtime-captures.json"), "Runtime capture report already exists.");
			// The owning runner may open stdout/stderr and its isolated data store
			// here before launching us. Prior image evidence is never accepted.
			for(const auto& entry : std::filesystem::directory_iterator(root))
				Require(entry.path().extension() != ".png", "Runtime capture directory contains prior PNG evidence.");
			evidenceReady = true;
			const auto observationRoot = std::filesystem::absolute(std::filesystem::u8path(Environment("TERRAENGINE_COLOR_FIXTURE_ROOT")));
			const auto run = Environment("TERRAENGINE_COLOR_FIXTURE_RUN_ID"); Require(SafeToken(run), "Invalid color run identity.");
			report["runId"] = run;
			engine.Initialize();
			auto* renderer = engine.GetSystem<RenderSystem>(); Require(renderer, "Runtime color validation needs the actual renderer.");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
			std::set<std::string> completed;
			std::string boot;
			for(;;)
			{
				Require(std::chrono::steady_clock::now() < deadline, "Runtime color acceptance exceeded its deadline.");
				Require(engine.Update(1.0f / 60.0f), "Runtime stopped before color acceptance completed.");
				const auto path = observationRoot / "color-observation.json";
				if(!std::filesystem::is_regular_file(path)) continue;
				Json observation;
				{ std::ifstream input(path, std::ios::binary); input >> observation; }
				Require(observation.at("runId") == run && observation.at("schemaVersion") == 1, "Managed color observation identity/schema mismatch.");
				const auto currentBoot = observation.at("bootId").get<std::string>(); Require(SafeToken(currentBoot), "Invalid color boot identity.");
				if(boot.empty()) { boot = currentBoot; report["bootId"] = boot; }
				Require(boot == currentBoot, "Managed fixture restarted within a capture process.");
				const auto status = observation.at("status").get<std::string>();
				Require(status != "failed", "Managed color fixture failed: " + observation.value("failure", std::string{}));
				if(status == "passed") { report["settingsObservation"] = observation; break; }
				const auto tag = observation.at("captureTag").get<std::string>();
				if(observation.at("phase") != "Capture" || tag.empty() || completed.contains(tag)) continue;
				Require(SafeToken(tag), "Invalid managed capture tag.");
				CaptureCamera(*renderer, root, tag, "CameraA", observation.at("cameraA"), report["captures"]);
				CaptureCamera(*renderer, root, tag, "CameraB", observation.at("cameraB"), report["captures"]);
				// Persist real PNG/provenance first. A correlated ACK only releases
				// settings progression; the external runner scores the actual pixels.
				report["checkpoints"].push_back(observation);
				AtomicJson(root / "runtime-captures.json", report);
				// Each exact phase has its own receipt so a Windows reader of the
				// prior phase cannot prevent publication of this acknowledgement.
				AtomicJson(observationRoot / ("color-capture-ack-" + tag + ".json"),
					{{"runId", run}, {"bootId", boot}, {"tag", tag}});
				completed.insert(tag);
			}
			engine.Shutdown(); report["orderlyShutdown"] = true; report["status"] = "passed";
			AtomicJson(root / "runtime-captures.json", report); return 0;
		}
		catch(const std::exception& error)
		{
			try { engine.Shutdown(); report["orderlyShutdown"] = true; } catch(...) { report["orderlyShutdown"] = false; }
			report["status"] = "failed"; report["failure"] = error.what();
			if(evidenceReady) { try { AtomicJson(evidenceDirectory / "runtime-captures.json", report); } catch(...) {} }
			return 1;
		}
	}
}
