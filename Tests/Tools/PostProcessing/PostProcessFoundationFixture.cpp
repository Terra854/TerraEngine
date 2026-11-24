// Installed Windows Vulkan evidence for fixed exposure and capture-stage parity.
// Run with --install-root <Build> --output <fresh.json> --run-nonce <unique-id>.
// The caller owns build/install identity pins and the external process watchdog.
#include <Windows.h>

#include "Core/Logger/Logger.h"
#include "DecodedCaptureAssertions.h"
#include "PostProcessScreenLightingGeometry.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "Terrain/TerrainMaterial.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Renderer/GraphicsSettings/PostProcessDiagnostics.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Window/GLFW/GlfwWindowBackend.h"
#include "Window/WindowSystem.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan_core.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
	using Json = nlohmann::json;
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Renderer::GraphicsSettings;
	using namespace TerraEngine::Renderer::Vulkan;
	using Decoded = TerraEngine::TestSupport::DecodedCapture;
	constexpr glm::uvec2 kExtent{320, 180};

	void Require(bool condition, std::string_view message)
	{
		if(!condition) throw std::runtime_error(std::string(message));
	}

	std::filesystem::path Utf8Path(std::string_view text)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
	}

	struct Options
	{
		std::filesystem::path installRoot;
		std::filesystem::path output;
		std::string nonce;
	};

	Options Parse(int count, char** arguments)
	{
		Options result;
		std::vector<std::string_view> seen;
		for(int index = 1; index < count; index += 2)
		{
			Require(index + 1 < count, "Each fixture option requires a value.");
			const std::string_view name = arguments[index];
			const std::string_view value = arguments[index + 1];
			Require(std::find(seen.begin(), seen.end(), name) == seen.end(), "Duplicate fixture option.");
			seen.push_back(name);
			if(name == "--install-root") result.installRoot = Utf8Path(value);
			else if(name == "--output") result.output = Utf8Path(value);
			else if(name == "--run-nonce") result.nonce = value;
			else throw std::runtime_error("Unknown fixture option.");
		}
		Require(!result.installRoot.empty() && !result.output.empty() && !result.nonce.empty(), "Missing fixture arguments.");
		Require(!std::filesystem::exists(result.output), "Fixture evidence must use a fresh path.");
		return result;
	}

	void WriteFresh(const std::filesystem::path& path, const Json& evidence)
	{
		if(!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "Cannot create fresh fixture evidence.");
		struct Owner { HANDLE file; ~Owner() { CloseHandle(file); } } owner{file};
		const auto bytes = evidence.dump(2) + '\n';
		std::size_t offset = 0;
		while(offset < bytes.size())
		{
			const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
			DWORD written = 0;
			Require(WriteFile(file, bytes.data() + offset, count, &written, nullptr) && written != 0, "Cannot write fixture evidence.");
			offset += written;
		}
		Require(FlushFileBuffers(file) != FALSE, "Cannot flush fixture evidence.");
	}

	struct SessionLog
	{
		std::mutex mutex;
		Json errors = Json::array();
		void Append(const LogEntry& entry)
		{
			if(entry.level < LogLevel::Error) return;
			std::lock_guard lock(mutex);
			errors.push_back({{"level", static_cast<int>(entry.level)}, {"message", entry.message}});
		}
	};

	struct ContextOwner
	{
		VulkanContext context;
		bool editor = false;
		bool closed = false;
		Json* presentationEvidence = nullptr;
		std::uint64_t pendingPublicationFrame = 0u;
		std::uint64_t completionBoundary = 0u;
		void Close()
		{
			if(closed) return;
			context.Shutdown();
			Require(context.IsShutdownComplete(), "Native fixture context retained resources after shutdown.");
			closed = true;
		}
		~ContextOwner() { try { Close(); } catch(...) {} }
	};

	RenderViewPresentationInput View(VulkanContext& context, std::uint64_t frame,
		RenderViewType type, RenderOutputKind kind, std::uint32_t output, glm::uvec2 extent,
		float exposure, bool hud, bool grid = false, std::span<const GraphicsSettingValue> colorValues = {},
		bool prepareTargets = true)
	{
		RenderViewPresentationInput input;
		input.output.kind = kind;
		input.output.outputId = output;
		input.output.diagnosticsView = type;
		input.output.targetSize = extent;
		input.referenceFrame.frameId = frame;
		input.referenceFrame.viewId = input.referenceFrame.outputId = output;
		input.referenceFrame.originGeneration = input.referenceFrame.spatialGeneration = input.referenceFrame.residencyGeneration = 1;
		input.sceneViewGridVisible = grid;
		input.sceneViewOrthographic = type == RenderViewType::Scene;
		input.settings.variableRateShading = 0;
		if(type == RenderViewType::Game)
		{
			// Use the actual procedural sky to produce nonzero raw camera pixels.
			// The Scene case instead uses its known uniform orthographic clear.
			input.sky.renderBackground = true;
			input.sky.exposure = 0.15f;
			input.sky.intensity = 0.4f;
			input.sky.horizonTint = glm::vec3(0.2f, 0.3f, 0.4f);
			input.sky.zenithTint = glm::vec3(0.1f, 0.2f, 0.3f);
			input.sky.groundTint = glm::vec3(0.1f);
		}
		if(hud)
		{
			input.uiBatch.viewportSize = glm::vec2(extent);
			UIStaticQuad quad;
			quad.position = glm::vec2(extent) * 0.5f - glm::vec2(20, 16);
			quad.size = {40, 32};
			quad.color = {0.2f, 0.4f, 0.1f, 1.0f};
			input.uiBatch.staticQuads.push_back(quad);
		}
		auto candidate = std::make_shared<GraphicsSettingsSnapshot>();
		candidate->lifecycle.rendererSessionGeneration = context.GetLatestRendererRecoverySnapshot().renderSessionGeneration;
		candidate->lifecycle.backendResourceGeneration = context.GetBackendResourceGeneration();
		candidate->revision = frame;
		candidate->publishedFrameId = frame;
		candidate->internalWidth = candidate->outputWidth = extent.x;
		candidate->internalHeight = candidate->outputHeight = extent.y;
		candidate->values.push_back({GraphicsSettingId::FixedExposureCompensation, exposure});
		// Rich scenarios append the real typed color settings before native
		// preparation. The original fixed-only calls retain their exact inputs.
		candidate->values.insert(candidate->values.end(), colorValues.begin(), colorValues.end());
		GraphicsAAConfiguration aa;
		Require(ResolveGraphicsAAConfiguration(candidate->values, aa) == GraphicsRequestReason::None,
			"Fixture values do not declare a valid AA configuration.");
		candidate->sampleCount = aa.sampleCount;
		candidate->internalWidth = ResolveGraphicsAAInternalDimension(extent.x, aa);
		candidate->internalHeight = ResolveGraphicsAAInternalDimension(extent.y, aa);
		PostProcessEffectSettings effects;
		Require(ResolvePostProcessEffectSettings(candidate->values, effects) == GraphicsRequestReason::None,
			"Fixture values do not declare valid camera effects.");
		if(effects.depthOfField.Active() || effects.motionBlur.Active() || effects.HasScreenSpace()
			|| aa.upscaling == GraphicsUpscalingMode::Temporal)
		{
			input.graphicsView = GraphicsViewIdentity{type, output, 1u, kind};
			candidate->target.view = input.graphicsView;
			input.projectionMatrix = glm::orthoRH_ZO(-3.2f, 3.2f, -1.8f, 1.8f, 0.1f, 25.0f);
			input.projectionMatrix[1][1] *= -1.0f;
		}
		if(prepareTargets)
		{
			std::string reason;
			const bool prepared = context.PrepareFixedExposureView(input, *candidate, reason);
			Require(prepared, "Actual color view preparation failed: " + reason);
			// Certification follows actual image/pipeline preparation, before sealing.
			candidate->postProcessResourceGeneration = context.GetBackendResourceGeneration();
		}
		input.graphicsSettings = std::move(candidate);
		return input;
	}

	std::shared_ptr<const RenderFramePublication> Publication(VulkanContext& context,
		std::uint64_t frame, std::vector<RenderViewPresentationInput> views)
	{
		const auto session = context.GetLatestRendererRecoverySnapshot().renderSessionGeneration;
		auto persistent = std::make_shared<RenderPersistentTopologyRoot>();
		auto dynamic = std::make_shared<RenderDynamicStateRoot>();
		persistent->generation = dynamic->generation = frame;
		persistent->rendererSessionGeneration = dynamic->rendererSessionGeneration = session;
		auto publication = std::make_shared<RenderFramePublication>();
		publication->frameId = publication->generation = frame;
		publication->rendererSessionGeneration = session;
		publication->backendResourceGeneration = context.GetBackendResourceGeneration();
		publication->sceneGeneration = 1;
		publication->persistent = std::move(persistent);
		publication->dynamic = std::move(dynamic);
		auto backend = std::make_shared<RenderBackendWorkPublication>();
		backend->frameId = backend->animationWork.frameId = frame;
		backend->valid = backend->animationWork.valid = true;
		publication->backendWork = std::move(backend);
		for(auto& input : views)
		{
			RenderViewSelection selection;
			selection.generation = frame;
			selection.outputId = input.output.outputId;
			selection.outputKind = input.output.kind;
			selection.viewType = input.output.diagnosticsView;
			publication->views.push_back(selection);
			publication->referenceFrames.push_back(input.referenceFrame);
			publication->presentationInputs.push_back(std::move(input));
		}
		const auto sealed = SealRenderFramePublication(std::move(publication), session);
		Require(sealed.Receipt().IsSuccess() && sealed.Publication(), "Fixture publication failed production sealing.");
		return sealed.Publication();
	}

	void FinishFrame(ContextOwner& owner)
	{
		owner.context.EndFrame();
		if(owner.editor)
		{
			// The production editor deliberately bounds WSI acquisition to one
			// millisecond. A timeout can already fence the submitted offscreen work
			// while reporting no UI submit. Continue only that explicit native state,
			// without recording another camera frame or advancing temporal history.
			auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
			Require(window != nullptr,"Editor completion lost its owned native window.");
			const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
			const auto boundary = ++owner.completionBoundary;
			unsigned attempt = 0u;
			for(;; ++attempt)
			{
				glfwPollEvents();
				int width = 0, height = 0; glfwGetFramebufferSize(window,&width,&height);
				Require(width > 0 && height > 0 && !glfwWindowShouldClose(window),
					"Editor completion has no viable owned framebuffer; no presentation was inferred.");
				const auto before = owner.context.GetEditorSubmissionProgress();
				const auto backendBefore = owner.context.GetBackendResourceGeneration();
				bool prepared = false, recorded = false;
				const auto receipt = owner.context.PresentEditor([&] { prepared = true; },[&](void*) { recorded = true; });
				const auto after = owner.context.GetEditorSubmissionProgress();
				DWORD foregroundProcess = 0u;
				if(const auto foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground,&foregroundProcess);
				Json event{{"boundary",boundary},{"attempt",attempt},{"preparedPublicationFrameId",owner.pendingPublicationFrame},
					{"uiOnlyContinuation",attempt != 0u},{"outcome",static_cast<unsigned>(receipt.outcome)},
					{"stage",EditorUiFrameStageName(receipt.stage)},{"nativeResult",receipt.nativeResult},
					{"submissionSerial",receipt.submissionSerial},{"uiPrepared",prepared},{"uiRecorded",recorded},
					{"beforeLastSubmittedSerial",before.lastSubmittedSerial},{"afterLastSubmittedSerial",after.lastSubmittedSerial},
					{"afterCompletedSerial",after.completedSerial},{"beforePresentationGeneration",before.presentationGeneration},
					{"afterPresentationGeneration",after.presentationGeneration},{"backendGenerationBefore",backendBefore},
					{"backendGenerationAfter",owner.context.GetBackendResourceGeneration()},{"deviceLost",after.deviceLost},
					{"framebufferExtent",{width,height}},{"windowVisible",glfwGetWindowAttrib(window,GLFW_VISIBLE) == GLFW_TRUE},
					{"windowFocused",glfwGetWindowAttrib(window,GLFW_FOCUSED) == GLFW_TRUE},{"foregroundProcessId",foregroundProcess}};
				if(receipt.acquiredImage) event["acquiredImage"] = {{"frameSlot",receipt.acquiredImage->frameSlot},
					{"imageIndex",receipt.acquiredImage->imageIndex},{"presentationGeneration",receipt.acquiredImage->presentationGeneration}};
				else event["acquiredImage"] = nullptr;
				if(owner.presentationEvidence) owner.presentationEvidence->push_back(event);
				const bool presented = receipt.stage == EditorUiFrameStage::Present
					&& (receipt.nativeResult == VK_SUCCESS || receipt.nativeResult == VK_SUBOPTIMAL_KHR);
				const bool accepted = (receipt.outcome == EditorPresentOutcome::Submitted
					|| receipt.outcome == EditorPresentOutcome::RecreationRequested)
					&& presented && receipt.submissionSerial > before.lastSubmittedSerial
					&& after.lastSubmittedSerial == receipt.submissionSerial && !after.deviceLost && prepared && recorded;
				if(accepted)
				{
					// A post-submit recreation may change the outcome after a real
					// successful present. Its positive current serial remains proof.
					owner.pendingPublicationFrame = 0u;
					return;
				}
				const bool boundedAcquireSkip = receipt.outcome == EditorPresentOutcome::Skipped
					&& receipt.stage == EditorUiFrameStage::AcquireImage
					&& (receipt.nativeResult == VK_TIMEOUT || receipt.nativeResult == VK_NOT_READY)
					&& receipt.submissionSerial == 0u && !receipt.acquiredImage && !prepared && !recorded
					&& after.lastSubmittedSerial == before.lastSubmittedSerial && !after.deviceLost
					&& before.presentationGeneration == after.presentationGeneration;
				Require(boundedAcquireSkip,"Native editor completion failed; exact receipt: "+event.dump());
				Require(std::chrono::steady_clock::now() < deadline,"Native editor acquisition remained unavailable; exact receipt: "+event.dump());
				// Pump real window events while waiting for the compositor's next
				// image. PresentEditor supports UI-only calls on the next frame slot;
				// its queue order consumes the already prepared offscreen submission.
				glfwWaitEventsTimeout(1.0/60.0);
			}
		}
		owner.pendingPublicationFrame = 0u;
	}

	void Draw(ContextOwner& owner, std::shared_ptr<const RenderFramePublication> publication)
	{
		Require(owner.context.ConsumeRenderFramePublication(publication), "Native backend rejected the sealed publication.");
		owner.pendingPublicationFrame = publication->frameId;
		for(std::uint32_t index = 0; index < publication->views.size(); ++index)
			owner.context.RenderFrame(publication, index);
		FinishFrame(owner);
		owner.context.WaitIdle();
	}

	struct Capture
	{
		RenderCaptureResult result;
		Decoded pixels;
	};

	Capture Read(VulkanContext& context, std::string id, RenderViewType type, bool dedicated,
		std::uint64_t frame, glm::uvec2 extent, RenderCaptureImageStage stage, Json* observation = nullptr)
	{
		RenderCaptureRequest request;
		request.requestId = std::move(id);
		request.sourceKind = dedicated ? RenderCaptureSourceKind::CustomCamera : RenderCaptureSourceKind::GameView;
		request.viewType = type;
		request.targetSize = extent;
		request.submittedFrameId = frame;
		request.dedicatedRenderTarget = dedicated;
		request.allowBackendFallback = false;
		request.timeoutMs = 10000;
		request.imageStage = stage;
		const auto accepted = context.ReadbackCapture(request);
		Require(accepted.ok && accepted.pendingAsync && accepted.backendReadback, "Native stage readback was not submitted: " + accepted.message);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for(;;)
		{
			for(auto& result : context.PollCaptureReadbacks())
			{
				// Temporal diagnostics must survive a rejected metadata/decoding
				// assertion. Preserve the actual native receipt before checking it.
				if(observation)
				{
					*observation = {{"requestId",result.requestId},{"ok",result.ok},{"message",result.message},
						{"frameId",result.frameId},{"settingsRevision",result.graphicsSettingsRevision},
						{"imageStage",static_cast<unsigned>(result.imageStage)},{"sourceEncoding",static_cast<unsigned>(result.sourceColorEncoding)},
						{"deliveredEncoding",static_cast<unsigned>(result.deliveredColorEncoding)},
						{"backendReadback",result.backendReadback},{"deterministicFallback",result.deterministicFallback}};
					if(result.backendTarget) (*observation)["target"] = {{"width",result.backendTarget->sourceWidth},
						{"height",result.backendTarget->sourceHeight},{"generation",result.backendTarget->targetGeneration}};
				}
				Require(result.requestId == request.requestId, "Unexpected readback correlation.");
				Require(result.ok && result.backendReadback && !result.deterministicFallback && result.backendTarget,
					"Actual stage capture failed: " + result.message);
				const bool legacy = stage == RenderCaptureImageStage::Legacy;
				Require(result.frameId == frame && result.imageStage == stage
					&& result.graphicsSettingsRevision == (legacy ? 0 : frame),
					"Capture returned a different stage or submitted settings frame.");
				Require(legacy
					? result.sourceColorEncoding == RenderCaptureColorEncoding::Legacy && result.deliveredColorEncoding == RenderCaptureColorEncoding::Legacy
					: result.sourceColorEncoding == RenderCaptureColorEncoding::Linear && result.deliveredColorEncoding == RenderCaptureColorEncoding::Srgb,
					"Native capture transfer metadata does not describe one linear-to-sRGB conversion.");
				Require(result.backendTarget->sourceWidth == extent.x && result.backendTarget->sourceHeight == extent.y,
					"Native stage extent mismatches the requested view.");
				auto decoded = TerraEngine::TestSupport::DecodeCaptureBase64(result.base64Data);
				Require(decoded.width == extent.x && decoded.height == extent.y, "Decoded PNG extent mismatches native provenance.");
				return {std::move(result), std::move(decoded)};
			}
			Require(std::chrono::steady_clock::now() < deadline, "Actual stage readback did not complete.");
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	std::array<int, 4> Pixel(const Capture& capture, std::uint32_t x, std::uint32_t y)
	{
		const auto offset = (static_cast<std::size_t>(y) * capture.pixels.width + x) * 4;
		Require(offset + 3 < capture.pixels.rgba.size(), "Decoded pixel is outside the image.");
		return {capture.pixels.rgba[offset], capture.pixels.rgba[offset + 1], capture.pixels.rgba[offset + 2], capture.pixels.rgba[offset + 3]};
	}

	int Encode(float value)
	{
		value = std::clamp(value, 0.0f, 1.0f);
		const float encoded = value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
		return static_cast<int>(std::round(encoded * 255));
	}

	void ExpectColor(const std::array<int, 4>& pixel, glm::vec3 linear, std::string_view message)
	{
		for(std::size_t channel = 0; channel < 3; ++channel)
			Require(std::abs(pixel[channel] - Encode(linear[static_cast<int>(channel)])) <= 2, message);
		Require(pixel[3] == 255, "Opaque fixture output has invalid alpha.");
	}

	Json CaptureEvidence(const Capture& capture)
	{
		return {{"frameId", capture.result.frameId}, {"settingsRevision", capture.result.graphicsSettingsRevision},
			{"imageStage", static_cast<unsigned>(capture.result.imageStage)}, {"sourceEncoding", static_cast<unsigned>(capture.result.sourceColorEncoding)},
			{"deliveredEncoding", static_cast<unsigned>(capture.result.deliveredColorEncoding)}, {"backendReadback", capture.result.backendReadback},
			{"targetGeneration", capture.result.backendTarget->targetGeneration}, {"overlaysIncluded", capture.result.overlaysIncluded},
			{"width", capture.pixels.width}, {"height", capture.pixels.height},
			{"centerRgba", Pixel(capture, capture.pixels.width / 2, capture.pixels.height / 2)}, {"pngBase64", capture.result.base64Data}};
	}

	const PostProcessEffectDiagnostic& ExposureDiagnostic(const PostProcessDiagnosticsSnapshot& snapshot)
	{
		const auto found = std::find_if(snapshot.effects.begin(), snapshot.effects.end(), [](const auto& effect) {
			return effect.effect.stableId == "exposure";
		});
		Require(found != snapshot.effects.end(), "Native diagnostics omitted the exposure effect identity.");
		return *found;
	}

	Json DiagnosticEvidence(const PostProcessDiagnosticsSnapshot& snapshot)
	{
		const auto& effect = ExposureDiagnostic(snapshot);
		return {{"frameId", snapshot.frameId}, {"viewId", snapshot.view.outputId},
			{"settingsRevision", snapshot.settingsRevision}, {"effect", effect.effect.stableId},
			{"state", static_cast<unsigned>(effect.state)}, {"reason", static_cast<unsigned>(effect.reason)},
			{"requestId", effect.requestId}, {"hasMeasuredGpuDuration", HasMeasuredPostProcessGpuDuration(effect.timing)}};
	}

	Json RunCase(ContextOwner& owner, std::uint64_t& frame, RenderViewType type,
		float exposure, bool dedicated, bool runtime, glm::uvec2 extent, bool rawPreview = false)
	{
		if(owner.editor && type == RenderViewType::Scene) owner.context.SetSceneViewRawPreview(rawPreview);
		owner.context.BeginFrame();
		const auto outputId = dedicated ? ResolveDedicatedCaptureViewIdentity(type).outputId : (type == RenderViewType::Scene ? 1U : 2U);
		const auto kind = dedicated ? RenderOutputKind::OffscreenTexture : (type == RenderViewType::Scene ? RenderOutputKind::SceneView : RenderOutputKind::GameView);
		std::vector<RenderViewPresentationInput> views;
		views.push_back(View(owner.context, frame, type, kind, outputId, extent, exposure, type == RenderViewType::Game));
		if(runtime)
		{
			auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
			int width = 0, height = 0;
			glfwGetFramebufferSize(window, &width, &height);
			Require(width > 0 && height > 0, "Runtime framebuffer extent is unavailable.");
			views.push_back(View(owner.context, frame, RenderViewType::Game, RenderOutputKind::MainRuntimeView, 2,
				{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, exposure, true));
		}
		const auto firstView = views.front();
		// This query describes an explicitly unqualified caller view before its
		// work is accepted. It never substitutes for the rendered-view evidence.
		auto unqualifiedView = firstView;
		auto unqualifiedSettings = std::make_shared<GraphicsSettingsSnapshot>(*firstView.graphicsSettings);
		unqualifiedSettings->postProcessResourceGeneration = 0;
		unqualifiedView.graphicsSettings = std::move(unqualifiedSettings);
		const auto unavailable = owner.context.GetPostProcessDiagnostics(unqualifiedView);
		const auto& unavailableEffect = ExposureDiagnostic(unavailable);
		Require(unavailableEffect.state == PostProcessEffectState::Unavailable
			&& unavailableEffect.reason == PostProcessDiagnosticReason::MissingResources
			&& !unavailableEffect.timing.durationMilliseconds,
			"Unqualified pre-submit view received active or fabricated timing diagnostics.");
		Draw(owner, Publication(owner.context, frame, std::move(views)));
		if(owner.editor && type == RenderViewType::Scene)
		{
			const auto preview = owner.context.GetSceneViewPostProcessPreview();
			Require(preview.postProcessActive && preview.effectiveRaw == rawPreview
				&& preview.frameId == frame && preview.extent == extent,
				"Scene preview status does not match the actual accepted output.");
		}
		const auto suffix = std::to_string(frame);
		const auto raw = Read(owner.context, "raw-" + suffix, type, dedicated, frame, extent, RenderCaptureImageStage::Raw);
		const auto processed = Read(owner.context, "processed-" + suffix, type, dedicated, frame, extent, RenderCaptureImageStage::Processed);
		const auto composited = Read(owner.context, "composited-" + suffix, type, dedicated, frame, extent, RenderCaptureImageStage::Composited);
		Require(raw.result.backendTarget->targetGeneration == processed.result.backendTarget->targetGeneration,
			"Raw and processed stages belong to different processing target generations.");
		const auto sampleX = extent.x / 4, sampleY = extent.y / 4;
		const auto rawPixel = Pixel(raw, sampleX, sampleY);
		const auto processedPixel = Pixel(processed, sampleX, sampleY);
		Require(rawPixel[0] + rawPixel[1] + rawPixel[2] > 0, "Fixture source must contain actual nonzero pixels.");
		if(type == RenderViewType::Scene)
		{
			ExpectColor(rawPixel, {0.36f, 0.43f, 0.50f}, "Raw stage changed the orthographic source clear.");
			ExpectColor(processedPixel, glm::vec3(0.36f, 0.43f, 0.50f) * std::exp2(exposure), "Fixed exposure did not produce the expected pixel values.");
			const auto& selected = rawPreview ? raw : processed;
			Require(selected.pixels.rgba == composited.pixels.rgba,
				"Unadorned Scene composition does not match the selected preview stage.");
			Require(raw.pixels.rgba != processed.pixels.rgba, "Scene processing did not differ from raw camera output.");
		}
		else
		{
			Require(rawPixel != processedPixel, "Nonzero fixed exposure did not change the procedural camera image.");
			ExpectColor(Pixel(composited, extent.x / 2, extent.y / 2), {0.2f, 0.4f, 0.1f}, "HUD was affected by camera exposure or failed to render.");
			Require(composited.result.overlaysIncluded, "Composited Game output did not report its actual HUD contribution.");
			Require(Pixel(processed, extent.x / 2, extent.y / 2) != Pixel(composited, extent.x / 2, extent.y / 2), "Processed camera stage contains the HUD.");
		}
		auto unsupported = *firstView.graphicsSettings;
		unsupported.values.push_back({GraphicsSettingId::AntiAliasingMode, std::uint32_t{999}});
		std::string rejectedReason;
		Require(!owner.context.PrepareFixedExposureView(firstView, unsupported, rejectedReason) && !rejectedReason.empty(), "Unsupported plan did not reject preparation.");
		const auto preserved = Read(owner.context, "preserved-" + suffix, type, dedicated, frame, extent, RenderCaptureImageStage::Processed);
		Require(preserved.pixels.rgba == processed.pixels.rgba, "Rejected preparation changed the prior usable output.");

		// Reuse each physical frame slot through real empty-frame completions. This
		// settles submitted query owners without replacing the captured view image.
		for(int step = 0; step < 3; ++step) { owner.context.BeginFrame(); FinishFrame(owner); }
		Json timings = Json::array();
		bool capturedViewTimed = false;
		bool mainRuntimeViewTimed = false;
		for(const auto& sample : owner.context.GetCompletedPostProcessTimings())
		{
			if(sample.owner.frameId != frame) continue;
			Require(HasMeasuredPostProcessGpuDuration(sample), "Actual effect timing is missing or lacks completed submission proof.");
			capturedViewTimed |= sample.owner.view.outputId == outputId && sample.owner.view.viewType == type;
			mainRuntimeViewTimed |= sample.owner.view.outputKind == RenderOutputKind::MainRuntimeView;
			timings.push_back({{"frameId", sample.owner.frameId}, {"viewId", sample.owner.view.outputId},
				{"outputKind", static_cast<unsigned>(sample.owner.view.outputKind)},
				{"settingsRevision", sample.owner.settingsRevision}, {"submitSerial", sample.submitSerial}, {"completionSerial", sample.completionSerial},
				{"milliseconds", *sample.durationMilliseconds}, {"targetGeneration", sample.owner.targetGeneration}});
		}
		Require(capturedViewTimed, "Captured view has no actual completed exposure GPU timestamp.");
		Require(!runtime || mainRuntimeViewTimed, "Main runtime output has no actual completed exposure GPU timestamp.");
		const auto active = owner.context.GetPostProcessDiagnostics(firstView);
		const auto& activeEffect = ExposureDiagnostic(active);
		Require(active.frameId == frame && active.settingsRevision == firstView.graphicsSettings->revision
			&& active.view.outputId == outputId && activeEffect.state == PostProcessEffectState::Active
			&& HasMeasuredPostProcessGpuDuration(activeEffect.timing)
			&& activeEffect.timing.owner.frameId == frame
			&& activeEffect.timing.owner.settingsRevision == active.settingsRevision,
			"Active diagnostics do not match the accepted view and completed GPU work.");

		// Exercise the optional caller-correlated status using an actual local
		// controller rejection. This request never becomes renderer settings and
		// cannot borrow the active view's measured timing as its own cost.
		GraphicsSettingsController rejectedController;
		GraphicsSettingDescriptor descriptor;
		descriptor.id = GraphicsSettingId::FixedExposureCompensation;
		descriptor.valueKind = GraphicsValueKind::Scalar;
		descriptor.defaultValue = 0.0f;
		descriptor.gameScope = true;
		descriptor.available = true;
		GraphicsPreparationContext rejectionContext;
		rejectionContext.lifecycle = firstView.graphicsSettings->lifecycle;
		rejectionContext.descriptors = std::span<const GraphicsSettingDescriptor>(&descriptor, 1);
		rejectionContext.frameId = frame;
		GraphicsSettingsRequest invalidRequest;
		invalidRequest.lifecycle = rejectionContext.lifecycle;
		invalidRequest.patches.push_back({GraphicsSettingId::FixedExposureCompensation, std::numeric_limits<float>::quiet_NaN()});
		const auto admission = rejectedController.Enqueue(std::move(invalidRequest));
		Require(!rejectedController.Prepare(rejectionContext), "Invalid local request unexpectedly produced a candidate.");
		const auto rejectedReceipt = rejectedController.GetReceipt(admission.requestId);
		Require(rejectedReceipt && rejectedReceipt->state == GraphicsRequestState::Rejected
			&& rejectedReceipt->reason == GraphicsRequestReason::InvalidValue,
			"Local controller did not produce the actual rejection receipt.");
		const auto correlatedRejection = owner.context.GetPostProcessDiagnostics(firstView, &*rejectedReceipt);
		const auto& rejectedEffect = ExposureDiagnostic(correlatedRejection);
		Require(rejectedEffect.state == PostProcessEffectState::Rejected
			&& rejectedEffect.requestId == admission.requestId && !rejectedEffect.timing.durationMilliseconds,
			"Caller-correlated rejection reused active GPU timing or lost its request identity.");
		Json result{{"frameId", frame}, {"viewType", static_cast<unsigned>(type)}, {"runtimeMainViewRendered", runtime},
			{"dedicatedCapture", dedicated}, {"exposureEV", exposure}, {"sceneRawPreview", rawPreview}, {"raw", CaptureEvidence(raw)},
			{"processed", CaptureEvidence(processed)}, {"composited", CaptureEvidence(composited)},
			{"unsupportedPreparationRejected", true}, {"rejectionReason", rejectedReason}, {"priorOutputPreserved", true}, {"timings", timings},
			{"preSubmitUnqualifiedDiagnostics", DiagnosticEvidence(unavailable)}, {"acceptedDiagnostics", DiagnosticEvidence(active)},
			{"callerCorrelatedRejection", {{"provenance", "Actual separate local controller rejection; request never submitted to renderer"},
				{"diagnostics", DiagnosticEvidence(correlatedRejection)}}}};
		++frame;
		return result;
	}

	// Full-chain cases retain the same native preparation/publication/capture
	// path as fixed exposure. The uniform Scene clear provides an independently
	// calculable reference; the Game case proves HUD composition stays downstream.
	Json RunRichColorCase(ContextOwner& owner, std::uint64_t& frame, bool fullChain, bool automatic, bool runtime,
		ToneMappingMode toneMode = ToneMappingMode::Filmic)
	{
		const auto type = runtime ? RenderViewType::Game : RenderViewType::Scene;
		const bool dedicated = runtime;
		const glm::uvec2 extent = fullChain && !runtime ? glm::uvec2{321,181} : kExtent;
		const auto outputId = dedicated ? ResolveDedicatedCaptureViewIdentity(type).outputId : 1u;
		const auto kind = dedicated ? RenderOutputKind::OffscreenTexture : RenderOutputKind::SceneView;
		const float compensation = fullChain ? 0.5f : 0.0f;
		std::vector<GraphicsSettingValue> values{
			{GraphicsSettingId::ExposureMode,std::uint32_t{fullChain ? (automatic ? 2u : 1u) : 0u}},
			{GraphicsSettingId::ToneMappingMode,static_cast<std::uint32_t>(toneMode)},
			{GraphicsSettingId::BloomEnabled,fullChain},
			{GraphicsSettingId::BloomThreshold,0.05f},
			{GraphicsSettingId::BloomIntensity,0.25f},
			{GraphicsSettingId::BloomSpread,1.0f},
			{GraphicsSettingId::BloomSoftKnee,0.0f},
			{GraphicsSettingId::ColorGradingEnabled,fullChain},
			{GraphicsSettingId::ColorTemperatureKelvin,6500.0f},
			{GraphicsSettingId::ColorTint,0.0f},
			{GraphicsSettingId::ColorContrast,1.0f},
			{GraphicsSettingId::ColorSaturation,1.0f},
			{GraphicsSettingId::ColorLift,0.02f},
			{GraphicsSettingId::ColorGain,1.1f}};
		if(automatic)
		{
			// A clamped zero automatic EV keeps the full-chain image oracle exact
			// while exercising actual histogram, resolve, and application dispatches.
			values.push_back({GraphicsSettingId::AutoExposureMinEV,0.0f});
			values.push_back({GraphicsSettingId::AutoExposureMaxEV,0.0f});
			values.push_back({GraphicsSettingId::AutoExposureBrightenSpeed,3.0f});
			values.push_back({GraphicsSettingId::AutoExposureDarkenSpeed,2.0f});
		}
		if(owner.editor) owner.context.SetSceneViewRawPreview(false);
		owner.context.BeginFrame();
		std::vector<RenderViewPresentationInput> views;
		views.push_back(View(owner.context,frame,type,kind,outputId,extent,compensation,runtime,false,values));
		if(runtime)
		{
			int width = 0, height = 0;
			glfwGetFramebufferSize(static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()),&width,&height);
			Require(width > 0 && height > 0,"Runtime color-chain framebuffer extent is unavailable.");
			views.push_back(View(owner.context,frame,RenderViewType::Game,RenderOutputKind::MainRuntimeView,2u,
				{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)},compensation,true,false,values));
		}
		const auto capturedView = views.front();
		Require(capturedView.graphicsSettings->postProcessResourceGeneration == owner.context.GetBackendResourceGeneration(),
			"Rich settings were not certified by successful native resource preparation.");
		Draw(owner,Publication(owner.context,frame,std::move(views)));
		const auto suffix = "rich-" + std::to_string(frame);
		const auto raw = Read(owner.context,"raw-"+suffix,type,dedicated,frame,extent,RenderCaptureImageStage::Raw);
		const auto processed = Read(owner.context,"processed-"+suffix,type,dedicated,frame,extent,RenderCaptureImageStage::Processed);
		const auto composited = Read(owner.context,"composited-"+suffix,type,dedicated,frame,extent,RenderCaptureImageStage::Composited);
		Require(raw.result.backendTarget->targetGeneration == processed.result.backendTarget->targetGeneration,
			"Rich raw and processed captures belong to different retained target generations.");
		std::size_t changedPixels = 0u;
		for(std::size_t index = 0u; index < processed.pixels.rgba.size(); index += 4u)
		{
			bool changed = false;
			for(std::size_t channel = 0u; channel != 3u; ++channel)
				changed |= raw.pixels.rgba[index+channel] != processed.pixels.rgba[index+channel];
			changedPixels += changed;
			Require(processed.pixels.rgba[index+3u] == 255u && composited.pixels.rgba[index+3u] == 255u,
				"An opaque camera acquired invalid alpha during the active color stack or its sampled composite.");
		}
		Require(changedPixels > static_cast<std::size_t>(extent.x)*extent.y/2u,
			"The active rich color stack must change a measured majority of actual camera pixels.");
		glm::vec3 expected{0.36f,0.43f,0.50f};
		if(!runtime)
		{
			ExpectColor(Pixel(raw,extent.x/4u,extent.y/4u),expected,"Rich processing changed the known raw Scene clear.");
			if(fullChain)
			{
				expected *= std::exp2(compensation);
				const float luminance = glm::dot(expected,glm::vec3{0.2126f,0.7152f,0.0722f});
				// Constant radiance passes unchanged through the normalized pyramid.
				// Its hard-threshold extraction and independent intensity are analytic.
				expected += expected * (std::max(luminance-0.05f,0.0f)/luminance) * 0.25f;
			}
			if(toneMode == ToneMappingMode::GT7)
			{
				// The GT7 comparison uses the same fixed zero EV clear as Filmic.
				Require(!fullChain, "The GT7 Scene oracle assumes independent tone mapping.");
				expected = {0.141828f, 0.170664f, 0.198972f};
			}
			else for(glm::length_t channel = 0; channel != 3; ++channel)
			{
				const float radiance = expected[channel];
				expected[channel] = std::clamp((radiance*(2.51f*radiance+0.03f))/(radiance*(2.43f*radiance+0.59f)+0.14f),0.0f,1.0f);
			}
			if(fullChain) expected = expected * 1.1f + glm::vec3{0.02f};
			ExpectColor(Pixel(processed,extent.x/4u,extent.y/4u),expected,"The actual tone/bloom/grade chain disagrees with its known linear reference and one capture transfer.");
			Require(processed.pixels.rgba == composited.pixels.rgba,"Scene composition altered the completed rich camera image.");
		}
		else
		{
			ExpectColor(Pixel(composited,extent.x/2u,extent.y/2u),{0.2f,0.4f,0.1f},"Rich camera color processing changed the downstream HUD's known color.");
			Require(composited.result.overlaysIncluded && Pixel(processed,extent.x/2u,extent.y/2u) != Pixel(composited,extent.x/2u,extent.y/2u),
				"The completed rich camera stage contains the HUD or composition omitted its native overlay provenance.");
		}
		for(int step = 0; step != 3; ++step) { owner.context.BeginFrame(); FinishFrame(owner); }
		Json timings = Json::array();
		std::set<std::string> capturedEffects;
		bool runtimeTimed = false;
		for(const auto& sample : owner.context.GetCompletedPostProcessTimings())
		{
			if(sample.owner.frameId != frame) continue;
			Require(HasMeasuredPostProcessGpuDuration(sample) && *sample.durationMilliseconds > 0.0,
				"Rich color timing lacks actual positive completed native query evidence.");
			Require(sample.owner.lifecycle == capturedView.graphicsSettings->lifecycle && sample.owner.settingsRevision == frame,
				"Rich timing was attributed to a different renderer lifetime or settings revision.");
			if(sample.owner.view.outputId == outputId && sample.owner.view.outputKind == kind && sample.owner.view.viewType == type)
				capturedEffects.insert(sample.effect.stableId);
			runtimeTimed |= sample.owner.view.outputKind == RenderOutputKind::MainRuntimeView;
			timings.push_back({{"effect",sample.effect.stableId},{"frameId",sample.owner.frameId},{"outputId",sample.owner.view.outputId},
				{"outputKind",static_cast<unsigned>(sample.owner.view.outputKind)},{"settingsRevision",sample.owner.settingsRevision},
				{"submitSerial",sample.submitSerial},{"completionSerial",sample.completionSerial},{"queryPoolGeneration",sample.owner.queryPoolGeneration},
				{"milliseconds",*sample.durationMilliseconds}});
		}
		Require(capturedEffects.contains("color-chain") && capturedEffects.contains("tone-and-grade"),
			"Rich capture omitted its actual aggregate color and tone/grade GPU intervals.");
		if(fullChain) Require(capturedEffects.contains("exposure") && capturedEffects.contains("bloom"),
			"The enabled rich stack omitted separate actual exposure or bloom GPU intervals.");
		Require(!runtime || runtimeTimed,"The full runtime color output omitted completed GPU timing.");
		Json result{{"status","Passed"},{"kind",fullChain ? (automatic ? "automatic-bloom-grade" : "manual-bloom-grade") : "tone-only"},
			{"executionMode",runtime ? "runtime-and-dedicated-capture" : "editor-scene"},{"frameId",frame},
			{"toneMappingMode",static_cast<std::uint32_t>(toneMode)},
			{"fixedExposureEV",compensation},{"automaticEVBounds",automatic ? Json::array({0.0f,0.0f}) : Json(nullptr)},
			{"knownSceneExpectedLinear",runtime ? Json(nullptr) : Json::array({expected.x,expected.y,expected.z})},
			{"changedPixelCount",changedPixels},{"pixelCount",static_cast<std::size_t>(extent.x)*extent.y},
			{"opaqueAlphaPreserved",true},{"hudUnaffected",runtime},{"raw",CaptureEvidence(raw)},
			{"processed",CaptureEvidence(processed)},{"composited",CaptureEvidence(composited)},{"timings",timings}};
		++frame;
		return result;
	}

	// A native camera target exposes raw, processed and composed stages. These
	// comparisons verify the ordering contract and the bounded SDR dither step.
	void RunAtmosphereAndDitherCases(ContextOwner& owner, std::uint64_t& frame, Json& rows)
	{
		// Compare the same procedural sky and HUD across independent switches.
		std::optional<Capture> baseline;
		for(const std::string_view name : {"off", "fog", "zero-fog", "shafts", "dither"})
		{
			rows.push_back({{"case", name}, {"frameId", frame}, {"status", "Incomplete"}});
			auto& row = rows.back();
			const bool fog = name == "fog" || name == "zero-fog";
			const bool dither = name == "dither";
			const std::array values{
				// Keep the capture stages available at an identity exposure.
				GraphicsSettingValue{GraphicsSettingId::ExposureMode, std::uint32_t{1}},
				GraphicsSettingValue{GraphicsSettingId::VolumetricFogEnabled, fog},
				GraphicsSettingValue{GraphicsSettingId::VolumetricFogDensity, name == "zero-fog" ? 0.0f : 0.02f},
				GraphicsSettingValue{GraphicsSettingId::SunShaftsEnabled, name == "shafts"},
				GraphicsSettingValue{GraphicsSettingId::SunShaftsIntensity, 1.0f},
				GraphicsSettingValue{GraphicsSettingId::OutputDitheringEnabled, dither}};
			owner.context.BeginFrame();
			auto view = View(owner.context, frame, RenderViewType::Game, RenderOutputKind::GameView, 2u, kExtent, 0.0f, true, false, values);
			view.projectionMatrix = glm::perspectiveRH_ZO(glm::radians(70.0f), float(kExtent.x) / float(kExtent.y), 0.1f, 1000.0f);
			view.projectionMatrix[1][1] *= -1.0f;
			view.sky.sunDirection = {0.0f, 0.0f, -1.0f};
			Draw(owner, Publication(owner.context, frame, {std::move(view)}));
			const auto suffix = "atmosphere-" + std::string(name);
			const auto raw = Read(owner.context, suffix + "-raw", RenderViewType::Game, false, frame, kExtent, RenderCaptureImageStage::Raw);
			const auto processed = Read(owner.context, suffix + "-finished", RenderViewType::Game, false, frame, kExtent, RenderCaptureImageStage::Processed);
			const auto composited = Read(owner.context, suffix + "-composited", RenderViewType::Game, false, frame, kExtent, RenderCaptureImageStage::Composited);
			const bool atmosphere = name == "fog" || name == "shafts";
			row["processedChanged"] = raw.pixels.rgba != processed.pixels.rgba;
			Require((raw.pixels.rgba != processed.pixels.rgba) == atmosphere, "Only active fog or shafts should change the processed HDR pixels.");
			ExpectColor(Pixel(composited, kExtent.x / 2u, kExtent.y / 2u), {0.2f, 0.4f, 0.1f}, "Atmosphere/dithering changed the later HUD color.");
			if(name == "off") baseline = composited;
			std::size_t changed = 0;
			if(dither)
			{
				for(std::size_t index = 0; index < composited.pixels.rgba.size(); ++index)
				{
					const auto difference = std::abs(int(composited.pixels.rgba[index]) - int(baseline->pixels.rgba[index]));
					Require(difference <= (index % 4u == 3u ? 0 : 1), "Dither must stay within one displayed code value and preserve alpha.");
					changed += difference != 0;
				}
				Require(changed > 0, "SDR dither produced no measurable code-value changes.");
			}
			const auto timings = owner.context.GetCompletedPostProcessTimings();
			const auto measured = std::find_if(timings.begin(), timings.end(), [&](const auto& sample) {
				return sample.owner.frameId == frame && sample.availability == PostProcessTimingAvailability::Measured
					&& sample.effect.stableId == (dither ? "output-dither" : "volumetric-fog-and-shafts");
			});
			Require((measured != timings.end()) == (atmosphere || dither), "Only the active native effect may publish a GPU timing.");
			row["changedDitherChannels"] = changed;
			row["gpuMilliseconds"] = measured != timings.end() ? measured->durationMilliseconds.value_or(0.0) : 0.0;
			row["status"] = "Passed";
			++frame;
		}
	}

	void RunRuntimeDitherCase(ContextOwner& owner, std::uint64_t& frame, Json& rows)
	{
		// The same setting binds a real swapchain output and a dedicated float
		// camera. Only the swapchain has an 8-bit quantization boundary.
		rows.push_back({{"case", "runtime-dither"}, {"frameId", frame}, {"status", "Incomplete"}});
		auto& row = rows.back();
		const std::array values{GraphicsSettingValue{GraphicsSettingId::OutputDitheringEnabled, true}};
		const std::array captureValues{values.front(), GraphicsSettingValue{GraphicsSettingId::ExposureMode, std::uint32_t{1}}};
		int width = 0, height = 0;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()), &width, &height);
		Require(width > 0 && height > 0, "The SDR runtime framebuffer is unavailable.");
		owner.context.BeginFrame();
		std::vector<RenderViewPresentationInput> views;
		views.push_back(View(owner.context, frame, RenderViewType::Game, RenderOutputKind::OffscreenTexture,
			ResolveDedicatedCaptureViewIdentity(RenderViewType::Game).outputId, kExtent, 0.0f, false, false, captureValues));
		views.push_back(View(owner.context, frame, RenderViewType::Game, RenderOutputKind::MainRuntimeView, 2u,
			{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, 0.0f, true, false, values));
		Draw(owner, Publication(owner.context, frame, std::move(views)));
		const auto processed = Read(owner.context, "runtime-dither-capture-processed", RenderViewType::Game, true, frame, kExtent, RenderCaptureImageStage::Processed);
		const auto composited = Read(owner.context, "runtime-dither-capture-composited", RenderViewType::Game, true, frame, kExtent, RenderCaptureImageStage::Composited);
		Require(processed.pixels.rgba == composited.pixels.rgba, "Dedicated float output received SDR dithering.");
		const auto timings = owner.context.GetCompletedPostProcessTimings();
		bool swapchainDithered = false;
		for(const auto& timing : timings)
		{
			if(timing.owner.frameId != frame || timing.effect.stableId != "output-dither") continue;
			Require(timing.owner.view.outputKind == RenderOutputKind::MainRuntimeView && HasMeasuredPostProcessGpuDuration(timing),
				"Only the actual runtime swapchain may receive the dither draw.");
			swapchainDithered = true;
			row["gpuMilliseconds"] = timing.durationMilliseconds.value_or(0.0);
		}
		Require(swapchainDithered, "No completed dither draw on the runtime swapchain.");
		row["status"] = "Passed";
		++frame;
	}

	// Constant-radiance lens controls qualify native execution, exact bypass and
	// uncovered lens borders; they deliberately make no edge-quality claim.
	void RunPresentationLensCases(ContextOwner& owner, std::uint64_t& frame, Json& rows, bool runtime)
	{
		std::optional<Capture> baseline, previousGrain;
		const std::vector<std::string> names = runtime ? std::vector<std::string>{"runtime-hud"}
			: std::vector<std::string>{"baseline", "vignette", "grain", "grain-next-frame", "zero-amplitudes", "chromatic-edge", "distortion-edge"};
		for(const auto& name : names)
		{
			rows.push_back({{"schema", "presentation-lens-native-case-v1"}, {"status", "Incomplete"}, {"case", name},
				{"frameId", frame}, {"executionMode", runtime ? "runtime-and-dedicated-capture" : "editor-scene"}});
			auto& evidence = rows.back();
			const bool vignette = name == "vignette" || runtime, grain = name.starts_with("grain") || runtime;
			const bool zero = name == "zero-amplitudes", chromatic = name == "chromatic-edge", distortion = name == "distortion-edge";
			std::vector<GraphicsSettingValue> values{
				{GraphicsSettingId::ExposureMode, std::uint32_t{1}}, {GraphicsSettingId::ToneMappingMode, std::uint32_t{0}},
				{GraphicsSettingId::AntiAliasingMode, std::uint32_t{0}},
				{GraphicsSettingId::VignetteEnabled, vignette || zero}, {GraphicsSettingId::VignetteIntensity, zero ? 0.0f : 0.8f},
				{GraphicsSettingId::VignetteRadius, 0.2f}, {GraphicsSettingId::VignetteSoftness, 0.4f},
				{GraphicsSettingId::FilmGrainEnabled, grain || zero}, {GraphicsSettingId::FilmGrainIntensity, zero ? 0.0f : 0.2f},
				{GraphicsSettingId::FilmGrainResponse, 1.0f},
				{GraphicsSettingId::ChromaticAberrationEnabled, chromatic || zero}, {GraphicsSettingId::ChromaticAberrationPixels, zero ? 0.0f : 16.0f},
				{GraphicsSettingId::LensDistortionEnabled, distortion || zero}, {GraphicsSettingId::LensDistortionK1, zero ? 0.0f : 0.5f},
				{GraphicsSettingId::LensDistortionK2, 0.0f}};
			const auto type = runtime ? RenderViewType::Game : RenderViewType::Scene;
			const auto kind = runtime ? RenderOutputKind::OffscreenTexture : RenderOutputKind::SceneView;
			const auto outputId = runtime ? ResolveDedicatedCaptureViewIdentity(type).outputId : 1u;
			if(owner.editor) owner.context.SetSceneViewRawPreview(false);
			owner.context.BeginFrame();
			std::vector<RenderViewPresentationInput> views;
			views.push_back(View(owner.context, frame, type, kind, outputId, kExtent, 0.0f, runtime, false, values));
			if(runtime)
			{
				int width = 0, height = 0; glfwGetFramebufferSize(static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()), &width, &height);
				Require(width > 0 && height > 0, "Lens runtime framebuffer is unavailable.");
				views.push_back(View(owner.context, frame, RenderViewType::Game, RenderOutputKind::MainRuntimeView, 2u,
					{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, 0.0f, true, false, values));
			}
			const auto accepted = views.front();
			Require(accepted.graphicsSettings->revision == frame && accepted.graphicsSettings->publishedFrameId == frame
				&& accepted.graphicsSettings->postProcessResourceGeneration == owner.context.GetBackendResourceGeneration(),
				"Lens settings lack chronological successful native preparation certification.");
			evidence["resourceCertificate"] = accepted.graphicsSettings->postProcessResourceGeneration;
			evidence["settingsRevision"] = accepted.graphicsSettings->revision;
			evidence["settings"] = Json::array();
			for(const auto& value : values) evidence["settings"].push_back({{"id", static_cast<unsigned>(value.id)},
				{"value", std::visit([](const auto& v) -> Json { return v; }, value.value)}});
			Draw(owner, Publication(owner.context, frame, std::move(views)));
			const auto suffix = "presentation-lens-" + std::to_string(frame);
			const auto raw = Read(owner.context, "raw-" + suffix, type, runtime, frame, kExtent, RenderCaptureImageStage::Raw, &evidence["rawReadback"]);
			const auto processed = Read(owner.context, "processed-" + suffix, type, runtime, frame, kExtent, RenderCaptureImageStage::Processed, &evidence["processedReadback"]);
			const auto composited = Read(owner.context, "composited-" + suffix, type, runtime, frame, kExtent, RenderCaptureImageStage::Composited, &evidence["compositedReadback"]);
			evidence["raw"] = CaptureEvidence(raw); evidence["processed"] = CaptureEvidence(processed); evidence["composited"] = CaptureEvidence(composited);
			Require(raw.result.backendTarget->targetGeneration == processed.result.backendTarget->targetGeneration,
				"Lens stages differ in retained processing-target generation.");
			std::size_t darker = 0, brighter = 0, changed = 0;
			for(std::size_t offset = 0; offset < raw.pixels.rgba.size(); offset += 4)
			{
				bool pixelChanged = false;
				for(std::size_t channel = 0; channel < 3; ++channel)
				{
					darker += processed.pixels.rgba[offset + channel] < raw.pixels.rgba[offset + channel];
					brighter += processed.pixels.rgba[offset + channel] > raw.pixels.rgba[offset + channel];
					pixelChanged |= processed.pixels.rgba[offset + channel] != raw.pixels.rgba[offset + channel];
				}
				changed += pixelChanged;
				Require(processed.pixels.rgba[offset + 3] == raw.pixels.rgba[offset + 3]
					&& composited.pixels.rgba[offset + 3] == 255, "Lens processing or HUD composition changed known opaque alpha.");
			}
			evidence["changedPixels"] = changed; evidence["darkerChannels"] = darker; evidence["brighterChannels"] = brighter;
			if(!runtime)
			{
				if(baseline) Require(raw.pixels.rgba == baseline->pixels.rgba, "A lens-only setting changed the raw Scene source.");
				if(name == "baseline")
				{
					Require(raw.pixels.rgba == processed.pixels.rgba, "The fixed0EV lens-disabled baseline is not exact.");
					baseline = processed;
				}
				else if(zero) Require(baseline && processed.pixels.rgba == baseline->pixels.rgba,
					"All-enabled zero lens amplitudes differ from the same-build bypass pixels.");
				else Require(changed > 0, "An enabled nonzero independent lens control changed no pixels.");
				if(vignette)
				{
					Require(darker > 0 && brighter == 0, "Independent vignette did not only darken its source.");
					Require(Pixel(processed, kExtent.x/2, kExtent.y/2) == Pixel(raw, kExtent.x/2, kExtent.y/2), "Vignette changed its unaffected optical center.");
				}
				if(grain)
				{
					Require(darker > 0 && brighter > 0, "Independent grain lacks both signed noise contributions.");
					if(previousGrain) Require(previousGrain->pixels.rgba != processed.pixels.rgba, "Actual consecutive frame grain seed did not vary its pixels.");
					previousGrain = processed;
				}
				if(chromatic)
				{
					const auto edge = Pixel(processed, 0, kExtent.y/2), reference = Pixel(raw, 0, kExtent.y/2);
					Require(edge[0] == 0 && edge[1] == reference[1] && edge[2] == reference[2], "Chromatic edge coverage did not retain only its in-image channels.");
					evidence["coverageOnlyNoEdgeQualityClaim"] = true;
				}
				if(distortion)
				{
					const auto corner = Pixel(processed, 0, 0);
					Require(corner == std::array<int,4>{0,0,0,255}, "Distortion stretched border color instead of exposing uncovered lens space.");
					ExpectColor(Pixel(processed, kExtent.x/2, kExtent.y/2), {0.36f,0.43f,0.50f}, "Distortion changed its constant optical center.");
					evidence["coverageOnlyNoEdgeQualityClaim"] = true;
				}
				Require(processed.pixels.rgba == composited.pixels.rgba, "Lens Scene composition changed the no-overlay result.");
			}
			else
			{
				ExpectColor(Pixel(composited,kExtent.x/2,kExtent.y/2), {0.2f,0.4f,0.1f}, "Active lens styling changed the downstream native HUD.");
				Require(composited.result.overlaysIncluded && !processed.result.overlaysIncluded,
					"Lens HUD capture omitted actual independent overlay-stage provenance.");
			}
			for(int step = 0; step < 3; ++step) { owner.context.BeginFrame(); FinishFrame(owner); }
			bool lensTimed = false, runtimeTimed = false; evidence["timings"] = Json::array();
			for(const auto& sample : owner.context.GetCompletedPostProcessTimings())
			{
				if(sample.owner.frameId != frame || sample.effect.stableId != "presentation-lens") continue;
				Require(HasMeasuredPostProcessGpuDuration(sample) && sample.owner.settingsRevision == frame,
					"Lens timing lacks actual completed positive query/chronological revision evidence.");
				if(sample.owner.view.outputKind == kind && sample.owner.view.viewType == type && sample.owner.view.outputId == outputId)
				{
					Require(sample.owner.lifecycle == accepted.graphicsSettings->lifecycle
						&& sample.owner.targetGeneration == raw.result.backendTarget->targetGeneration,
						"Lens GPU timing belongs to a different prepared lifetime or processing target.");
					lensTimed = true;
				}
				runtimeTimed |= sample.owner.view.outputKind == RenderOutputKind::MainRuntimeView;
				evidence["timings"].push_back({{"effect",sample.effect.stableId},{"frameId",sample.owner.frameId},{"settingsRevision",sample.owner.settingsRevision},
					{"outputId",sample.owner.view.outputId},{"outputKind",static_cast<unsigned>(sample.owner.view.outputKind)},
					{"targetGeneration",sample.owner.targetGeneration},{"submitSerial",sample.submitSerial},{"completionSerial",sample.completionSerial},
					{"queryPoolGeneration",sample.owner.queryPoolGeneration},{"milliseconds",*sample.durationMilliseconds}});
			}
			if(name != "baseline" && !zero) Require(lensTimed, "An enabled lens stage lacks its separate completed GPU interval.");
			else Require(!lensTimed, "Disabled or zero-amplitude lens bypass recorded an active lens interval.");
			Require(!runtime || runtimeTimed, "Actual main runtime lens work omitted its completed GPU interval.");
			evidence["alphaPreserved"] = true; evidence["zeroAmplitudeExact"] = zero;
			evidence["hudUnaffected"] = runtime; evidence["status"] = "Passed";
			++frame;
		}
	}

	double DecodedLinearLuminance(const Capture& capture)
	{
		Require(capture.pixels.width >= 9u && capture.pixels.height >= 9u,"Temporal exposure needs a nonempty central sampling patch.");
		const auto decode = [](int channel) {
			const double encoded = static_cast<double>(channel)/255.0;
			return encoded <= 0.04045 ? encoded/12.92 : std::pow((encoded+0.055)/1.055,2.4);
		};
		double total = 0.0;
		const auto centerX = capture.pixels.width/2u, centerY = capture.pixels.height/2u;
		for(auto y = centerY-4u; y <= centerY+4u; ++y)
			for(auto x = centerX-4u; x <= centerX+4u; ++x)
			{
				const auto pixel = Pixel(capture,x,y);
				Require(pixel[3] == 255,"Temporal exposure changed the opaque sky alpha.");
				for(unsigned channel = 0u; channel != 3u; ++channel)
					Require(pixel[channel] > 0 && pixel[channel] < 250,
						"Temporal pixel ratios require actual nonblack, unclipped PNG samples; this view cannot provide a reliable EV measurement.");
				total += decode(pixel[0])*0.2126 + decode(pixel[1])*0.7152 + decode(pixel[2])*0.0722;
			}
		return total/81.0;
	}

	Json TemporalCaptureStatistics(const Capture& capture)
	{
		std::array<int,4> minimum{255,255,255,255}, maximum{};
		for(std::size_t index = 0u; index < capture.pixels.rgba.size(); index += 4u)
			for(std::size_t channel = 0u; channel != 4u; ++channel)
			{
				const int value = capture.pixels.rgba[index+channel];
				minimum[channel] = std::min(minimum[channel],value); maximum[channel] = std::max(maximum[channel],value);
			}
		Json result{{"globalMinimumRgba",minimum},{"globalMaximumRgba",maximum},
			{"sourceEncoding",static_cast<unsigned>(capture.result.sourceColorEncoding)},
			{"deliveredEncoding",static_cast<unsigned>(capture.result.deliveredColorEncoding)},
			{"frameId",capture.result.frameId},{"imageStage",static_cast<unsigned>(capture.result.imageStage)}};
		if(capture.pixels.width < 9u || capture.pixels.height < 9u) { result["patchAvailable"] = false; return result; }
		std::array<int,4> patchMinimum{255,255,255,255}, patchMaximum{};
		std::size_t blackChannels = 0u, clippedChannels = 0u;
		double luminance = 0.0;
		const auto decode = [](int channel) {
			const double encoded = static_cast<double>(channel)/255.0;
			return encoded <= 0.04045 ? encoded/12.92 : std::pow((encoded+0.055)/1.055,2.4);
		};
		const auto centerX = capture.pixels.width/2u, centerY = capture.pixels.height/2u;
		for(auto y = centerY-4u; y <= centerY+4u; ++y)
			for(auto x = centerX-4u; x <= centerX+4u; ++x)
			{
				const auto pixel = Pixel(capture,x,y);
				for(std::size_t channel = 0u; channel != 4u; ++channel)
				{
					patchMinimum[channel] = std::min(patchMinimum[channel],pixel[channel]);
					patchMaximum[channel] = std::max(patchMaximum[channel],pixel[channel]);
					if(channel < 3u) { blackChannels += pixel[channel] == 0; clippedChannels += pixel[channel] >= 250; }
				}
				luminance += decode(pixel[0])*0.2126+decode(pixel[1])*0.7152+decode(pixel[2])*0.0722;
			}
		result["patchAvailable"] = true; result["patchMinimumRgba"] = patchMinimum; result["patchMaximumRgba"] = patchMaximum;
		result["patchBlackRgbChannelCount"] = blackChannels; result["patchClippedRgbChannelCount"] = clippedChannels;
		result["patchMeanDecodedLinearLuminance"] = luminance/81.0;
		return result;
	}

	void RunTemporalAutoExposure(ContextOwner& owner, std::uint64_t& frame, Json& evidence)
	{
		// Publish progress directly into the retained root. A later assertion must
		// not discard already captured native images and their measured ranges.
		evidence = {{"status","Running"},{"completed",false},{"trace",Json::array()},
			{"scope","ActualAutoExposureAdapterAcrossCompletedGameFrames"},
			{"source","Equal 0.4 procedural sky tints; zenith sun outside the camera; sky exposure 0.5"}};
		Require(owner.editor,"Temporal fixture requires the editor's two independent offscreen view outputs.");
		owner.context.SetSceneViewRawPreview(false);
		constexpr float deltaSeconds = 0.125f, darkIntensity = 0.4f, brightIntensity = 1.2f;
		const glm::uvec2 initialExtent{321u,181u}, resizedExtent{385u,217u};
		const std::vector<GraphicsSettingValue> values{
			{GraphicsSettingId::ExposureMode,std::uint32_t{2u}},
			{GraphicsSettingId::AutoExposureMinEV,-8.0f},{GraphicsSettingId::AutoExposureMaxEV,8.0f},
			{GraphicsSettingId::AutoExposureBrightenSpeed,2.0f},{GraphicsSettingId::AutoExposureDarkenSpeed,1.0f},
			{GraphicsSettingId::ToneMappingMode,std::uint32_t{0u}},
			{GraphicsSettingId::BloomEnabled,false},{GraphicsSettingId::ColorGradingEnabled,false}};
		struct Observation
		{
			double rawLuminance = 0.0, processedLuminance = 0.0, exposureEV = 0.0;
			std::uint64_t generation = 0u;
		};
		std::optional<Capture> isolatedBaseline;
		auto& trace = evidence["trace"];
		const auto render = [&](std::string_view phase, float intensity, glm::uvec2 extent) {
			trace.push_back({{"status","Preparing"},{"phase",phase},{"frameId",frame},{"deltaSeconds",deltaSeconds},
				{"skyIntensity",intensity},{"extent",{extent.x,extent.y}}});
			auto& row = trace.back();
			owner.context.BeginFrame();
			auto game = View(owner.context,frame,RenderViewType::Game,RenderOutputKind::GameView,2u,extent,0.0f,false,false,values);
			// Equal day tints and a sun above the camera eliminate the previous
			// center-facing solar highlight. The normal procedural sky shader still
			// produces and changes the actual source; no GPU exposure state is set.
			game.sky.horizonTint = game.sky.zenithTint = game.sky.groundTint = glm::vec3{0.4f};
			game.sky.sunDirection = {0.0f,1.0f,0.0f}; game.sky.exposure = 0.5f;
			game.sky.intensity = intensity; game.deltaTimeSeconds = deltaSeconds;
			auto scene = View(owner.context,frame,RenderViewType::Scene,RenderOutputKind::SceneView,1u,kExtent,0.0f,false,false,values);
			scene.deltaTimeSeconds = deltaSeconds;
			Draw(owner,Publication(owner.context,frame,{std::move(game),std::move(scene)}));
			const auto suffix = "temporal-"+std::to_string(frame);
			row["status"] = "Rendered";
			const auto raw = Read(owner.context,"raw-"+suffix,RenderViewType::Game,false,frame,extent,RenderCaptureImageStage::Raw,&row["rawReadback"]);
			row["raw"] = CaptureEvidence(raw); row["rawStatistics"] = TemporalCaptureStatistics(raw);
			const auto processed = Read(owner.context,"processed-"+suffix,RenderViewType::Game,false,frame,extent,RenderCaptureImageStage::Processed,&row["processedReadback"]);
			row["processed"] = CaptureEvidence(processed); row["processedStatistics"] = TemporalCaptureStatistics(processed);
			for(std::size_t channel = 0u; channel != 3u; ++channel)
				Require(row["rawStatistics"]["globalMaximumRgba"][channel].get<int>()
					-row["rawStatistics"]["globalMinimumRgba"][channel].get<int>() <= 1,
					"The procedural temporal source is not uniform across the measured camera image.");
			const auto isolated = Read(owner.context,"scene-"+suffix,RenderViewType::Scene,false,frame,kExtent,RenderCaptureImageStage::Processed,&row["constantSceneReadback"]);
			row["constantSceneStatistics"] = TemporalCaptureStatistics(isolated); row["status"] = "Captured";
			if(!isolatedBaseline) isolatedBaseline = isolated;
			row["constantScenePixelsEqual"] = isolated.pixels.rgba == isolatedBaseline->pixels.rgba;
			row["constantSceneTargetGeneration"] = isolated.result.backendTarget->targetGeneration;
			row["constantSceneGenerationEqual"] = isolated.result.backendTarget->targetGeneration == isolatedBaseline->result.backendTarget->targetGeneration;
			Require(isolated.pixels.rgba == isolatedBaseline->pixels.rgba
				&& isolated.result.backendTarget->targetGeneration == isolatedBaseline->result.backendTarget->targetGeneration,
				"A changing or resized Game camera changed another view's constant automatic-exposure history or target.");
			Observation result;
			result.rawLuminance = DecodedLinearLuminance(raw);
			result.processedLuminance = DecodedLinearLuminance(processed);
			result.exposureEV = std::log2(result.processedLuminance/result.rawLuminance);
			result.generation = processed.result.backendTarget->targetGeneration;
			row["rawLinearLuminance"] = result.rawLuminance; row["processedLinearLuminance"] = result.processedLuminance;
			row["estimatedExposureEV"] = result.exposureEV; row["targetGeneration"] = result.generation;
			const float expectedSource = 1.0f-std::exp(-0.4f*intensity*0.5f);
			row["knownUniformSourceLinear"] = expectedSource;
			Require(std::abs(result.rawLuminance-expectedSource) < 0.005,
				"The temporal camera did not render the requested measurable uniform procedural source.");
			Require(raw.result.backendTarget->targetGeneration == result.generation,
				"Temporal Raw and Processed measurements did not use the same actual retained target.");
			row["status"] = "Measured";
			++frame;
			return result;
		};
		const auto dark = render("initial-dark",darkIntensity,initialExtent);
		auto previous = render("stable-dark",darkIntensity,initialExtent);
		Require(previous.generation == dark.generation && std::abs(previous.exposureEV-dark.exposureEV) < 0.04,
			"A newly initialized automatic target must hold its first stable meter across actual completed frames.");
		Observation bright;
		for(unsigned step = 0u; step != 4u; ++step)
		{
			bright = render("dark-to-bright",brightIntensity,initialExtent);
			Require(bright.generation == dark.generation && bright.exposureEV < previous.exposureEV-0.005,
				"The stable native target must gradually reduce its measured exposure after the sky becomes brighter.");
			previous = bright;
		}
		Require(bright.rawLuminance > dark.rawLuminance*2.5,"The real sky transition did not produce the required measured brightness increase.");
		for(unsigned step = 0u; step != 5u; ++step)
		{
			const auto returning = render("bright-to-dark",darkIntensity,initialExtent);
			Require(returning.generation == dark.generation && returning.exposureEV > previous.exposureEV+0.005,
				"The separate brightening rate must gradually increase actual exposure when the camera returns to the darker sky.");
			Require(std::abs(returning.rawLuminance-dark.rawLuminance) < dark.rawLuminance*0.01,
				"The returning sky is not the same measured source used for the initial dark reference.");
			previous = returning;
		}
		const auto held = render("bright-before-resize",brightIntensity,initialExtent);
		const auto reset = render("bright-resized-reset",brightIntensity,resizedExtent);
		const auto stableReset = render("bright-resized-stable",brightIntensity,resizedExtent);
		Require(held.generation == dark.generation && reset.generation > held.generation && stableReset.generation == reset.generation,
			"A real extent replacement must establish a fresh native target generation and retain it for the next frame.");
		Require(reset.exposureEV < held.exposureEV-0.25 && std::abs(stableReset.exposureEV-reset.exposureEV) < 0.04,
			"A recreated target must discard the old adapted EV, snap to its fresh meter, and remain stable on its second completed frame.");
		evidence["status"] = "Passed"; evidence["completed"] = true;
		evidence["measurement"] = "Inverse-sRGB decoded 9x9 patch; log2(Processed linear luminance / Raw linear luminance)";
		evidence["deltaSeconds"] = deltaSeconds; evidence["brightenSpeedPerSecond"] = 2.0f; evidence["darkenSpeedPerSecond"] = 1.0f;
		evidence["monotonicDarkening"] = true; evidence["monotonicBrightening"] = true; evidence["freshResizeHistoryReset"] = true;
		evidence["constantSceneHistoryIsolated"] = true; evidence["constantSceneBaseline"] = CaptureEvidence(*isolatedBaseline);
	}

	void RunSpatialAACase(ContextOwner& owner, std::uint64_t& frame, std::uint32_t mode,
		const GraphicsCatalogCapabilities& startupCapabilities, Json& evidence)
	{
		Require(owner.editor && mode >= 1u && mode <= 3u,"Spatial native fixture needs an actual supported Scene AA mode.");
		constexpr glm::uvec2 extent{321u,181u};
		const std::array<std::string_view,4> names{"Off","FXAA","SMAA","CMAA2"};
		evidence = {{"status","Discovering"},{"mode",names[mode]},{"modeValue",mode},{"frameId",frame},
			{"scope","ActualNativeSpatialAAConstantScene"},{"movingEdgeQualityMeasured",false},
			{"extent",{extent.x,extent.y}},{"startupSpatialAAModeMask",startupCapabilities.spatialAAModeMask}};
		// Discover against the selected device before preparation. Reuse the
		// startup snapshot too, so lazy offscreen allocation cannot hide missing
		// options from the catalog seen before a game's first rendered view.
		const auto currentCapabilities = owner.context.ReadGraphicsCatalogCapabilities(false);
		evidence["currentSpatialAAModeMask"] = currentCapabilities.spatialAAModeMask;
		Require((startupCapabilities.spatialAAModeMask & (1u << mode)) != 0u
			&& (currentCapabilities.spatialAAModeMask & (1u << mode)) != 0u,
			"Selected native adapter did not advertise this spatial mode at startup and before recording.");
		const auto catalog = BuildGraphicsSettingsCatalog(RenderSettings{},true,GraphicsProfileMetadata{},GraphicsSceneSkyValues{},startupCapabilities);
		const auto* descriptor = FindGraphicsSetting(catalog,GraphicsSettingId::AntiAliasingMode);
		Require(descriptor && descriptor->available && ValidateSpatialAAValue(*descriptor,GraphicsValue{mode}) == GraphicsRequestReason::None,
			"The actual capability-derived catalog did not expose the selected spatial mode.");
		evidence["catalogModeAvailable"] = true;
		owner.context.SetSceneViewRawPreview(false);
		owner.context.BeginFrame();
		const std::array values{GraphicsSettingValue{GraphicsSettingId::AntiAliasingMode,mode}};
		auto input = View(owner.context,frame,RenderViewType::Scene,RenderOutputKind::SceneView,1u,extent,0.0f,false,false,values);
		const auto plan = BuildPostProcessGraphSettings(*input.graphicsSettings);
		Require(plan.executionAvailable && plan.spatialAA && plan.antiAliasingMode == mode
			&& !plan.hdrCamera && !plan.toneAndGrade && input.graphicsSettings->sampleCount == 1u,
			"The certified native-size single-sample spatial plan enabled the wrong mode or unrelated color work.");
		const auto acceptedInput = input;
		evidence["preparedSettingsRevision"] = input.graphicsSettings->revision;
		evidence["resourceCertificate"] = input.graphicsSettings->postProcessResourceGeneration;
		evidence["status"] = "Prepared";
		Draw(owner,Publication(owner.context,frame,{std::move(input)}));
		const auto preview = owner.context.GetSceneViewPostProcessPreview();
		Require(preview.postProcessActive && !preview.effectiveRaw && preview.frameId == frame && preview.extent == extent,
			"Scene preview did not identify its actual accepted spatial-AA output.");
		const auto suffix = "spatial-aa-"+std::to_string(mode)+"-"+std::to_string(frame);
		const auto raw = Read(owner.context,"raw-"+suffix,RenderViewType::Scene,false,frame,extent,RenderCaptureImageStage::Raw,&evidence["rawReadback"]);
		evidence["raw"] = CaptureEvidence(raw);
		const auto processed = Read(owner.context,"processed-"+suffix,RenderViewType::Scene,false,frame,extent,RenderCaptureImageStage::Processed,&evidence["processedReadback"]);
		evidence["processed"] = CaptureEvidence(processed);
		const auto composited = Read(owner.context,"composited-"+suffix,RenderViewType::Scene,false,frame,extent,RenderCaptureImageStage::Composited,&evidence["compositedReadback"]);
		evidence["composited"] = CaptureEvidence(composited);
		evidence["rawStatistics"] = TemporalCaptureStatistics(raw);
		evidence["processedStatistics"] = TemporalCaptureStatistics(processed);
		const auto session = acceptedInput.graphicsSettings->lifecycle.rendererSessionGeneration;
		VulkanContext::EditorHandles handles{};
		owner.context.GetEditorHandles(handles);
		Require(handles.sceneImageView != nullptr && handles.sceneImageGeneration != 0u,
			"Spatial composition has no actual retained Scene image owner.");
		for(const auto* capture : {&raw,&processed,&composited})
		{
			Require(capture->result.backendTarget->rendererSessionGeneration == session
				&& !capture->result.backendTarget->dedicatedCaptureTarget
				&& capture->result.sourceKind == RenderCaptureSourceKind::GameView && !capture->result.overlaysIncluded,
				"Spatial stages did not come from the matching visible Scene target without overlays.");
		}
		// Raw and Processed live in the private processing bundle; Composited
		// lives in the editor's Scene image. Their independent allocation counters
		// must match their actual owners, not each other by coincidence.
		Require(raw.result.backendTarget->targetGeneration == processed.result.backendTarget->targetGeneration,
			"Spatial Raw and Processed stages used different processing target generations.");
		Require(composited.result.backendTarget->targetGeneration == handles.sceneImageGeneration,
			"Spatial Composited stage does not belong to the actual retained Scene image generation.");
		evidence["processingTargetGeneration"] = raw.result.backendTarget->targetGeneration;
		evidence["compositionTargetGeneration"] = handles.sceneImageGeneration;
		evidence["compositionSceneImageViewPresent"] = true;
		int maximumEncodedDelta = 0;
		// The odd dimensions exercise partial compute tiles and all image borders.
		// This is constant-signal pipeline coverage, not an edge-quality score.
		for(std::uint32_t y = 0u; y != extent.y; ++y)
			for(std::uint32_t x = 0u; x != extent.x; ++x)
			{
				const auto rawPixel = Pixel(raw,x,y), processedPixel = Pixel(processed,x,y);
				ExpectColor(rawPixel,{0.36f,0.43f,0.50f},"Spatial raw capture changed the known Scene source.");
				ExpectColor(processedPixel,{0.36f,0.43f,0.50f},"Spatial AA changed a constant color or left an odd-extent border unwritten.");
				for(std::size_t channel = 0u; channel != 3u; ++channel)
					maximumEncodedDelta = std::max(maximumEncodedDelta,std::abs(rawPixel[channel]-processedPixel[channel]));
			}
		Require(maximumEncodedDelta <= 2 && processed.pixels.rgba == composited.pixels.rgba,
			"Spatial processing or its zero-EV composition failed constant preservation within native capture quantization.");
		evidence["maximumEncodedChannelDeltaFromRaw"] = maximumEncodedDelta;
		evidence["pixelCountChecked"] = static_cast<std::size_t>(extent.x)*extent.y;
		evidence["status"] = "PixelsChecked";
		for(int step = 0; step != 3; ++step) { owner.context.BeginFrame(); FinishFrame(owner); }
		Json timings = Json::array(); bool spatialMeasured = false;
		for(const auto& sample : owner.context.GetCompletedPostProcessTimings())
		{
			if(sample.owner.frameId != frame) continue;
			Require(HasMeasuredPostProcessGpuDuration(sample) && *sample.durationMilliseconds > 0.0
				&& sample.owner.lifecycle == acceptedInput.graphicsSettings->lifecycle && sample.owner.settingsRevision == frame
				&& sample.owner.view.outputKind == RenderOutputKind::SceneView && sample.owner.view.viewType == RenderViewType::Scene
				&& sample.owner.view.outputId == 1u && sample.owner.targetGeneration == raw.result.backendTarget->targetGeneration,
				"Spatial timing lacked completed native query ownership for the exact source view, frame, settings, and target.");
			spatialMeasured |= sample.effect.stableId == "spatial-aa" && sample.effect.passKind == PostProcessPassKind::SpatialAA;
			timings.push_back({{"effect",sample.effect.stableId},{"frameId",sample.owner.frameId},{"viewId",sample.owner.view.outputId},
				{"outputKind",static_cast<unsigned>(sample.owner.view.outputKind)},{"settingsRevision",sample.owner.settingsRevision},
				{"targetGeneration",sample.owner.targetGeneration},{"queryPoolGeneration",sample.owner.queryPoolGeneration},
				{"submitSerial",sample.submitSerial},{"completionSerial",sample.completionSerial},{"milliseconds",*sample.durationMilliseconds}});
		}
		evidence["timings"] = std::move(timings);
		Require(spatialMeasured,"Spatial mode has no separate positive completed spatial-aa GPU interval.");
		evidence["constantColorAndAlphaPreserved"] = true;
		evidence["nativeScenePreviewMatched"] = true;
		evidence["status"] = "Passed";
		++frame;
	}

	void RunNativeHdrCameraCases(ContextOwner& owner, std::uint64_t& frame, Json& rows)
	{
		// Begin with the actual procedural sky. Empty scene depth must remain
		// invalid after linearization, so DOF cannot smear the sky into geometry.
		for(const bool flare : {false, true})
		{
			const std::array values{
				GraphicsSettingValue{GraphicsSettingId::DepthOfFieldEnabled, !flare},
				GraphicsSettingValue{GraphicsSettingId::LensFlareEnabled, flare},
				GraphicsSettingValue{GraphicsSettingId::LensFlareThreshold, 0.0f},
				GraphicsSettingValue{GraphicsSettingId::LensFlareIntensity, 0.5f}};
			owner.context.BeginFrame();
			auto view = View(owner.context, frame, RenderViewType::Game, RenderOutputKind::GameView,
				2u, kExtent, 0.0f, true, false, values);
			Draw(owner, Publication(owner.context, frame, {std::move(view)}));
			const auto raw = Read(owner.context, "hdr-raw-" + std::to_string(frame), RenderViewType::Game,
				false, frame, kExtent, RenderCaptureImageStage::Raw);
			const auto finished = Read(owner.context, "hdr-processed-" + std::to_string(frame), RenderViewType::Game,
				false, frame, kExtent, RenderCaptureImageStage::Processed);
			const auto composited = Read(owner.context, "hdr-composited-" + std::to_string(frame), RenderViewType::Game,
				false, frame, kExtent, RenderCaptureImageStage::Composited);
			Require((raw.pixels.rgba != finished.pixels.rgba) == flare,
				"The independent flare must change HDR pixels, and sky-only DOF must preserve them.");
			ExpectColor(Pixel(composited, kExtent.x / 2u, kExtent.y / 2u),
				{0.2f, 0.4f, 0.1f}, "The HDR camera effects changed the later HUD color.");
			// The native query must resolve its own named stage on this frame.
			for(int i = 0; i < 3; ++i) { owner.context.BeginFrame(); FinishFrame(owner); }
			const std::string label = flare ? "lens-flare" : "depth-of-field";
			const auto timings = owner.context.GetCompletedPostProcessTimings();
			const auto timing = std::find_if(timings.begin(), timings.end(), [&](const auto& sample) {
				return sample.owner.frameId == frame && sample.effect.stableId == label
					&& sample.availability == PostProcessTimingAvailability::Measured;
			});
			Require(timing != timings.end(), "The named HDR camera stage has no completed GPU timestamp.");
			rows.push_back({{"effect", label}, {"frameId", frame}, {"gpuMilliseconds", timing->durationMilliseconds.value_or(0.0)},
				{"raw", CaptureEvidence(raw)},
				{"processed", CaptureEvidence(finished)}, {"hudVerified", true}});
			++frame;
		}
	}


	// Generated texture data enters through normal ResourceManager publication.
	// Its black/white spatial mask distinguishes real authored dirt sampling from
	// a constant gain, while real emissive geometry supplies bright HDR radiance.
	void RunNativeLensDirtCase(ContextOwner& owner, std::uint64_t& frame, Json& rows)
	{
		auto texture=std::make_unique<Texture>();
		texture->header.magic=kMagic;texture->header.version=kVersion;texture->header.headerSize=sizeof(TETFHeader);
		texture->descriptor.width=8;texture->descriptor.height=4;
		texture->descriptor.depth=texture->descriptor.layers=texture->descriptor.faces=texture->descriptor.mipCount=1;
		texture->descriptor.blockWidth=texture->descriptor.blockHeight=1;texture->descriptor.blockBytes=4;
		texture->descriptor.semantic=static_cast<std::uint8_t>(TextureFileSemantic::Data);
		texture->descriptor.vkFormat=VK_FORMAT_R8G8B8A8_UNORM;
		texture->transcodedData.resize(8u*4u*4u);
		for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<8;++x)
		{
			const auto offset=(y*8+x)*4;
			for(std::uint32_t channel=0;channel<3;++channel)texture->transcodedData[offset+channel]=x<4 ? 0 : 255;
			texture->transcodedData[offset+3]=255;
		}
		texture->levelTable.push_back({0,texture->transcodedData.size()});
		auto& resources=ResourceManager::Instance();
		const auto textureId=resources.RegisterTexture("native-lens-dirt/"+ToHexString(TerraEngine::GenerateUUID()),std::move(texture));
		const auto published=resources.AcquireTextureSnapshot(textureId);
		Require(textureId!=TerraEngine::EMPTY_UUID && published && published->publicationGeneration,
			"The actual linear dirt texture was not published by ResourceManager.");
		// Use genuine emissive HDR geometry: the procedural sky shader itself
		// already compresses values below one and cannot drive the dirt threshold.
		auto geometry=TerraEngine::TestSupport::CreateScreenLightingGeometry();
		auto material=TerraEngine::Terrain::CreateTerrainMaterial(resources,{glm::vec3(0.1f),0.0f,0.6f});
		material->header.emissiveFactor[0]=8.0f;material->header.emissiveFactor[1]=0.5f;material->header.emissiveFactor[2]=0.25f;
		const auto materialId=resources.RegisterMaterial("native-dirt-emitter/"+ToHexString(TerraEngine::GenerateUUID()),std::move(material));
		const auto materialOwner=resources.AcquireMaterialSnapshot(materialId);
		Require(materialOwner && materialOwner->publicationGeneration,"Real dirt HDR emitter material was not published.");
		for(auto& surface:geometry.surfaces){surface.materialId=materialId;surface.materialGeneration=materialOwner->publicationGeneration;}
		std::optional<Capture> baselineRaw,baselineProcessed;
		for(std::uint32_t mode=0;mode<3;++mode)
		{
			auto& row=rows.emplace_back(Json{{"case",mode==0 ? "disabled" : mode==1 ? "zero-intensity" : "authored-linear-mask"},
				{"textureId",ToHexString(textureId)},{"texturePublicationGeneration",published->publicationGeneration},{"status","Pending"}});
			owner.context.BeginFrame();
			const std::array values{
				GraphicsSettingValue{GraphicsSettingId::BloomEnabled,true},
				GraphicsSettingValue{GraphicsSettingId::BloomThreshold,1.0f},
				GraphicsSettingValue{GraphicsSettingId::BloomIntensity,0.2f},
				GraphicsSettingValue{GraphicsSettingId::LensDirtEnabled,mode!=0},
				GraphicsSettingValue{GraphicsSettingId::LensDirtIntensity,mode==1 ? 0.0f : 1.0f},
				GraphicsSettingValue{GraphicsSettingId::LensDirtTexture,textureId},
				GraphicsSettingValue{GraphicsSettingId::GeometryRasterMode,std::uint32_t{1}}};
			auto bootstrapValues=values;bootstrapValues[3].value=false;
			auto input=View(owner.context,frame,RenderViewType::Game,RenderOutputKind::GameView,2u,kExtent,0.0f,true,false,bootstrapValues);
			input.projectionMatrix=glm::orthoRH_ZO(-3.2f,3.2f,-1.8f,1.8f,0.1f,25.0f);
			input.projectionMatrix[1][1]*=-1.0f;
			input.graphicsView=GraphicsViewIdentity{RenderViewType::Game,2u,1u,RenderOutputKind::GameView};
			auto candidate=std::make_shared<GraphicsSettingsSnapshot>(*input.graphicsSettings);
			candidate->target.view=input.graphicsView;
			for(const auto& value:values)
			{
				const auto found=std::find_if(candidate->values.begin(),candidate->values.end(),[&](const auto& old){return old.id==value.id;});
				Require(found!=candidate->values.end(),"Lens dirt candidate lost its actual setting.");*found=value;
			}
			std::string reason;
			const bool prepared=owner.context.PrepareFixedExposureView(input,*candidate,reason);
			row["preparation"]={{"accepted",prepared},{"reason",reason}};
			Require(prepared,"Real linear dirt texture/native lens preparation failed: "+reason);
			candidate->postProcessResourceGeneration=owner.context.GetBackendResourceGeneration();input.graphicsSettings=std::move(candidate);
			const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
			for(;;)
			{
				Draw(owner,TerraEngine::TestSupport::PublishScreenLightingGeometry(geometry,input,frame,
					owner.context.GetLatestRendererRecoverySnapshot().renderSessionGeneration,owner.context.GetBackendResourceGeneration()));
				const auto native=owner.context.ReadRecordedGraphicsViews(frame);
				const auto draw=std::find_if(native.begin(),native.end(),[](const auto& value){return value.publicationIndex==0;});
				if(draw!=native.end() && draw->indexedDrawCount==geometry.surfaces.size())break;
				Require(std::chrono::steady_clock::now()<deadline,"Lens dirt HDR geometry never finished native preparation.");
				++frame;owner.context.BeginFrame();
				input.referenceFrame.frameId=frame;
				auto next=std::make_shared<GraphicsSettingsSnapshot>(*input.graphicsSettings);
				next->revision=next->publishedFrameId=frame;input.graphicsSettings=std::move(next);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			const auto raw=Read(owner.context,"dirt-raw-"+std::to_string(frame),RenderViewType::Game,false,frame,kExtent,RenderCaptureImageStage::Raw);
			const auto processed=Read(owner.context,"dirt-processed-"+std::to_string(frame),RenderViewType::Game,false,frame,kExtent,RenderCaptureImageStage::Processed);
			const auto composited=Read(owner.context,"dirt-composited-"+std::to_string(frame),RenderViewType::Game,false,frame,kExtent,RenderCaptureImageStage::Composited);
			row["frameId"]=frame;row["raw"]=CaptureEvidence(raw);row["processed"]=CaptureEvidence(processed);
			Require(raw.result.backendTarget->targetGeneration==processed.result.backendTarget->targetGeneration,
				"Lens dirt captures differ in native target lifetime.");
			ExpectColor(Pixel(composited,kExtent.x/2,kExtent.y/2),{0.2f,0.4f,0.1f},"Lens dirt changed the later HUD color.");
			if(mode==0){baselineRaw=raw;baselineProcessed=processed;}
			else
			{
				Require(raw.pixels.rgba==baselineRaw->pixels.rgba,"Lens dirt controls changed the original Raw scene.");
				Require((processed.pixels.rgba!=baselineProcessed->pixels.rgba)==(mode==2),
					mode==1 ? "Zero dirt intensity differs from disabled output." : "Real authored dirt produced no changed pixels.");
				std::uint64_t leftDelta=0,rightDelta=0,alphaChanged=0;
				for(std::uint32_t y=0;y<kExtent.y;++y)for(std::uint32_t x=0;x<kExtent.x;++x)
				{
					const auto before=Pixel(*baselineProcessed,x,y),after=Pixel(processed,x,y);
					for(std::uint32_t channel=0;channel<3;++channel)
					{
						const auto delta=std::abs(after[channel]-before[channel]);
						if(x<kExtent.x/4)leftDelta+=delta;
						if(x>=kExtent.x*3/4)rightDelta+=delta;
					}
					if(before[3]!=after[3])++alphaChanged;
				}
				row["blackMaskCodeDelta"]=leftDelta;row["whiteMaskCodeDelta"]=rightDelta;
				Require(alphaChanged==0,"Lens dirt changed the original source alpha.");
				if(mode==2)Require(leftDelta==0 && rightDelta>0,"Dirt response does not follow the actual sampled black/white texture mask.");
			}
			const auto accepted=owner.context.ReadAcceptedRenderFramePublication(frame);
			Require(accepted && accepted->presentationInputs.size()==1 && accepted->presentationInputs[0].graphicsSettings,
				"Lens dirt lost its actual accepted settings publication.");
			const auto& settings=*accepted->presentationInputs[0].graphicsSettings;
			PostProcessEffectSettings controls;
			Require(ResolvePostProcessEffectSettings(settings.values,controls)==GraphicsRequestReason::None
				&& controls.lensDirt.enabled==(mode!=0) && controls.lensDirt.intensity==(mode==1 ? 0.0f : 1.0f)
				&& controls.lensDirt.texture==textureId,"Consumed dirt controls or asset identity differ from the native case.");
			for(int i=0;i<3;++i){owner.context.BeginFrame();FinishFrame(owner);}owner.context.WaitIdle();
			const auto timings=owner.context.GetCompletedPostProcessTimings();row["timings"]=Json::array();
			for(const std::string label:{std::string("bloom"),std::string("lens-radiance"),std::string("lens-dirt")})
			{
				const auto timing=std::find_if(timings.begin(),timings.end(),[&](const auto& value){
					return value.owner.frameId==frame && value.effect.stableId==label
						&& value.owner.settingsRevision==settings.revision && value.owner.lifecycle==settings.lifecycle
						&& value.owner.view==*accepted->presentationInputs[0].graphicsView
						&& value.owner.targetGeneration==processed.result.backendTarget->targetGeneration
						&& value.availability==PostProcessTimingAvailability::Measured;
				});
				if(mode!=2 && label!="bloom") {Require(timing==timings.end(),"Inactive dirt has a measured native effect cost.");continue;}
				Require(timing!=timings.end() && timing->durationMilliseconds && std::isfinite(*timing->durationMilliseconds)
					&& *timing->durationMilliseconds>=0 && timing->nonemptyWork && timing->beginAvailable && timing->endAvailable
					&& timing->submitSerial && timing->completionSerial>=timing->submitSerial,
					"Lens dirt or its real shared radiance has no exact completed named GPU timing: "+label);
				row["timings"].push_back({{"effect",label},{"milliseconds",*timing->durationMilliseconds},
					{"frameId",frame},{"settingsRevision",settings.revision},{"targetGeneration",timing->owner.targetGeneration},
					{"submitSerial",timing->submitSerial},{"completionSerial",timing->completionSerial}});
			}
			row["hudVerified"]=true;row["status"]="Passed";++frame;
		}
	}

	void RunNativeScaledCameraCase(ContextOwner& owner, std::uint64_t& frame, Json& result)
	{
		constexpr glm::uvec2 output{640u, 360u}, internal{320u, 180u};
		const std::array values{
			GraphicsSettingValue{GraphicsSettingId::GeometryRasterMode, std::uint32_t{1}},
			GraphicsSettingValue{GraphicsSettingId::UpscalingMode, std::uint32_t{1}},
			GraphicsSettingValue{GraphicsSettingId::InternalResolutionScale, 0.5f}};
		const auto capabilities = owner.context.ReadGraphicsCatalogCapabilities(true);
		Require(capabilities.temporalUpscalingSupported, "The RTX adapter lacks the indexed half-extent candidate.");
		// An empty geometric fixture isolates actual framebuffers and HUD
		// ordering. The moving-edge fixture separately proves geometry/motion.
		auto& resources = ResourceManager::Instance();
		resources.LoadDefaultResources();
		int width = 0, height = 0;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()), &width, &height);
		Require(width == output.x && height == output.y, "Runtime smoke requires a physical 640x360 output.");
		const auto outputId = ResolveDedicatedCaptureViewIdentity(RenderViewType::Game).outputId;
		for(std::uint32_t index = 0; index < 2u; ++index)
		{
			owner.context.BeginFrame();
			std::vector<RenderViewPresentationInput> views;
			views.push_back(View(owner.context, frame, RenderViewType::Game, RenderOutputKind::OffscreenTexture,
				outputId, output, 0.0f, true, false, values));
			views.push_back(View(owner.context, frame, RenderViewType::Game, RenderOutputKind::MainRuntimeView,
				2u, output, 0.0f, true, false, values));
			Draw(owner, Publication(owner.context, frame, std::move(views)));
			const auto recorded = owner.context.ReadRecordedGraphicsViews(frame);
			Require(recorded.size() == 2u, "Both physically scaled camera recordings must be accepted.");
			for(const auto& view : recorded)
			{
				Require(view.width == output.x && view.height == output.y && view.internalWidth == internal.x
					&& view.internalHeight == internal.y && view.rasterSampleCount == 1u && view.temporalRecordingId == frame,
					"The native command receipt does not prove the true internal and output extents.");
				Require(view.temporalPreviousFrameId == (index == 0 ? 0u : frame - 1u),
					"The scaled camera did not retain its own previous recording.");
			}
			const auto raw = Read(owner.context, "scaled-raw-" + std::to_string(frame), RenderViewType::Game,
				true, frame, internal, RenderCaptureImageStage::Raw);
			const auto processed = Read(owner.context, "scaled-processed-" + std::to_string(frame), RenderViewType::Game,
				true, frame, output, RenderCaptureImageStage::Processed);
			const auto composited = Read(owner.context, "scaled-composited-" + std::to_string(frame), RenderViewType::Game,
				true, frame, output, RenderCaptureImageStage::Composited);
			ExpectColor(Pixel(composited, output.x / 2u, output.y / 2u),
				{0.2f, 0.4f, 0.1f}, "Temporal upscaling changed the late HUD color.");
			result.push_back({{"frameId", frame}, {"nativeMainAndDedicatedViews", recorded.size()},
				{"raw", CaptureEvidence(raw)}, {"processed", CaptureEvidence(processed)}, {"hudVerified", true}});
			++frame;
		}
	}

	void RunNativeScreenLightingCases(ContextOwner& owner, std::uint64_t& frame, Json& result)
	{
		ResourceManager::Instance().LoadDefaultResources();
		const auto capabilities = owner.context.ReadGraphicsCatalogCapabilities(false);
		const auto native = static_cast<PostProcessEffectCapability>(capabilities.postProcessEffectMask);
		const auto all = PostProcessEffectCapability::GTAO | PostProcessEffectCapability::SSR | PostProcessEffectCapability::SSGI;
		if(!HasPostProcessEffectCapability(native, all)) return;
		const std::array requested{
			std::pair{GraphicsSettingId::GtaoEnabled, "gtao"},
			std::pair{GraphicsSettingId::ScreenSpaceReflectionsEnabled, "ssr"},
			std::pair{GraphicsSettingId::ScreenSpaceIndirectEnabled, "ssgi"}};
		for(std::uint32_t mask = 1u; mask <= 7u; ++mask)
		{
			std::vector<GraphicsSettingValue> values{{GraphicsSettingId::GeometryRasterMode, std::uint32_t{1}}};
			std::vector<std::string> timingsExpected{"screen-space-composite"};
			for(std::size_t i = 0; i < requested.size(); ++i)
			{
				if((mask & (1u << i)) == 0u) continue;
				values.push_back({requested[i].first, true});
				timingsExpected.emplace_back(requested[i].second);
			}
			owner.context.BeginFrame();
			auto input = View(owner.context, frame, RenderViewType::Game, RenderOutputKind::GameView,
				2u, kExtent, 0.0f, false, false, values);
			Draw(owner, Publication(owner.context, frame, {std::move(input)}));
			const auto raw = Read(owner.context, "screen-raw-" + std::to_string(frame), RenderViewType::Game,
				false, frame, kExtent, RenderCaptureImageStage::Raw);
			const auto processed = Read(owner.context, "screen-processed-" + std::to_string(frame), RenderViewType::Game,
				false, frame, kExtent, RenderCaptureImageStage::Processed);
			Require(raw.pixels.rgba == processed.pixels.rgba,
				"Background without credible geometry or hits must retain the exact environment result.");
			for(int i = 0; i < 3; ++i) { owner.context.BeginFrame(); FinishFrame(owner); }
			const auto timings = owner.context.GetCompletedPostProcessTimings();
			auto& row = result.emplace_back(Json{{"mask", mask}, {"frameId", frame}, {"skyPixelsEqual", true}, {"gpuMs", Json::object()}});
			for(const auto& expected : timingsExpected)
			{
				const auto timing = std::find_if(timings.begin(), timings.end(), [&](const auto& sample) {
					return sample.owner.frameId == frame && sample.effect.stableId == expected
						&& sample.availability == PostProcessTimingAvailability::Measured;
				});
				Require(timing != timings.end() && timing->durationMilliseconds.has_value(),
					"A demanded native lighting stage has no covering completed timestamp.");
				row["gpuMs"][expected] = *timing->durationMilliseconds;
			}
			++frame;
		}
	}

	// Exercise real ordinary indexed surfaces independently of the sky-only
	// fallback cases. CPU materialization is bounded and immutable; native worker
	// topology preparation is allowed to settle before a draw is considered proof.
	void RunScreenLightingGeometryCases(ContextOwner& owner, std::uint64_t& frame, Json& rows)
	{
		const auto capabilities=owner.context.ReadGraphicsCatalogCapabilities(false);
		const auto required=PostProcessEffectCapability::GTAO | PostProcessEffectCapability::SSR | PostProcessEffectCapability::SSGI;
		Require(HasPostProcessEffectCapability(static_cast<PostProcessEffectCapability>(capabilities.postProcessEffectMask),required),
			"Geometry lighting acceptance requires actual advertised AO, SSR, and SSGI support.");
		const auto geometry=TerraEngine::TestSupport::CreateScreenLightingGeometry();
		const std::array effects{
			std::pair{GraphicsSettingId::GtaoEnabled,"gtao"},
			std::pair{GraphicsSettingId::ScreenSpaceReflectionsEnabled,"ssr"},
			std::pair{GraphicsSettingId::ScreenSpaceIndirectEnabled,"ssgi"}};
		for(std::size_t effect=0;effect<effects.size();++effect)
		{
			for(const bool isolatedMiss : {false,true})
			{
				// An isolated front-facing wall has genuine valid normals/depth but
				// no other surface along an outgoing ray. SSR/SSGI must retain IBL.
				if(isolatedMiss && effect==0) continue;
				auto scene=geometry;
				if(isolatedMiss) scene.surfaces={geometry.surfaces.at(1)};
				auto& row=rows.emplace_back(Json{{"schemaVersion",1},{"effect",effects[effect].second},
					{"case",isolatedMiss ? "isolated-geometry-miss" : "contact-room-hit"},{"status","Pending"}});
				bool recorded=false;
				const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
				while(!recorded)
				{
					owner.context.BeginFrame();
					const std::array values{GraphicsSettingValue{GraphicsSettingId::GeometryRasterMode,std::uint32_t{1}},
						GraphicsSettingValue{GraphicsSettingId::ExposureMode,std::uint32_t{1}},
						GraphicsSettingValue{effects[effect].first,true}};
					auto input=View(owner.context,frame,RenderViewType::Game,RenderOutputKind::GameView,2u,kExtent,0,false,false,values);
					input.sceneViewGridVisible=false;
					Draw(owner,TerraEngine::TestSupport::PublishScreenLightingGeometry(scene,std::move(input),frame,
						owner.context.GetLatestRendererRecoverySnapshot().renderSessionGeneration,owner.context.GetBackendResourceGeneration()));
					const auto native=owner.context.ReadRecordedGraphicsViews(frame);
					const auto found=std::find_if(native.begin(),native.end(),[](const auto& view) { return view.publicationIndex==0; });
					recorded=found!=native.end() && found->indexedDrawCount==scene.surfaces.size()
						&& found->rasterSampleCount==1 && found->width==kExtent.x && found->height==kExtent.y;
					if(!recorded)
					{
						Require(std::chrono::steady_clock::now()<deadline,"Real indexed lighting scene never finished native preparation.");
						++frame;
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
				}
				row["frameId"]=frame; row["nativeIndexedDrawCount"]=scene.surfaces.size(); row["sampleCount"]=1;
				Json rawReceipt,processedReceipt;
				const auto raw=Read(owner.context,"lighting-geometry-raw-"+std::to_string(frame),RenderViewType::Game,
					false,frame,kExtent,RenderCaptureImageStage::Raw,&rawReceipt);
				row["raw"]=CaptureEvidence(raw);
				const auto processed=Read(owner.context,"lighting-geometry-processed-"+std::to_string(frame),RenderViewType::Game,
					false,frame,kExtent,RenderCaptureImageStage::Processed,&processedReceipt);
				row["processed"]=CaptureEvidence(processed);
				Require(raw.result.backendTarget->targetGeneration==processed.result.backendTarget->targetGeneration,
					"Lighting raw/processed captures do not share the exact native target lifetime.");
				std::uint64_t changed=0,backgroundChanged=0,alphaChanged=0;
				int maximumDelta=0;
				for(std::uint32_t y=0;y<kExtent.y;++y) for(std::uint32_t x=0;x<kExtent.x;++x)
				{
					const auto a=Pixel(raw,x,y),b=Pixel(processed,x,y);
					int delta=0; for(int channel=0;channel<3;++channel) delta=std::max(delta,std::abs(a[channel]-b[channel]));
					if(delta) { ++changed; maximumDelta=std::max(maximumDelta,delta); if(y<10) ++backgroundChanged; }
					if(a[3]!=b[3]) ++alphaChanged;
				}
				row["changedPixels"]=changed; row["maximumCodeDelta"]=maximumDelta;
				row["backgroundChangedPixels"]=backgroundChanged; row["alphaChangedPixels"]=alphaChanged;
				Require(alphaChanged==0 && backgroundChanged==0,"Lighting altered current alpha or known background fallback pixels.");
				Require(isolatedMiss ? changed==0 : changed>0,
					isolatedMiss ? "Isolated valid geometry miss did not preserve exact fallback." : "Real geometry produced no independent screen lighting signal.");
				// Drain actual native query completion; never reuse a different frame
				// or infer zero cost from the absence of a completed timing record.
				for(int i=0;i<3;++i) { owner.context.BeginFrame(); FinishFrame(owner); }
				owner.context.WaitIdle();
				const auto timings=owner.context.GetCompletedPostProcessTimings();
				row["gpuMs"]=Json::object(); row["timingReceipts"]=Json::array();
				const auto accepted=owner.context.ReadAcceptedRenderFramePublication(frame);
				Require(accepted && accepted->presentationInputs.size()==1 && accepted->presentationInputs[0].graphicsSettings,
					"Geometry lighting lacks its exact accepted settings publication.");
				const auto& settings=*accepted->presentationInputs[0].graphicsSettings;
				PostProcessEffectSettings effective;
				Require(ResolvePostProcessEffectSettings(settings.values,effective)==GraphicsRequestReason::None
					&& effective.gtao.enabled==(effect==0) && effective.reflections.enabled==(effect==1) && effective.indirect.enabled==(effect==2),
					"Independent lighting control publication does not match the actual recorded case.");
				std::vector<std::string> measuredStages{effects[effect].second,"screen-space-composite"};
				const bool hierarchyRecorded=std::any_of(timings.begin(),timings.end(),[frame](const auto& sample) {
					return sample.owner.frameId==frame && sample.effect.stableId=="screen-depth-hierarchy";
				});
				Require(!hierarchyRecorded || effect!=0,"GTAO alone unexpectedly recorded a radiance depth hierarchy.");
				if(hierarchyRecorded) measuredStages.emplace_back("screen-depth-hierarchy");
				for(const auto& expected : measuredStages)
				{
					const auto timing=std::find_if(timings.begin(),timings.end(),[&](const auto& value) {
						return value.owner.frameId==frame && value.effect.stableId==expected
							&& value.owner.settingsRevision==frame && value.owner.lifecycle==settings.lifecycle
							&& value.owner.targetGeneration==raw.result.backendTarget->targetGeneration
							&& value.owner.view.outputId==2u && value.owner.view.viewType==RenderViewType::Game
							&& value.availability==PostProcessTimingAvailability::Measured;
					});
					Require(timing!=timings.end() && timing->durationMilliseconds && std::isfinite(*timing->durationMilliseconds)
						&& *timing->durationMilliseconds>=0,"Geometry lighting stage lacks a finite completed native GPU cost.");
					Require(timing->nonemptyWork && timing->beginAvailable && timing->endAvailable
						&& timing->submitSerial>0 && timing->completionSerial>=timing->submitSerial,
						"Lighting GPU timing has no actual completed nonempty command proof.");
					row["gpuMs"][expected]=*timing->durationMilliseconds;
					row["timingReceipts"].push_back({{"effect",expected},{"frameId",timing->owner.frameId},
						{"settingsRevision",timing->owner.settingsRevision},{"targetGeneration",timing->owner.targetGeneration},
						{"submitSerial",timing->submitSerial},{"completionSerial",timing->completionSerial},
						{"commandSerial",timing->owner.commandSerial},{"queryPoolGeneration",timing->owner.queryPoolGeneration},
						{"beginTicks",timing->beginTicks},{"endTicks",timing->endTicks},{"timestampValidBits",timing->timestampValidBits}});
				}
				row["status"]="Passed"; ++frame;
			}
		}
	}


	// Static opaque geometry isolates camera motion. Every sequence ends at the
	// same pose; only the accepted previous-camera transform and blur controls
	// can explain changed Processed pixels. This does not prove object motion.
	void RunNativeMotionBlurGeometryCase(ContextOwner& owner, std::uint64_t& frame, Json& rows)
	{
		const auto capabilities=owner.context.ReadGraphicsCatalogCapabilities(false);
		Require(HasPostProcessEffectCapability(static_cast<PostProcessEffectCapability>(capabilities.postProcessEffectMask),
			PostProcessEffectCapability::MotionBlur),"Camera blur evidence requires actual native motion-blur support.");
		const auto geometry=TerraEngine::TestSupport::CreateScreenLightingGeometry();
		std::optional<Capture> baselineRaw,baselineProcessed;
		for(std::uint32_t mode=0;mode<3;++mode)
		{
			auto& row=rows.emplace_back(Json{{"case",mode==0 ? "disabled" : mode==1 ? "zero-strength" : "camera-only"},
				{"status","Pending"},{"objectOnlyExercised",false},{"cameraTranslationMetres",0.18f}});
			std::uint64_t predecessorFrame=0;
			std::shared_ptr<const RenderFramePublication> predecessor;
			const auto draw=[&](float cameraX) {
				owner.context.BeginFrame();
				const std::array values{
					GraphicsSettingValue{GraphicsSettingId::GeometryRasterMode,std::uint32_t{1}},
					GraphicsSettingValue{GraphicsSettingId::MotionBlurEnabled,mode!=0},
					GraphicsSettingValue{GraphicsSettingId::MotionBlurStrength,mode==1 ? 0.0f : 1.0f},
					GraphicsSettingValue{GraphicsSettingId::MotionBlurCameraContribution,1.0f},
					GraphicsSettingValue{GraphicsSettingId::MotionBlurObjectContribution,0.0f}};
				// Prepare the actual candidate once under its stable history owner.
				// A disabled bootstrap each frame would replace that owner.
				auto input=View(owner.context,frame,RenderViewType::Game,RenderOutputKind::GameView,2u,kExtent,0.01f,false,false,values,false);
				input.graphicsView=GraphicsViewIdentity{RenderViewType::Game,2u,1u,RenderOutputKind::GameView};
				input.projectionMatrix=glm::orthoRH_ZO(-3.2f,3.2f,-1.8f,1.8f,0.1f,25.0f);
				input.projectionMatrix[1][1]*=-1.0f;
				input.viewMatrix=glm::translate(glm::mat4(1),glm::vec3(-cameraX,0,0));
				// Off/zero share the actual identity and projection of the active lane.
				auto candidate=std::make_shared<GraphicsSettingsSnapshot>(*input.graphicsSettings);
				candidate->target.view=input.graphicsView;
				input.graphicsSettings=candidate;
				const auto probe=TerraEngine::TestSupport::PublishScreenLightingGeometry(geometry,input,frame,
					owner.context.GetLatestRendererRecoverySnapshot().renderSessionGeneration,owner.context.GetBackendResourceGeneration());
				std::string reason;bool preparationPending=false;
				// This protected production entry first qualifies the actual sealed
				// indexed draws, then prepares the motion/depth/pipeline variants.
				// Do not let message argument evaluation race the reason output.
				const bool prepared=owner.context.PrepareIndexedRasterCandidate(*probe,0,*candidate,reason,preparationPending);
				if(!prepared)
				{
					row["lastPreparation"]={{"frameId",frame},{"pending",preparationPending},{"reason",reason}};
					FinishFrame(owner);
					if(preparationPending)return false;
					Require(false,"Camera blur indexed/post-process preparation failed: "+reason);
				}
				auto certified=std::make_shared<GraphicsSettingsSnapshot>(*candidate);
				certified->postProcessResourceGeneration=owner.context.GetBackendResourceGeneration();
				input.graphicsSettings=std::move(certified);
				const auto publication=TerraEngine::TestSupport::PublishScreenLightingGeometry(geometry,std::move(input),frame,
					owner.context.GetLatestRendererRecoverySnapshot().renderSessionGeneration,owner.context.GetBackendResourceGeneration());
				Draw(owner,publication);
				const auto recorded=owner.context.ReadRecordedGraphicsViews(frame);
				const auto found=std::find_if(recorded.begin(),recorded.end(),[](const auto& value){return value.publicationIndex==0;});
				return found!=recorded.end() && found->indexedDrawCount==geometry.surfaces.size()
					&& found->rasterSampleCount==1 && found->width==kExtent.x && found->height==kExtent.y;
			};
			// Allow only actual immutable topology preparation to settle. No moving
			// sample is accepted until the complete ordinary geometry was recorded.
			const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
			while(!draw(0.0f))
			{
				Require(std::chrono::steady_clock::now()<deadline,"Motion-blur geometry never became natively drawable.");
				++frame;std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			for(int step=1;step<=3;++step)
			{
				predecessorFrame=frame;predecessor=owner.context.ReadAcceptedRenderFramePublication(frame);
				Require(predecessor && predecessor->frameId==frame,"Camera translation predecessor was not actually accepted.");
				++frame;
				Require(draw(0.06f*float(step)),"Continuous camera-motion frame lost its actual indexed geometry.");
			}
			const auto accepted=owner.context.ReadAcceptedRenderFramePublication(frame);
			Require(accepted && accepted->presentationInputs.size()==1 && accepted->presentationInputs[0].graphicsSettings,
				"Camera blur lost its accepted immutable current publication.");
			const auto& presentation=accepted->presentationInputs[0];const auto& settings=*presentation.graphicsSettings;
			PostProcessEffectSettings controls;
			Require(ResolvePostProcessEffectSettings(settings.values,controls)==GraphicsRequestReason::None
				&& controls.motionBlur.enabled==(mode!=0) && controls.motionBlur.strength==(mode==1 ? 0.0f : 1.0f)
				&& controls.motionBlur.objectContribution==0 && controls.motionBlur.cameraContribution==1,
				"Camera-only blur controls differ from their consumed publication.");
			GraphicsAAConfiguration aa;
			Require(ResolveGraphicsAAConfiguration(settings.values,aa)==GraphicsRequestReason::None
				&& aa.raster==GraphicsGeometryRasterMode::Indexed && aa.upscaling==GraphicsUpscalingMode::Off
				&& aa.sampleCount==1 && presentation.graphicsView && predecessor->presentationInputs.size()==1,
				"Camera blur evidence must use the same actual native indexed AA-Off view.");
			Require(presentation.graphicsView==predecessor->presentationInputs[0].graphicsView
				&& std::abs(presentation.viewMatrix[3].x+0.18f)<0.00001f
				&& std::abs(predecessor->presentationInputs[0].viewMatrix[3].x+0.12f)<0.00001f,
				"Accepted continuous camera poses or exact view identity differ from the prescribed translation.");
			row["view"]={{"outputId",presentation.graphicsView->outputId},
				{"lifetimeGeneration",presentation.graphicsView->lifetimeGeneration}};
			const auto raw=Read(owner.context,"motion-camera-raw-"+std::to_string(frame),RenderViewType::Game,false,frame,kExtent,RenderCaptureImageStage::Raw);
			const auto processed=Read(owner.context,"motion-camera-processed-"+std::to_string(frame),RenderViewType::Game,false,frame,kExtent,RenderCaptureImageStage::Processed);
			row["raw"]=CaptureEvidence(raw);row["processed"]=CaptureEvidence(processed);row["frameId"]=frame;
			Require(raw.result.backendTarget->targetGeneration==processed.result.backendTarget->targetGeneration,
				"Camera blur stage captures differ in actual native target lifetime.");
			if(mode==0){baselineRaw=raw;baselineProcessed=processed;}
			else
			{
				Require(raw.pixels.rgba==baselineRaw->pixels.rgba,"Changing blur controls altered Raw at the same camera pose.");
				Require((processed.pixels.rgba!=baselineProcessed->pixels.rgba)==(mode==2),
					mode==1 ? "Zero-strength blur differs from disabled output." : "Camera-only blur produced no changed native pixels.");
			}
			const auto recorded=owner.context.ReadRecordedGraphicsViews(frame);
			const auto native=std::find_if(recorded.begin(),recorded.end(),[](const auto& value){return value.publicationIndex==0;});
			Require(native!=recorded.end(),"Camera blur lost its command-side frame receipt.");
			row["recordedMotionDrawCount"]=native->recordedMotionDrawCount;
			row["previousFrameId"]=native->temporalPreviousFrameId;row["motionRecordingId"]=native->temporalRecordingId;
			if(mode==2)
				Require(native->recordedMotionDrawCount==geometry.surfaces.size() && native->temporalRecordingId
					&& native->temporalPreviousFrameId==predecessorFrame,
					"Camera blur has no exact unjittered immediately preceding accepted motion receipt.");
			for(int i=0;i<3;++i){owner.context.BeginFrame();FinishFrame(owner);}owner.context.WaitIdle();
			const auto timings=owner.context.GetCompletedPostProcessTimings();
			const auto timing=std::find_if(timings.begin(),timings.end(),[&](const auto& value){
				return value.owner.frameId==frame && value.effect.stableId=="motion-blur"
					&& value.owner.settingsRevision==settings.revision && value.owner.lifecycle==settings.lifecycle
					&& value.owner.view==*presentation.graphicsView
					&& value.owner.targetGeneration==processed.result.backendTarget->targetGeneration
					&& value.availability==PostProcessTimingAvailability::Measured;
			});
			if(mode!=2) Require(timing==timings.end(),"Inactive blur unexpectedly produced an effect GPU timing.");
			else
			{
				Require(timing!=timings.end() && timing->durationMilliseconds && std::isfinite(*timing->durationMilliseconds)
					&& *timing->durationMilliseconds>=0 && timing->nonemptyWork && timing->beginAvailable && timing->endAvailable
					&& timing->submitSerial && timing->completionSerial>=timing->submitSerial,
					"Camera blur lacks its exact separately measured completed native timing.");
				row["timing"]={{"effect","motion-blur"},{"milliseconds",*timing->durationMilliseconds},
					{"frameId",timing->owner.frameId},{"settingsRevision",timing->owner.settingsRevision},
					{"targetGeneration",timing->owner.targetGeneration},{"submitSerial",timing->submitSerial},
					{"completionSerial",timing->completionSerial},{"commandSerial",timing->owner.commandSerial}};
			}
			row["status"]="Passed";++frame;
		}
	}

	Json RunBorrowedTargetReplacement(ContextOwner& owner, std::uint64_t& frame)
	{
		const auto initialValidation = owner.context.GetReadbackLifetimeValidationSnapshot();
		if(!initialValidation.timelineSemaphoreEnabled)
			return {{"status", "NotExercised"}, {"reason", "Native timeline-semaphore readback gate is unavailable."}};
		owner.context.SetSceneViewRawPreview(false);
		owner.context.BeginFrame();
		auto initialView = View(owner.context, frame, RenderViewType::Scene,
			RenderOutputKind::SceneView, 1, kExtent, 0.5f, false);
		Draw(owner, Publication(owner.context, frame, {initialView}));
		const auto baseline = Read(owner.context, "borrow-baseline-" + std::to_string(frame),
			RenderViewType::Scene, false, frame, kExtent, RenderCaptureImageStage::Raw);
		const auto originalGeneration = baseline.result.backendTarget->targetGeneration;
		const auto borrowedFrame = frame;
		RenderCaptureRequest request;
		request.requestId = "borrow-pending-" + std::to_string(frame);
		request.sourceKind = RenderCaptureSourceKind::GameView;
		request.viewType = RenderViewType::Scene;
		request.targetSize = kExtent;
		request.submittedFrameId = frame;
		request.allowBackendFallback = false;
		request.timeoutMs = 60000;
		request.imageStage = RenderCaptureImageStage::Raw;

		// Every exit path signals before context shutdown can wait for this queue.
		// This guard never supplies fake completion; the real fence must settle.
		struct GateOwner
		{
			VulkanContext& context;
			bool armed = false;
			void Release()
			{
				Require(armed && context.ReleaseReadbackLifetimeGate(), "Cannot release the actual pending readback gate.");
				armed = false;
			}
			~GateOwner() { if(armed) context.ReleaseReadbackLifetimeGate(); }
		} gate{owner.context};
		Require(owner.context.ArmReadbackLifetimeGate(request.requestId), "Cannot arm native readback ownership gate.");
		gate.armed = true;
		const auto acknowledgement = owner.context.ReadbackCapture(request);
		Require(acknowledgement.ok && acknowledgement.pendingAsync && acknowledgement.backendReadback,
			"Gated Raw readback did not reach native queue submission.");
		Require(owner.context.PollCaptureReadbacks().empty(), "Unsignalled readback unexpectedly produced pixels.");
		const auto held = owner.context.GetReadbackLifetimeValidationSnapshot();
		Require(held.operationId == request.requestId && held.submitAccepted && held.gateRecorded && !held.hostReleased
			&& held.sourceImageBlocked && held.nativeState == ReadbackNativeState::Submitted
			&& owner.context.GetCaptureReadbackOwnership(request.requestId) == CaptureReadbackOwnership::Retained,
			"Raw target was not retained by the real incomplete readback.");

		// Keep the existing raw source image and extent. Changing only grid pass
		// compatibility replaces processing resources without an initializing queue
		// submission that could block behind the intentionally gated readback.
		auto changedView = initialView;
		changedView.sceneViewGridVisible = true;
		std::string reason;
		Require(owner.context.PrepareFixedExposureView(changedView, *changedView.graphicsSettings, reason),
			"Same-extent processing replacement failed while old Raw was borrowed: " + reason);
		Require(owner.context.GetCaptureReadbackOwnership(request.requestId) == CaptureReadbackOwnership::Retained,
			"Processing replacement released a source with an actual incomplete readback.");
		gate.Release();

		Capture original;
		bool complete = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while(!complete)
		{
			for(auto& result : owner.context.PollCaptureReadbacks())
			{
				Require(result.requestId == request.requestId && result.ok && result.backendReadback && result.backendTarget,
					"Borrowed original Raw capture failed after actual gate release.");
				Require(result.frameId == borrowedFrame && result.graphicsSettingsRevision == borrowedFrame
					&& result.imageStage == RenderCaptureImageStage::Raw
					&& result.backendTarget->targetGeneration == originalGeneration,
					"Replacement changed the submitted Raw capture provenance.");
				Require(result.sourceColorEncoding == RenderCaptureColorEncoding::Linear
					&& result.deliveredColorEncoding == RenderCaptureColorEncoding::Srgb,
					"Borrowed Raw capture transfer metadata changed.");
				auto pixels = TerraEngine::TestSupport::DecodeCaptureBase64(result.base64Data);
				Require(pixels.width == baseline.pixels.width && pixels.height == baseline.pixels.height
					&& pixels.rgba == baseline.pixels.rgba, "Borrowed old Raw pixels changed after processing replacement.");
				original = {std::move(result), std::move(pixels)};
				complete = true;
			}
			Require(complete || std::chrono::steady_clock::now() < deadline, "Borrowed readback never completed after gate release.");
			if(!complete) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Require(owner.context.GetCaptureReadbackOwnership(request.requestId) == CaptureReadbackOwnership::Released,
			"Completed Raw readback retained its source blocker.");
		++frame;
		owner.context.BeginFrame();
		auto replacementView = View(owner.context, frame, RenderViewType::Scene,
			RenderOutputKind::SceneView, 1, kExtent, 0.5f, false, true);
		Draw(owner, Publication(owner.context, frame, {std::move(replacementView)}));
		const auto replacement = Read(owner.context, "borrow-replacement-" + std::to_string(frame),
			RenderViewType::Scene, false, frame, kExtent, RenderCaptureImageStage::Raw);
		Require(replacement.result.backendTarget->targetGeneration > originalGeneration,
			"A newer processing target was not accepted after the borrowed target retired.");
		++frame;
		return {{"status", "Passed"}, {"sourceExtentChanged", false}, {"nativeGateSubmitted", true},
			{"sourceRetainedDuringReplacement", true}, {"originalRaw", CaptureEvidence(original)},
			{"replacementRaw", CaptureEvidence(replacement)}, {"originalPixelsPreserved", true},
			{"readbackRetirementObserved", true}};
	}

	Json RunDisabledBypass(ContextOwner& owner, std::uint64_t& frame)
	{
		owner.context.SetSceneViewRawPreview(false);
		Json disabledDiagnostics = Json::array();
		const auto drawBypass = [&](bool typed) {
			owner.context.BeginFrame();
			auto input = View(owner.context, frame, RenderViewType::Scene,
				RenderOutputKind::SceneView, 1, kExtent, 0.0f, false);
			if(!typed) input.graphicsSettings.reset();
			const auto queryInput = input;
			Draw(owner, Publication(owner.context, frame, {std::move(input)}));
			const auto diagnostics = owner.context.GetPostProcessDiagnostics(queryInput);
			const auto& effect = ExposureDiagnostic(diagnostics);
			Require(effect.state == PostProcessEffectState::Disabled
				&& effect.reason == PostProcessDiagnosticReason::Disabled && !effect.timing.durationMilliseconds
				&& diagnostics.frameId == frame && diagnostics.settingsRevision == (typed ? frame : 0),
				"Bypass diagnostics do not identify disabled work without a GPU cost.");
			disabledDiagnostics.push_back(DiagnosticEvidence(diagnostics));
			const auto preview = owner.context.GetSceneViewPostProcessPreview();
			Require(!preview.postProcessActive && !preview.effectiveRaw
				&& preview.frameId == frame && preview.extent == kExtent,
				"Bypass frame reports an active or mismatched Scene preview.");
			RenderCaptureRequest explicitRequest;
			explicitRequest.requestId = "bypass-unavailable-" + std::to_string(frame);
			explicitRequest.sourceKind = RenderCaptureSourceKind::GameView;
			explicitRequest.viewType = RenderViewType::Scene;
			explicitRequest.targetSize = kExtent;
			explicitRequest.submittedFrameId = frame;
			explicitRequest.allowBackendFallback = false;
			explicitRequest.imageStage = RenderCaptureImageStage::Processed;
			const auto unavailable = owner.context.ReadbackCapture(explicitRequest);
			Require(!unavailable.ok && !unavailable.pendingAsync && unavailable.errorCategory == "capture_stage_unavailable",
				"Disabled output incorrectly exposed an older processed image.");
			return Read(owner.context, "bypass-legacy-" + std::to_string(frame), RenderViewType::Scene,
				false, frame, kExtent, RenderCaptureImageStage::Legacy);
		};
		const auto legacyFrame = frame;
		const auto legacy = drawBypass(false);
		++frame;
		const auto supportFrame = frame;
		owner.context.BeginFrame();
		auto active = View(owner.context, frame, RenderViewType::Scene,
			RenderOutputKind::SceneView, 1, kExtent, 0.5f, false);
		Draw(owner, Publication(owner.context, frame, {std::move(active)}));
		Require(owner.context.GetSceneViewPostProcessPreview().postProcessActive,
			"Disabled bypass control did not follow a real active exposure frame.");
		++frame;
		const auto disabledFrame = frame;
		const auto disabled = drawBypass(true);
		Require(legacy.pixels.width == disabled.pixels.width && legacy.pixels.height == disabled.pixels.height
			&& legacy.pixels.rgba == disabled.pixels.rgba,
			"Typed 0-EV bypass differs from the same build's untyped legacy output.");
		for(int step = 0; step < 3; ++step) { owner.context.BeginFrame(); FinishFrame(owner); }
		for(const auto& sample : owner.context.GetCompletedPostProcessTimings())
			Require(sample.owner.frameId != legacyFrame && sample.owner.frameId != disabledFrame,
				"A disabled or legacy frame produced a GPU effect timing sample.");
		++frame;
		return {{"status", "Passed"}, {"legacy", CaptureEvidence(legacy)}, {"disabled", CaptureEvidence(disabled)},
			{"activeSupportFrameId", supportFrame}, {"decodedPixelsExactlyEqual", true},
			{"previewInactive", true}, {"processedStageUnavailable", true}, {"effectTimingSamplesAbsent", true},
			{"disabledDiagnostics", disabledDiagnostics}};
	}
}

int main(int count, char** arguments)
{
	try
	{
		const auto options = Parse(count, arguments);
		Json evidence{{"schema", "post-process-foundation-native-v1"}, {"runNonce", options.nonce},
			{"processId", GetCurrentProcessId()}, {"status", "Incomplete"}, {"evidenceKind", "RealVulkan"}, {"cases", Json::array()},
			{"richColorCases",Json::array()},{"spatialAACases",Json::array()},{"presentationLensCases",Json::array()},
			{"hdrCameraCases",Json::array()},{"scaledCameraCases",Json::array()},{"screenLightingCases",Json::array()},{"screenLightingGeometryCases",Json::array()},
			{"editorPresentationReceipts",Json::array()}};
		SessionLog logs;
		const auto sink = Logger::Instance().AddSink([&](const LogEntry& entry) { logs.Append(entry); });
		struct SinkOwner { Logger::SinkId id; ~SinkOwner() { Logger::Instance().RemoveSink(id); } } sinkOwner{sink};
		int exitCode = 1;
		{
			auto& pool = ThreadPool::instance();
			struct PoolOwner { ThreadPool& pool; ~PoolOwner() { pool.shutdown(); } } poolOwner{pool};
			auto& window = WindowSystem::Instance();
			struct WindowOwner { WindowSystem& window; ~WindowOwner() { window.Shutdown(); } } windowOwner{window};
			try
			{
				Require(std::filesystem::is_directory(options.installRoot / "Editor"), "Installed Editor resource root is unavailable.");
				ResourceManager::Instance().SetEngineRoot(options.installRoot / "Editor");
				pool.initialize(2);
				window.Configure(std::make_unique<GlfwWindowBackend>(), {640, 360, "Post-processing foundation", false}, [] {});
				window.Initialize();
				auto* handle = static_cast<GLFWwindow*>(window.GetNativeHandle());
				Require(handle != nullptr, "Native fixture window is unavailable.");
				glfwHideWindow(handle);
				Require(glfwGetWindowAttrib(handle, GLFW_VISIBLE) == GLFW_FALSE, "Native fixture window could not be hidden.");
				// Settle initial native window messages before a context captures its
				// swapchain extent. Later completion boundaries also pump real events.
				glfwPollEvents();
				for(bool editor : {true, false})
				{
					ContextOwner owner;
					owner.editor = editor;
					if(editor) owner.presentationEvidence = &evidence["editorPresentationReceipts"];
					owner.context.SetValidationLayersRequested(true);
					owner.context.SetVariableRateShadingEnabled(false);
					owner.context.Initialize(window);
					const auto startup = owner.context.GetEmptyFrameValidationReceipt();
					Require(startup.initialized && startup.validationActuallyEnabled, "Actual Vulkan initialization and validation are required.");
					if(editor) owner.context.EnableEditorMode();
					const auto initialCapabilities = owner.context.ReadGraphicsCatalogCapabilities(!editor);
					Require(initialCapabilities.postProcessColorSupported,
						"The selected native adapter did not qualify color processing before any offscreen allocation.");
					const auto adapter = owner.context.GetLatestRendererRecoverySnapshot();
					evidence[editor ? "editorAdapter" : "runtimeAdapter"] = {{"name", adapter.hardwareName}, {"driver", adapter.driverVersion},
						{"api", adapter.apiVersion},{"colorSupportedBeforeOffscreenAllocation",initialCapabilities.postProcessColorSupported}};
					std::uint64_t frame = 1;
					if(editor)
					{
						evidence["cases"].push_back(RunCase(owner, frame, RenderViewType::Scene, -1.0f, false, false, kExtent));
						evidence["cases"].push_back(RunCase(owner, frame, RenderViewType::Scene, 0.5f, false, false, kExtent));
						evidence["cases"].push_back(RunCase(owner, frame, RenderViewType::Scene, 0.5f, false, false, kExtent, true));
						const auto oldGeneration = evidence["cases"].back().at("processed").at("targetGeneration").get<std::uint64_t>();
						auto resized = RunCase(owner, frame, RenderViewType::Scene, 0.5f, false, false, {384, 216});
						Require(resized.at("processed").at("targetGeneration").get<std::uint64_t>() > oldGeneration,
							"Actual processing target resize did not advance its generation.");
						resized["resizeAfterCompletedReadbacks"] = true;
						evidence["cases"].push_back(std::move(resized));
						evidence["cases"].push_back(RunCase(owner, frame, RenderViewType::Game, 0.5f, false, false, kExtent));
						evidence["inFlightTargetReplacement"] = RunBorrowedTargetReplacement(owner, frame);
						evidence["disabledBypass"] = RunDisabledBypass(owner, frame);
						evidence["richColorCases"].push_back(RunRichColorCase(owner,frame,false,false,false));
						auto gt7 = RunRichColorCase(owner,frame,false,false,false,ToneMappingMode::GT7);
						const auto& filmic = evidence["richColorCases"].front();
						Require(gt7["raw"]["pngBase64"] == filmic["raw"]["pngBase64"]
							&& gt7["processed"]["centerRgba"] != filmic["processed"]["centerRgba"],
							"GT7 and Filmic must produce distinct camera output from identical raw pixels at matched exposure.");
						evidence["gt7ToneMapping"]["scene"] = std::move(gt7);
						evidence["richColorCases"].push_back(RunRichColorCase(owner,frame,true,false,false));
						evidence["richColorCases"].push_back(RunRichColorCase(owner,frame,true,true,false));
						RunTemporalAutoExposure(owner,frame,evidence["temporalAutoExposure"]);
						RunPresentationLensCases(owner,frame,evidence["presentationLensCases"],false);
						RunNativeHdrCameraCases(owner,frame,evidence["hdrCameraCases"]);
						RunAtmosphereAndDitherCases(owner,frame,evidence["atmosphereAndDitherCases"]);
						RunNativeLensDirtCase(owner,frame,evidence["lensDirtCases"]);
						RunNativeScreenLightingCases(owner,frame,evidence["screenLightingCases"]);
						RunScreenLightingGeometryCases(owner,frame,evidence["screenLightingGeometryCases"]);
						RunNativeMotionBlurGeometryCase(owner,frame,evidence["motionBlurGeometryCases"]);
						for(std::uint32_t mode = 1u; mode <= 3u; ++mode)
						{
							evidence["spatialAACases"].push_back(Json::object());
							RunSpatialAACase(owner,frame,mode,initialCapabilities,evidence["spatialAACases"].back());
						}
					}
					else
					{
						evidence["cases"].push_back(RunCase(owner, frame, RenderViewType::Game, 0.5f, true, true, kExtent));
						evidence["richColorCases"].push_back(RunRichColorCase(owner,frame,true,true,true));
						evidence["gt7ToneMapping"]["runtime"] = RunRichColorCase(owner,frame,false,false,true,ToneMappingMode::GT7);
						RunPresentationLensCases(owner,frame,evidence["presentationLensCases"],true);
						RunRuntimeDitherCase(owner,frame,evidence["atmosphereAndDitherCases"]);
						if(initialCapabilities.temporalUpscalingSupported)
							RunNativeScaledCameraCase(owner,frame,evidence["scaledCameraCases"]);
					}
					owner.Close();
					Require(owner.context.GetEmptyFrameValidationReceipt().validationErrorsAfter == 0, "Vulkan validation reported a native lifecycle error.");
				}
				evidence["status"] = "Passed";
				exitCode = 0;
			}
			catch(const std::exception& error) { evidence["status"] = "Failed"; evidence["reason"] = error.what(); }
		}
		{
			std::lock_guard lock(logs.mutex);
			evidence["errorLog"] = logs.errors;
			if(!logs.errors.empty()) { evidence["status"] = "Failed"; exitCode = 1; }
		}
		evidence["threadPoolWorkersAfterCleanup"] = ThreadPool::instance().getThreadCount();
		evidence["windowReleased"] = WindowSystem::Instance().GetNativeHandle() == nullptr;
		evidence["resizeWithPendingBorrow"] = {{"status", "NotExercised"}, {"reason", "This fixture collects and retires each native readback before replacing view targets."}};
		WriteFresh(options.output, evidence);
		return exitCode;
	}
	catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
