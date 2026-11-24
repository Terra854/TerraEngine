// Packaged authoring fixture: real native state/readbacks precede every ACK.
#include "PostProcessAuthoringRuntimeFixture.h"
#include "Core/MainEngine.h"
#include "Components/CameraComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/GraphicsSettings/PostProcessAuthoringController.h"
#include "Renderer/GraphicsSettings/PostProcessAuthoringFrame.h"
#include "Renderer/GraphicsSettings/PostProcessDiagnostics.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/PostProcessEffectAsset.h"
#include "DecodedCaptureAssertions.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace TerraEngine::Tests
{
	namespace
	{
		using Json = nlohmann::json;
		using namespace Renderer;
		using namespace Renderer::GraphicsSettings;
		constexpr std::array<std::string_view, 4> tags{"baseline", "enabled", "parameter-changed", "disabled"};
		constexpr std::array<std::string_view, 2> names{"CameraA", "CameraB"};
		constexpr glm::uvec2 extent{1280u, 720u};
		void Require(bool valid, const std::string& detail) { if (!valid) throw std::runtime_error(detail); }
		std::string Environment(const char* name)
		{
			const char* text = std::getenv(name);
			Require(text && *text, std::string("Missing authoring fixture environment: ") + name); return text;
		}
		bool SafeToken(const std::string& text)
		{
			return !text.empty() && text.size() <= 100 && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos;
		}
		std::uint64_t Number(const Json& object, const char* name)
		{
			const auto& value = object.at(name);
			if (value.is_string()) {
				const auto text = value.get<std::string>();
				Require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, std::string("Invalid decimal native token: ") + name);
				return std::stoull(text);
			}
			Require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0), "Native token must be unsigned.");
			return value.get<std::uint64_t>();
		}
		UUID Asset(const char* name)
		{
			const auto id = ParseUUID(Environment(name)); Require(id && *id != EMPTY_UUID, "Invalid authoring fixture asset UUID."); return *id;
		}
		UUID ObservedProfileIdentity(const Json& value)
		{
			Require(value.is_string(), "Observed camera profile identity must be a string.");
			const auto text = value.get<std::string>();
			// The managed optional asset reference renders EMPTY_UUID as an empty
			// string. Native scene JSON renders that same identity as 32 zeroes.
			// Compare UUID values rather than requiring identical wire spellings.
			if (text.empty()) return EMPTY_UUID;
			const auto parsed = ParseUUID(text);
			Require(parsed.has_value(), "Observed camera profile identity is malformed.");
			return *parsed;
		}
		void AtomicJson(const std::filesystem::path& path, const Json& document)
		{
			const auto pending = std::filesystem::path(path.native() + std::filesystem::path(".pending").native());
			{ std::ofstream output(pending, std::ios::binary | std::ios::trunc); output << document.dump(2) << '\n'; output.flush(); Require(output.good(), "Cannot flush authoring evidence."); }
#ifdef _WIN32
			Require(MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "Cannot publish authoring evidence atomically.");
#else
			std::filesystem::rename(pending, path);
#endif
		}
		std::vector<std::uint8_t> PngBytes(const std::string& encoded)
		{
			Require(encoded.size() % 4 == 0, "Native capture Base64 length is invalid.");
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::vector<std::uint8_t> bytes; unsigned bits = 0, count = 0; bool padding = false;
			for (const auto character : encoded) {
				if (character == '=') { padding = true; continue; }
				const auto digit = alphabet.find(character); Require(!padding && digit != std::string_view::npos, "Native capture Base64 alphabet is invalid.");
				bits = (bits << 6u) | static_cast<unsigned>(digit); count += 6;
				if (count >= 8) { count -= 8; bytes.push_back(static_cast<std::uint8_t>((bits >> count) & 255u)); }
			}
			return bytes;
		}
		std::string Hash(std::span<const std::uint8_t> bytes)
		{
#ifdef _WIN32
			// Use the platform provider already shipped with Windows. RAII retains
			// and releases both native handles on every failure path.
			struct Algorithm { BCRYPT_ALG_HANDLE value{}; ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); } } algorithm;
			struct Digest { BCRYPT_HASH_HANDLE value{}; ~Digest() { if (value) BCryptDestroyHash(value); } } digest;
			Require(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot open native SHA256 provider.");
			Require(BCryptCreateHash(algorithm.value, &digest.value, nullptr, 0, nullptr, 0, 0) >= 0, "Cannot create native image hash.");
			for (std::size_t offset = 0; offset < bytes.size();) {
				const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 65536));
				Require(BCryptHashData(digest.value, const_cast<PUCHAR>(bytes.data() + offset), count, 0) >= 0, "Cannot hash native PNG bytes."); offset += count;
			}
			std::array<std::uint8_t, 32> result{}; Require(BCryptFinishHash(digest.value, result.data(), static_cast<ULONG>(result.size()), 0) >= 0, "Cannot finish native image hash.");
			constexpr char digits[] = "0123456789abcdef"; std::string text;
			for (const auto byte : result) { text += digits[byte >> 4u]; text += digits[byte & 15u]; } return text;
#else
			(void)bytes; throw std::runtime_error("The installed authoring fixture requires the Windows native SHA256 provider.");
#endif
		}
		const char* Encoding(RenderCaptureColorEncoding value)
		{
			switch (value) { case RenderCaptureColorEncoding::Linear: return "linear"; case RenderCaptureColorEncoding::Srgb: return "srgb"; default: return "legacy"; }
		}
		const char* Stage(RenderCaptureImageStage value)
		{
			switch (value) { case RenderCaptureImageStage::Raw: return "raw"; case RenderCaptureImageStage::Processed: return "processed"; case RenderCaptureImageStage::Composited: return "composited"; default: return "legacy"; }
		}
		void ValidateEffect(const AuthoringWire::Snapshot& snapshot, const Json& observed, const UUID& effectId, const UUID& shaderId)
		{
			const auto effect = std::find_if(snapshot.effectiveEffects.begin(), snapshot.effectiveEffects.end(), [&](const auto& item) { return item.assetId == effectId; });
			const auto& expected = observed.at("customEffect");
			Require(ToHexString(effectId) == expected.at("assetId") && ToHexString(shaderId) == expected.at("shaderAssetId"), "Checkpoint names another effect/shader.");
			// CameraB need not contain this effect. Its absence is an exact disabled
			// state, not a missing CameraA descriptor being accepted as execution.
			if (effect == snapshot.effectiveEffects.end()) { Require(!expected.at("enabled").get<bool>(), "An enabled effect is missing from native effective state."); return; }
			Require(effect->enabled == expected.at("enabled").get<bool>(), "Native custom enable state differs from the managed checkpoint.");
			const auto descriptor = std::find_if(snapshot.descriptors.begin(), snapshot.descriptors.end(), [&](const auto& item) { return item.effect && item.effect->descriptor.assetId == effectId; });
			Require(descriptor != snapshot.descriptors.end() && descriptor->available && descriptor->effect->descriptor.shaderAssetId == shaderId
				&& descriptor->effect->descriptor.parameterBytes == 20, "The effective effect has no matching qualified native shader descriptor.");
			std::uint32_t gains = 0, tints = 0;
			for (const auto& parameter : effect->parameters) {
				if (parameter.name == "gain") { ++gains; Require(std::holds_alternative<float>(parameter.value)
					&& std::get<float>(parameter.value) == expected.at("gain").get<float>(), "Native independent gain differs.");
				}
				if (parameter.name == "tint") { ++tints; Require(std::holds_alternative<std::array<float, 4>>(parameter.value)
					&& std::get<std::array<float, 4>>(parameter.value) == expected.at("tint").get<std::array<float, 4>>(), "Native independent tint differs.");
				}
			}
			Require(gains == 1 && tints == 1, "Native custom state omitted or duplicated an independent typed parameter.");
		}
		struct CameraCapture
		{
			GraphicsTarget target;
			std::shared_ptr<const GraphicsSettingsSnapshot> graphics;
			RenderCaptureRequest request;
			bool neutralCamera = false;
		};
		bool MayUseEmptyNeutralAuthoring(bool neutralCamera, bool authoringPresent,
			std::uint8_t retainedCustomStages, std::uint8_t plannedCustomStages,
			bool currentNativeOutput, bool activeNativeCustomStage) noexcept
		{
			return neutralCamera && !authoringPresent && retainedCustomStages == 0u && plannedCustomStages == 0u
				&& currentNativeOutput && !activeNativeCustomStage;
		}
		CameraCapture ReadCamera(RenderSystem& renderer, const std::string& name, const Json& observed,
			std::map<std::string, GraphicsTarget>& pinned, const UUID& effectId, const UUID& shaderId, Json& report)
		{
			// Engine::Update has settled every job. Read live authored pose only at
			// this protected gap, then carry copies through native capture admission.
			auto& objects = ObjectManager::instance(); const auto object = objects.FindObjectByName(name);
			Require(object.has_value(), "Missing packaged authored camera: " + name);
			const auto* camera = objects.GetComponent<CameraComponent>(*object); const auto* transform = objects.GetComponent<Transform>(*object);
			const auto pose = objects.GetResolvedWorldPose(*object);
			Require(camera && camera->active && transform && transform->active && pose && Scene::IsWorldPoseFinite(*pose), "Packaged capture has no active canonical camera pose.");
			const auto resolved = renderer.ResolveGraphicsSettingsCameraTarget(object->entityValue);
			Require(resolved && resolved->camera, "Packaged camera has no protected native identity.");
			const auto identity = *resolved->camera;
			Require(ToHexString(identity.cameraId) == observed.at("cameraId") && identity.sceneGeneration == Number(observed, "sceneGeneration")
				&& identity.objectLifetimeGeneration == Number(observed, "objectLifetimeGeneration") && identity.componentGeneration == Number(observed, "componentGeneration")
				&& identity.bindingGeneration == Number(observed, "bindingGeneration"), "Managed camera tokens differ from current protected native ownership.");
			if (!pinned.contains(name)) {
				const auto accepted = renderer.GetPostProcessAuthoringController().ReadAcceptedView(*resolved);
				Require(accepted.snapshot && accepted.snapshot->target.view, "Authoring camera does not have one unambiguous accepted native view: " + accepted.detail);
				pinned.emplace(name, accepted.snapshot->target);
			}
			const auto& target = pinned.at(name);
			Require(target.camera == resolved->camera && target.view && target.view->lifetimeGeneration == Number(observed, "viewLifetimeGeneration"), "Authoring capture crossed the pinned native view lifetime.");
			const auto accepted = renderer.GetPostProcessAuthoringController().ReadAcceptedView(target);
			Require(accepted.snapshot && accepted.snapshot->revision == Number(observed, "authoringRevision")
				&& accepted.snapshot->effectiveFrameId >= Number(observed, "effectiveFrameId") && accepted.snapshot->effectiveFrameId,
				"Checkpoint authoring revision has no matching accepted native output.");
			const auto observedProfile = ObservedProfileIdentity(observed.at("authoredProfileAssetId"));
			// SetProfile is deliberately exercised in this runtime session after
			// package load. Require its real live and accepted identities, without
			// assuming the pre-Play saved scene already contained that runtime edit.
			report["nativeCameraProfileChecks"].push_back({{"camera", name}, {"cameraUUID", ToHexString(identity.cameraId)},
				{"observedProfileWire", observed.at("authoredProfileAssetId")}, {"observedProfileUUID", ToHexString(observedProfile)},
				{"liveProfileUUID", ToHexString(camera->PostProcessProfileId)}, {"acceptedProfileUUID", ToHexString(accepted.snapshot->authoredProfileAssetId)},
				{"authoringRevision", accepted.snapshot->revision}, {"sceneVisibleFrameId", accepted.snapshot->sceneVisibleFrameId}, {"effectiveFrameId", accepted.snapshot->effectiveFrameId}});
			Require(camera->PostProcessProfileId == observedProfile && accepted.snapshot->authoredProfileAssetId == observedProfile,
				"Native live/accepted camera profile differs from managed authoring publication for " + name
				+ ": live=" + ToHexString(camera->PostProcessProfileId) + ", accepted=" + ToHexString(accepted.snapshot->authoredProfileAssetId)
				+ ", observed=" + ToHexString(observedProfile));
			ValidateEffect(*accepted.snapshot, observed, effectId, shaderId);
			const auto graphics = renderer.GetGraphicsSettingsSnapshot(target);
			Require(graphics && graphics->revision == Number(observed, "revision") && graphics->lifecycle == accepted.snapshot->lifecycle
				&& graphics->lifecycle.rendererSessionGeneration == Number(observed, "rendererSessionGeneration")
				&& graphics->lifecycle.backendResourceGeneration == Number(observed, "backendResourceGeneration"), "Checkpoint graphics publication/lifecycle differs from the retained native camera.");
			Require(BuildPostProcessGraphSettings(*graphics).executionAvailable, "Requested explicit authoring stages lack native preparation; legacy substitution is forbidden.");
			const auto rotation = glm::normalize(pose->rotation); const auto up = rotation * camera->up;
			Require(std::isfinite(camera->verticalFovDegrees) && camera->verticalFovDegrees > 0 && camera->verticalFovDegrees < 179
				&& std::isfinite(camera->aspectRatio) && camera->aspectRatio > 0 && std::isfinite(camera->nearPlane)
				&& std::isfinite(camera->farPlane) && camera->nearPlane > 0 && camera->farPlane > camera->nearPlane && glm::length(up) > .0001f, "Packaged camera projection is invalid.");
			RenderCaptureRequest request; request.sourceKind = RenderCaptureSourceKind::CameraObject; request.viewType = RenderViewType::Game;
			request.sourceName = name; request.sourceObjectId = identity.cameraId; request.graphicsCamera = identity; request.targetSize = extent;
			request.cameraCanonicalPosition = pose->position; request.viewMatrix = glm::lookAt(glm::vec3(0), rotation * glm::vec3(0, 0, -1), glm::normalize(up));
			request.projectionMatrix = glm::perspectiveZO(glm::radians(camera->verticalFovDegrees), camera->aspectRatio, camera->nearPlane, camera->farPlane); request.projectionMatrix[1][1] *= -1;
			request.freshness = "nextFrame"; request.allowBackendFallback = false; request.includeOverlays = false; request.timeoutMs = 30000;
			const bool neutralCamera = name == "CameraB" && ToHexString(identity.cameraId) == "75000000000000000000000000000002"
				&& observedProfile == EMPTY_UUID && accepted.snapshot->authoredProfileAssetId == EMPTY_UUID
				&& accepted.snapshot->revision == 1u && accepted.snapshot->requestedEffects.empty()
				&& accepted.snapshot->effectiveEffects.empty() && !observed.at("customEffect").at("enabled").get<bool>();
			return {target, graphics, std::move(request), neutralCamera};
		}
		void CaptureCamera(RenderSystem& renderer, const std::filesystem::path& root, const std::string& tag,
			const std::string& name, const Json& observed, CameraCapture camera, Json& report)
		{
			// Explicitly initialize the real dedicated target using a Legacy request,
			// as the maintained color host does. This render is recorded separately
			// and never substitutes for any of the three required stage captures.
			auto warmRequest = camera.request; warmRequest.requestId = "authoring-warm-" + tag + "-" + name;
			const auto warm = renderer.CaptureImage(warmRequest);
			Require(warm.ok && warm.backendReadback && !warm.deterministicFallback && !warm.pendingAsync && warm.backendTarget && warm.frameId
				&& warm.imageStage == RenderCaptureImageStage::Legacy, "Actual authoring dedicated-target initialization failed: " + warm.message);
			report["targetInitialization"].push_back({{"tag", tag}, {"camera", name}, {"requestedStage", "legacy"}, {"frameId", warm.frameId}, {"targetGeneration", warm.backendTarget->targetGeneration}});
			for (const auto stage : {RenderCaptureImageStage::Raw, RenderCaptureImageStage::Processed, RenderCaptureImageStage::Composited}) {
				auto request = camera.request; request.imageStage = stage; const auto stem = tag + "-" + name + "-" + Stage(stage); request.requestId = "authoring-" + stem;
				const auto capture = renderer.CaptureImage(request);
				Require(capture.ok && capture.backendReadback && !capture.deterministicFallback && !capture.pendingAsync && capture.backendTarget
					&& capture.requestId == request.requestId && capture.imageStage == stage && capture.sourceKind == RenderCaptureSourceKind::CameraObject
					&& capture.frameId && capture.frameId >= Number(observed, "frameId") && capture.size == extent && capture.mimeType == "image/png"
					&& capture.graphicsSettingsRevision == camera.graphics->revision && !capture.overlaysIncluded, "Actual packaged authoring stage capture failed: " + capture.message);
				const auto& backend = *capture.backendTarget;
				Require(backend.targetGeneration && backend.rendererSessionGeneration == camera.graphics->lifecycle.rendererSessionGeneration
					&& backend.dedicatedCaptureTarget && backend.sourceWidth == extent.x && backend.sourceHeight == extent.y,
					"Authoring stage has no matching retained native target/session/extent.");
				Require(capture.deliveredColorEncoding == RenderCaptureColorEncoding::Srgb
					&& (capture.sourceColorEncoding == RenderCaptureColorEncoding::Linear || capture.sourceColorEncoding == RenderCaptureColorEncoding::Srgb), "Authoring stage omitted exact transfer metadata.");
				// The normal-view receipt is not proof that a dedicated capture used
				// its custom pass. Inspect the exact accepted native capture frame and
				// retained prepared shader/value owners, without inferring execution
				// from the managed request or another view with the same camera UUID.
				auto* native = dynamic_cast<VulkanContext*>(renderer.GetContext()); Require(native, "Authoring native capture proof requires the actual Vulkan context.");
				const auto publication = native->ReadAcceptedRenderFramePublication(capture.frameId);
				Require(publication && publication->rendererSessionGeneration == backend.rendererSessionGeneration
					&& publication->backendResourceGeneration == camera.graphics->lifecycle.backendResourceGeneration,
					"Capture has no exact accepted native frame publication.");
				const RenderViewPresentationInput* source = nullptr;
				std::size_t sourceCount = 0;
				Json witness{{"tag", tag}, {"camera", name}, {"stage", Stage(stage)}, {"captureFrameId", capture.frameId},
					{"readbackRevision", capture.graphicsSettingsRevision}, {"normalViewRevision", camera.graphics->revision},
					{"readbackTargetGeneration", backend.targetGeneration}, {"rendererSessionGeneration", backend.rendererSessionGeneration},
					{"backendResourceGeneration", publication->backendResourceGeneration}, {"neutralCameraQualified", camera.neutralCamera},
					{"presentationCount", publication->presentationInputs.size()}, {"sources", Json::array()}};
				for (std::size_t index = 0; index < publication->presentationInputs.size(); ++index) {
					const auto& input = publication->presentationInputs[index];
					const bool matches = input.graphicsCamera == camera.target.camera && input.output.kind == RenderOutputKind::OffscreenTexture
						&& input.output.targetSize == extent;
					if (matches) { ++sourceCount; if (!source) source = &input; }
					if (index >= 16u) continue; // Keep failure evidence bounded without hiding the total/match counts.
					std::size_t enabled = 0;
					if (input.graphicsAuthoring && input.graphicsAuthoring->effects)
						enabled = std::count_if(input.graphicsAuthoring->effects->passes.begin(), input.graphicsAuthoring->effects->passes.end(), [](const auto& pass) { return pass.enabled; });
					Json row{{"index", index}, {"matchesRequestedCameraCapture", matches}, {"outputKind", input.output.kind}, {"outputId", input.output.outputId},
						{"width", input.output.targetSize.x}, {"height", input.output.targetSize.y}, {"referenceFrameId", input.referenceFrame.frameId},
						{"cameraUUID", input.graphicsCamera ? ToHexString(input.graphicsCamera->cameraId) : ""},
						{"cameraSceneGeneration", input.graphicsCamera ? input.graphicsCamera->sceneGeneration : 0u},
						{"cameraObjectLifetime", input.graphicsCamera ? input.graphicsCamera->objectLifetimeGeneration : 0u},
						{"cameraComponentGeneration", input.graphicsCamera ? input.graphicsCamera->componentGeneration : 0u},
						{"cameraBindingGeneration", input.graphicsCamera ? input.graphicsCamera->bindingGeneration : 0u},
						{"viewLifetimeGeneration", input.graphicsView ? input.graphicsView->lifetimeGeneration : 0u},
						{"settingsPresent", bool(input.graphicsSettings)}, {"settingsRevision", input.graphicsSettings ? input.graphicsSettings->revision : 0u},
						{"retainedCustomStages", input.graphicsSettings ? input.graphicsSettings->customPostProcessStages : 0u},
						{"authoringPresent", bool(input.graphicsAuthoring)}, {"authoringRevision", input.graphicsAuthoring ? input.graphicsAuthoring->authoringRevision : 0u},
						{"retainedEnabledCustomPasses", enabled}};
					witness["sources"].push_back(std::move(row));
				}
				witness["matchingSourceCount"] = sourceCount;
				report["captureSourceWitnesses"].push_back(witness);
				Require(sourceCount == 1u, "Native authoring capture source is missing or ambiguous; see captureSourceWitnesses.");
				Require(source && source->graphicsView && source->graphicsView->lifetimeGeneration && source->graphicsSettings
					&& source->graphicsSettings->revision == capture.graphicsSettingsRevision
					&& source->graphicsSettings->lifecycle == camera.graphics->lifecycle && source->referenceFrame.frameId == capture.frameId
					&& source->graphicsSettings->target.camera == source->graphicsCamera && source->graphicsSettings->target.view == source->graphicsView,
					"Capture did not retain its exact camera/view/settings source; see captureSourceWitnesses.");
				const auto plan = BuildPostProcessGraphSettings(*source->graphicsSettings);
				const auto diagnostics = native->GetPostProcessDiagnostics(*source);
				bool currentNativeOutput = diagnostics.frameId == capture.frameId && diagnostics.view == *source->graphicsView
					&& diagnostics.settingsRevision == capture.graphicsSettingsRevision && diagnostics.lifecycle == camera.graphics->lifecycle;
				bool activeOutput = false, activeCustom = false;
				for (const auto& diagnostic : diagnostics.effects) {
					if (diagnostic.state != GraphicsSettings::PostProcessEffectState::Active) continue;
					if (diagnostic.effect.passKind == PostProcessPassKind::CustomFullscreen) activeCustom = true;
					if (diagnostic.reason == PostProcessDiagnosticReason::None && diagnostic.timing.owner.targetGeneration
						&& diagnostic.timing.owner.frameId == capture.frameId) activeOutput = true;
				}
				currentNativeOutput = currentNativeOutput && activeOutput;
				auto& retainedWitness = report["captureSourceWitnesses"].back();
				retainedWitness["plannedCustomStages"] = plan.customStages; retainedWitness["currentAcceptedNativeOutput"] = currentNativeOutput;
				retainedWitness["activeNativeCustomStage"] = activeCustom;
				const bool emptyNeutralAuthoring = MayUseEmptyNeutralAuthoring(camera.neutralCamera, bool(source->graphicsAuthoring),
					source->graphicsSettings->customPostProcessStages, plan.customStages, currentNativeOutput, activeCustom);
				// Production deliberately omits an allocation for B's untouched
				// revision1 empty chain. That is permitted only with current native
				// output/resource proof and explicit zero custom-stage masks. A's
				// authored profile requires its retained owner even while disabled.
				Require(source->graphicsAuthoring || emptyNeutralAuthoring,
					"Capture lacks required authored ownership and is not an explicitly empty neutral camera chain; see captureSourceWitnesses.");
				if (source->graphicsAuthoring) {
					const auto& authoring = *source->graphicsAuthoring;
					Require(authoring.target.camera == source->graphicsCamera && authoring.target.view == source->graphicsView
						&& authoring.lifecycle == camera.graphics->lifecycle && authoring.authoringRevision == Number(observed, "authoringRevision")
						&& authoring.effects, "Dedicated native capture consumed another authoring revision/view.");
					const auto& expectedEffect = observed.at("customEffect");
					const auto pass = std::find_if(authoring.effects->passes.begin(), authoring.effects->passes.end(), [&](const auto& item) {
						return ToHexString(item.descriptor.assetId) == expectedEffect.at("assetId");
					});
					if (expectedEffect.at("enabled").get<bool>()) {
						Require(pass != authoring.effects->passes.end() && pass->enabled && pass->shader
							&& ToHexString(pass->descriptor.shaderAssetId) == expectedEffect.at("shaderAssetId"), "Native capture omitted the enabled prepared custom shader owner.");
						std::uint32_t gains = 0, tints = 0;
						for (const auto& value : pass->values) {
							if (value.name == "gain") { ++gains; Require(std::holds_alternative<float>(value.value) && std::get<float>(value.value) == expectedEffect.at("gain").get<float>(), "Native capture consumed another gain."); }
							if (value.name == "tint") { ++tints; Require(std::holds_alternative<std::array<float, 4>>(value.value) && std::get<std::array<float, 4>>(value.value) == expectedEffect.at("tint").get<std::array<float, 4>>(), "Native capture consumed another tint."); }
						}
						Require(gains == 1 && tints == 1, "Prepared native capture omitted independent custom parameter owners.");
					} else Require(pass == authoring.effects->passes.end() || !pass->enabled, "Disabled native capture still retained an enabled custom pass.");
				}
				const auto bytes = PngBytes(capture.base64Data); const auto decoded = TestSupport::DecodePng(bytes);
				Require(decoded.width == extent.x && decoded.height == extent.y, "Decoded authoring PNG extent differs from native readback.");
				const auto png = root / (stem + ".png"); Require(!std::filesystem::exists(png), "Refusing to overwrite native capture evidence.");
				{ std::ofstream output(png, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); output.flush(); Require(output.good(), "Cannot flush native authoring PNG."); }
				Json row{{"tag", tag}, {"camera", name}, {"cameraUUID", ToHexString(camera.target.camera->cameraId)},
					{"stage", Stage(stage)}, {"imageStage", Stage(stage)}, {"png", png.filename().generic_string()}, {"sha256", Hash(bytes)},
					{"frameId", capture.frameId}, {"settingsRevision", capture.graphicsSettingsRevision}, {"expectedSettingsRevision", camera.graphics->revision},
					{"width", extent.x}, {"height", extent.y}, {"sourceEncoding", Encoding(capture.sourceColorEncoding)}, {"deliveredEncoding", Encoding(capture.deliveredColorEncoding)},
					{"backendReadback", capture.backendReadback}, {"deterministicFallback", capture.deterministicFallback}, {"overlaysIncluded", capture.overlaysIncluded},
					{"backendTarget", {{"targetGeneration", backend.targetGeneration}, {"rendererSessionGeneration", backend.rendererSessionGeneration}, {"sourceWidth", backend.sourceWidth}, {"sourceHeight", backend.sourceHeight}, {"dedicatedCaptureTarget", backend.dedicatedCaptureTarget}}},
					{"snapshot", observed}, {"nativeAuthoringTarget", {{"outputKind", camera.target.view->outputKind}, {"viewType", camera.target.view->viewType}, {"outputId", camera.target.view->outputId}, {"viewLifetimeGeneration", camera.target.view->lifetimeGeneration}}}};
				row["nativeCaptureView"] = {{"outputKind", source->graphicsView->outputKind}, {"viewType", source->graphicsView->viewType}, {"outputId", source->graphicsView->outputId}, {"viewLifetimeGeneration", source->graphicsView->lifetimeGeneration},
					{"authoringPresent", bool(source->graphicsAuthoring)}, {"emptyNeutralCustomChain", emptyNeutralAuthoring}, {"customStages", plan.customStages},
					{"authoringRevision", source->graphicsAuthoring ? source->graphicsAuthoring->authoringRevision : 0u},
					{"authoredResourceRevision", source->graphicsAuthoring ? source->graphicsAuthoring->authoredResourceRevision : 0u},
					{"preparedFromFrameId", source->graphicsAuthoring ? source->graphicsAuthoring->preparedFromFrameId : 0u}};
				AtomicJson(root / (stem + ".json"), row); report["captures"].push_back(std::move(row));
			}
		}
	}

	int RunPostProcessAuthoringRuntimeFixture(const std::filesystem::path& evidenceDirectory)
	{
		Json report{{"schemaVersion", 1}, {"mode", "export"}, {"status", "running"}, {"captures", Json::array()}, {"checkpoints", Json::array()}, {"targetInitialization", Json::array()}, {"nativeCheckpointReceipts", Json::array()}, {"nativeCameraProfileChecks", Json::array()}, {"captureSourceWitnesses", Json::array()}};
		Engine engine; bool evidenceReady = false;
		try {
			const auto root = std::filesystem::absolute(evidenceDirectory).lexically_normal();
			Require(std::filesystem::is_directory(root) && !std::filesystem::exists(root / "runtime-captures.json"), "Authoring evidence directory must exist with no prior capture report.");
			for (const auto& entry : std::filesystem::directory_iterator(root)) Require(entry.path().extension() != ".png", "Authoring evidence directory contains prior PNGs.");
			evidenceReady = true;
			const auto observationRoot = std::filesystem::absolute(std::filesystem::u8path(Environment("TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_ROOT"))).lexically_normal();
			Require(root == observationRoot, "Managed observation and native evidence roots must be identical.");
			const auto run = Environment("TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_RUN_ID"); Require(SafeToken(run), "Invalid authoring run ID.");
			const auto effectId = Asset("TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_EFFECT_ID"), shaderId = Asset("TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_SHADER_ID");
			report["runId"] = run; engine.Initialize(); auto* renderer = engine.GetSystem<RenderSystem>(); Require(renderer, "Authoring fixture needs the actual native renderer.");
			std::map<std::string, GraphicsTarget> pinned; std::string boot; std::size_t completed = 0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
			for (;;) {
				Require(std::chrono::steady_clock::now() < deadline, "Packaged authoring observation/capture deadline expired.");
				Require(engine.Update(1.0f / 60.0f), "Exported engine stopped before authoring completion.");
				const auto path = observationRoot / "custom-post-process-observation.json"; if (!std::filesystem::is_regular_file(path)) continue;
				Json observation; { std::ifstream input(path, std::ios::binary); input >> observation; }
				Require(observation.at("schemaVersion") == 1 && observation.at("runId") == run, "Packaged authoring observation identity/schema mismatch.");
				const auto currentBoot = observation.at("bootId").get<std::string>(); Require(SafeToken(currentBoot), "Invalid authoring boot identity.");
				if (boot.empty()) { boot = currentBoot; report["bootId"] = boot; } Require(boot == currentBoot, "Managed authoring fixture changed boot inside one process.");
				const auto status = observation.at("status").get<std::string>(); Require(status != "failed", "Packaged authoring fixture failed: " + observation.value("failure", std::string{}));
				if (status == "passed") { Require(completed == tags.size() && report["captures"].size() == 24, "Managed authoring passed before all exact native stage captures."); report["settingsObservation"] = observation; break; }
				if (observation.at("phase") != "Capture") continue;
				const auto tag = observation.at("captureTag").get<std::string>();
				if (completed && tag == tags[completed - 1]) continue;
				Require(completed < tags.size() && tag == tags[completed], "Authoring capture checkpoint was reordered or repeated.");
				const auto requestId = Number(observation, "requestId"); const auto receipt = renderer->GetPostProcessAuthoringController().ReadReceipt(requestId);
				Require(requestId && receipt && receipt->request.requestId == requestId && receipt->request.state == GraphicsRequestState::Applied
					&& receipt->request.reason == GraphicsRequestReason::None && receipt->effectiveFrameId
					&& receipt->request.effectiveRevision == Number(observation.at("cameraA"), "authoringRevision"), "Capture checkpoint lacks its exact actual native authoring receipt.");
				report["nativeCheckpointReceipts"].push_back({{"tag", tag}, {"requestId", std::to_string(receipt->request.requestId)},
					{"state", receipt->request.state}, {"reason", receipt->request.reason}, {"effectiveRevision", std::to_string(receipt->request.effectiveRevision)},
					{"appliedFrameId", std::to_string(receipt->request.appliedFrameId)}, {"sceneVisibleFrameId", std::to_string(receipt->sceneVisibleFrameId)},
					{"effectiveFrameId", std::to_string(receipt->effectiveFrameId)}, {"detail", receipt->request.detail}});
				// Pin both normal camera outputs before the first dedicated view is
				// created. Never guess an output ID or borrow another view's history.
				std::array<CameraCapture, 2> cameras;
				for (std::size_t index = 0; index < names.size(); ++index) cameras[index] = ReadCamera(*renderer, std::string(names[index]), observation.at(index ? "cameraB" : "cameraA"), pinned, effectId, shaderId, report);
				for (std::size_t index = 0; index < names.size(); ++index) CaptureCamera(*renderer, root, tag, std::string(names[index]), observation.at(index ? "cameraB" : "cameraA"), std::move(cameras[index]), report);
				report["checkpoints"].push_back(observation); AtomicJson(root / "runtime-captures.json", report);
				AtomicJson(observationRoot / ("custom-post-process-capture-ack-" + tag + ".json"), {{"schemaVersion", 1}, {"runId", run}, {"bootId", boot}, {"tag", tag}});
				++completed;
			}
			engine.Shutdown(); report["orderlyShutdown"] = true; report["status"] = "passed"; AtomicJson(root / "runtime-captures.json", report); return 0;
		}
		catch (const std::exception& error) {
			try { engine.Shutdown(); report["orderlyShutdown"] = true; } catch (...) { report["orderlyShutdown"] = false; }
			report["status"] = "failed"; report["failure"] = error.what();
			if (evidenceReady) { try { AtomicJson(evidenceDirectory / "runtime-captures.json", report); } catch (...) {} } return 1;
		}
	}
}
