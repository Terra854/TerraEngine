// Existing Scene, Game/runtime and dedicated capture view preparation in original output order.
#include "RenderFrameSealing.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Profiling/ProfilerScope.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace TerraEngine::Renderer::Frame {
// Resolve exact captured output identity and per-view localization without reopening live scene state.
RenderOutputRequest MakeFrameOutputRequest(RenderOutputKind kind, RenderViewType diagnosticsView, std::uint32_t outputId,
	const glm::uvec2& effectiveSceneTargetSize, const glm::uvec2& effectiveGameTargetSize) {
		RenderOutputRequest request{};
		request.kind = kind;
		request.priority = RenderOutputPriority::Required;
		request.diagnosticsView = diagnosticsView;
		// Normal editor targets follow panel dimensions; sealed fixture targets
		// retain their actual native extent through query, submission and readback.
		if (kind == RenderOutputKind::SceneView)
		{
			request.targetSize = effectiveSceneTargetSize;
		}
		else if (kind == RenderOutputKind::GameView || kind == RenderOutputKind::MainRuntimeView)
		{
			request.targetSize = effectiveGameTargetSize;
		}
		request.outputId = outputId;
		return request;
	}
RenderReferenceFrame FindFrameReferenceFrame(const RenderFrameSnapshot& renderFrameSnapshot, RenderOutputKind outputKind, std::uint32_t outputId) {
		for (const TerraEngine::Renderer::RenderFrameViewRequest& request :
			renderFrameSnapshot.viewRequests)
		{
			if (request.outputKind == outputKind && request.outputId == outputId)
			{
				return request.referenceFrame;
			}
		}
		return TerraEngine::Renderer::RenderReferenceFrame{};
	}
void LocalizeFrameLights(RenderViewBuildState& view) {
		view.lightCandidates.erase(std::remove_if(
			view.lightCandidates.begin(), view.lightCandidates.end(),
			[&](FrameLightCandidate& candidate)
			{
				const TerraEngine::Renderer::RenderLocalizationResult localized =
					TerraEngine::Renderer::LocalizeRenderPosition(
						candidate.canonicalPosition, view.referenceFrame);
				if (!localized.Succeeded())
				{
					return true;
				}
				candidate.gpuLight.positionAndType = glm::vec4(
					localized.offsetMetres,
					candidate.gpuLight.positionAndType.w);
				return false;
			}), view.lightCandidates.end());
	}
void LocalizeFrameFixtureCamera(RenderViewBuildState& view, const TerraEngine::Scene::WorldPosition& canonicalCamera,
	std::span<const RendererEfficiencyResolvedFixtureCamera> sealedFixtureCameras) {
	const auto sealedCamera = [&](RenderViewType type, std::uint32_t output) {
		return view.efficiencyFixtureIntent.controlsApplied && std::any_of(sealedFixtureCameras.begin(), sealedFixtureCameras.end(),
			[&](const auto& value) {return value.viewType == type && value.outputId == output;});
	};

		// Sealed matrices already incorporate both baseline and per-step origins.
		// Applying the legacy UI-camera translation again would double the rebase.
		if(sealedCamera(view.referenceFrame.outputId == 1 ? RenderViewType::Scene : RenderViewType::Game,view.referenceFrame.outputId)) return;
		if (!view.efficiencyFixtureIntent.controlsApplied || !TerraEngine::Renderer::RendererEfficiencyFixtureOriginDelta(
			view.efficiencyFixtureIntent, view.referenceFrame.outputId == 1 ? RenderViewType::Scene : RenderViewType::Game,
			view.referenceFrame.outputId)) return;
		const auto localized = TerraEngine::Renderer::LocalizeRenderPosition(canonicalCamera, view.referenceFrame);
		if (!localized.Succeeded()) return; // Preflight already rejects unsafe view deltas.
		const glm::vec3 camera(localized.offsetMetres);
		view.viewMatrix = view.viewMatrix * glm::translate(glm::mat4(1), view.cameraLocalPosition - camera);
		view.cameraLocalPosition = camera;
	}
}

void RenderSystem::PrepareFrameSceneView(const FrameVisibilityInputs& visibilityInputs, const FrameVirtualGeometryInputs& virtualGeometryInputs,
	FrameVirtualGeometryPreparation& virtualGeometryPreparation, const FrameVisibleStreamingInputs& visibleStreamingInputs,
	const FrameViewSelectionSettings& settings, FrameViewBuildOutputs& outputs,
	std::vector<std::pair<RenderViewType, TerraEngine::Renderer::VisibleStreamingTouchSet>>& pendingStreamingTouchSets,
	const RenderViewBuildState& frameData,
	const TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera* fixedSceneCamera,
	bool sealedCameraRecipeActive, std::span<const TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera> sealedFixtureCameras)
{
	const auto& renderFrameSnapshot = visibilityInputs.renderFrameSnapshot;
	const auto renderables = visibilityInputs.renderables;
	const auto& renderableDrawCommands = visibilityInputs.renderableDrawCommands;
	const auto& renderableCommandIndices = visibilityInputs.identityCache.indicesByObjectId;
	const auto& retainedRenderTopology = visibilityInputs.retainedRenderTopology;
	auto& groupedSubmissionMembershipCache = lifetimeState->groupedSubmissionMembershipCache;
	const auto snapshotRendererGeneration = visibilityInputs.snapshotRendererGeneration;
	const auto renderTopologyRebuilt = settings.renderTopologyRebuilt;
	const auto renderPreparationSettingsSignature = settings.renderPreparationSettingsSignature;
	const auto renderableSubmissionSignature = settings.renderableSubmissionSignature;
	const auto renderGraphWorkerCount = settings.renderGraphWorkerCount;
	const auto renderableDrawCommandsByOrdinal = settings.renderableDrawCommandsByOrdinal;
	const auto effectiveSceneTargetSize = settings.effectiveSceneTargetSize;
	const auto effectiveGameTargetSize = settings.effectiveGameTargetSize;
	auto& renderGraphDiagnostics = virtualGeometryInputs.renderGraphDiagnostics;
	auto& preparedRenderSubmissions = outputs.preparedRenderSubmissions;
	auto& visibleAnimatedRenderableFlags = outputs.visibleAnimatedRenderableFlags;
	auto& sceneSpatialDiagnostics = outputs.sceneSpatialDiagnostics;
	auto& gameSpatialDiagnostics = outputs.gameSpatialDiagnostics;
	auto& sceneVisibleCommandTelemetry = outputs.sceneVisibleCommandTelemetry;
	auto& gameVisibleCommandTelemetry = outputs.gameVisibleCommandTelemetry;

	const bool sceneViewTargetReady = fixedSceneCamera || !editorModeEnabled || (sceneViewTargetSize.x > 0u && sceneViewTargetSize.y > 0u);

	if ((fixedSceneCamera || (!sealedCameraRecipeActive && hasSceneViewCamera)) && sceneViewTargetReady)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Scene View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Start from the small common frame header, then build this view in its own
		// owner. Copying the shared header before materialization avoids deep-copying
		// thousands of completed command and page records when the view is queued.
		RenderViewBuildState sceneFrameData = frameData;
		sceneFrameData.viewMatrix = fixedSceneCamera ? fixedSceneCamera->viewMatrix : sceneViewCamera.view;
		sceneFrameData.projectionMatrix = fixedSceneCamera ? fixedSceneCamera->projectionMatrix : sceneViewCamera.proj;
		sceneFrameData.referenceFrame = FindFrameReferenceFrame(renderFrameSnapshot,
			RenderOutputKind::SceneView, 1U);
		sceneFrameData.cameraLocalPosition = fixedSceneCamera ? fixedSceneCamera->cameraLocalPosition : glm::vec3(0.0F);
		LocalizeFrameFixtureCamera(sceneFrameData, sceneViewCamera.canonicalPosition, sealedFixtureCameras);
		LocalizeFrameLights(sceneFrameData);
		sceneFrameData.sceneViewGridVisible = !fixedSceneCamera && sceneViewGridVisible;
		sceneFrameData.sceneViewOrthographic = !fixedSceneCamera && sceneViewCamera.orthographic;
		VisibleCommandBuildResult visibleCommands;
		std::vector<RenderItemRecord>& sceneCommands =
			lifetimeState->viewCommandScratch;
		sceneCommands.clear();
		std::uint64_t sceneGroupingCompatibilitySignature = 0U;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Scene View Visible Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			visibleCommands = PrepareFrameVisibleCommands(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs, pendingStreamingTouchSets,
				sceneFrameData.projectionMatrix * sceneFrameData.viewMatrix,
				effectiveSceneTargetSize, RenderOutputKind::SceneView,
				RenderViewType::Scene, 1U, sceneFrameData.cameraLocalPosition,
				sceneFrameData.referenceFrame);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Materialize Scene View Commands",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			visibleCommands.PopulateSelectedItems(sceneCommands, renderables,
				renderableDrawCommands,
				&visibleAnimatedRenderableFlags, &renderableCommandIndices,
				&sceneGroupingCompatibilitySignature);
			sceneFrameData.virtualGeometryVisibilitySetupSignature =
				sceneGroupingCompatibilitySignature;
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Bind Scene View Geometry Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			CopyFrameVirtualGeometryPayload(sceneFrameData, visibleCommands);
		}
		sceneSpatialDiagnostics = visibleCommands.diagnostics;
		sceneVisibleCommandTelemetry = visibleCommands.reuseTelemetry;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Apply Scene View Context",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(!fixedSceneCamera) ApplyFrameSceneContextTint(sceneFrameData, sceneCommands);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Scene View Groups",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			PrepareFrameGroupedSubmissions(retainedRenderTopology, groupedSubmissionMembershipCache, snapshotRendererGeneration, renderTopologyRebuilt, sceneFrameData, sceneCommands,
				RenderOutputKind::SceneView,
				RenderViewType::Scene, 1u,
				sceneGroupingCompatibilitySignature,
				visibleCommands.cacheHit);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Scene View Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			// The spatial query uses its unchanged canonical index frame, not the
			// deliberately shifted GPU view frame. Rebasing cannot change membership.
			sceneFrameData.shadowDrawLists = PrepareFrameShadowLists(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, renderableDrawCommandsByOrdinal, renderPreparationSettingsSignature, renderableSubmissionSignature, renderGraphWorkerCount, sceneFrameData.lightCandidates, sceneFrameData.cameraLocalPosition, sceneFrameData.referenceFrame, RenderViewType::Scene,1);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Queue Scene View Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			QueueFramePreparedSubmission(visibilityInputs, preparedRenderSubmissions, std::move(sceneFrameData),
				MakeFrameOutputRequest(RenderOutputKind::SceneView, RenderViewType::Scene, 1u, effectiveSceneTargetSize, effectiveGameTargetSize),
				sceneCommands, visibleCommands.cachedRecord);
		}
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		renderGraphDiagnostics.delayedWork.push_back(
			hasSceneViewCamera
				? "Scene View skipped because its render target is not ready."
				: "Scene View skipped because the editor did not provide a scene camera this frame.");
	}

}
void RenderSystem::PrepareFrameGameViews(const FrameVisibilityInputs& visibilityInputs, const FrameVirtualGeometryInputs& virtualGeometryInputs,
	FrameVirtualGeometryPreparation& virtualGeometryPreparation, const FrameVisibleStreamingInputs& visibleStreamingInputs,
	const FrameViewSelectionSettings& settings, FrameViewBuildOutputs& outputs,
	std::vector<std::pair<RenderViewType, TerraEngine::Renderer::VisibleStreamingTouchSet>>& pendingStreamingTouchSets,
	const RenderViewBuildState& frameData,
	std::span<const CameraEntry> cameras, const FrameWorldTransforms& worldCapture,
	const TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera* fixedGameCamera,
	bool sealedCameraRecipeActive, std::span<const TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera> sealedFixtureCameras,
	float deltaTime)
{
	const auto& renderFrameSnapshot = visibilityInputs.renderFrameSnapshot;
	const auto renderables = visibilityInputs.renderables;
	const auto& renderableDrawCommands = visibilityInputs.renderableDrawCommands;
	const auto& renderableCommandIndices = visibilityInputs.identityCache.indicesByObjectId;
	const auto& retainedRenderTopology = visibilityInputs.retainedRenderTopology;
	auto& groupedSubmissionMembershipCache = lifetimeState->groupedSubmissionMembershipCache;
	const auto snapshotRendererGeneration = visibilityInputs.snapshotRendererGeneration;
	const auto renderTopologyRebuilt = settings.renderTopologyRebuilt;
	const auto renderPreparationSettingsSignature = settings.renderPreparationSettingsSignature;
	const auto renderableSubmissionSignature = settings.renderableSubmissionSignature;
	const auto renderGraphWorkerCount = settings.renderGraphWorkerCount;
	const auto renderableDrawCommandsByOrdinal = settings.renderableDrawCommandsByOrdinal;
	const auto effectiveSceneTargetSize = settings.effectiveSceneTargetSize;
	const auto effectiveGameTargetSize = settings.effectiveGameTargetSize;
	auto& renderGraphDiagnostics = virtualGeometryInputs.renderGraphDiagnostics;
	auto& preparedRenderSubmissions = outputs.preparedRenderSubmissions;
	auto& visibleAnimatedRenderableFlags = outputs.visibleAnimatedRenderableFlags;
	auto& sceneSpatialDiagnostics = outputs.sceneSpatialDiagnostics;
	auto& gameSpatialDiagnostics = outputs.gameSpatialDiagnostics;
	auto& sceneVisibleCommandTelemetry = outputs.sceneVisibleCommandTelemetry;
	auto& gameVisibleCommandTelemetry = outputs.gameVisibleCommandTelemetry;

	const bool gameViewTargetReady = fixedGameCamera || !editorModeEnabled || (gameViewTargetSize.x > 0u && gameViewTargetSize.y > 0u);
	const auto* worldTransforms = worldCapture.worldTransforms;
	// Asset-style prefab editing has no active runtime scene. When the editor
	// requests suppression, skip every Game View submission path so opening a
	// prefab asset cannot burn a full offscreen game render every frame.
	if (fixedGameCamera || (!sealedCameraRecipeActive && !gameViewSuppressed && gameViewTargetReady && !gameViewCameras.empty()))
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Game View Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Use the same existing GameView query/submission path with one immutable
		// sealed descriptor; author components and UI descriptors remain untouched.
		std::array<OverrideCameraData,1> sealedGameDescriptor{};
		std::span<const OverrideCameraData> effectiveGameCameras = gameViewCameras;
		if(fixedGameCamera) {
			sealedGameDescriptor[0] = {fixedGameCamera->viewMatrix,fixedGameCamera->projectionMatrix,fixedGameCamera->cameraPosition,false};
			effectiveGameCameras = sealedGameDescriptor;
		}
		for (const auto& cam : effectiveGameCameras)
		{
			RenderViewBuildState gameFrameData = frameData;
			gameFrameData.editorContextTint = glm::vec4(0.0f);
			gameFrameData.viewMatrix = cam.view;
			gameFrameData.projectionMatrix = cam.proj;
			gameFrameData.referenceFrame = FindFrameReferenceFrame(renderFrameSnapshot,
				RenderOutputKind::GameView, 2U);
			gameFrameData.cameraLocalPosition = fixedGameCamera ? fixedGameCamera->cameraLocalPosition : glm::vec3(0.0F);
			LocalizeFrameFixtureCamera(gameFrameData, cam.canonicalPosition, sealedFixtureCameras);
			LocalizeFrameLights(gameFrameData);
			VisibleCommandBuildResult visibleCommands;
			std::vector<RenderItemRecord>& gameCommands =
				lifetimeState->viewCommandScratch;
			gameCommands.clear();
			std::uint64_t gameGroupingCompatibilitySignature = 0U;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Game View Visible Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				visibleCommands = PrepareFrameVisibleCommands(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs, pendingStreamingTouchSets,
					gameFrameData.projectionMatrix * gameFrameData.viewMatrix,
					effectiveGameTargetSize, RenderOutputKind::GameView,
					RenderViewType::Game, 2U, gameFrameData.cameraLocalPosition,
					gameFrameData.referenceFrame);
			}
			visibleCommands.PopulateSelectedItems(gameCommands, renderables,
				renderableDrawCommands,
				&visibleAnimatedRenderableFlags, &renderableCommandIndices,
				&gameGroupingCompatibilitySignature);
			gameFrameData.virtualGeometryVisibilitySetupSignature =
				gameGroupingCompatibilitySignature;
			CopyFrameVirtualGeometryPayload(gameFrameData, visibleCommands);
			gameSpatialDiagnostics = visibleCommands.diagnostics;
			gameVisibleCommandTelemetry = visibleCommands.reuseTelemetry;
			PrepareFrameGroupedSubmissions(retainedRenderTopology, groupedSubmissionMembershipCache, snapshotRendererGeneration, renderTopologyRebuilt, gameFrameData, gameCommands,
				RenderOutputKind::GameView,
				RenderViewType::Game, 2u,
				gameGroupingCompatibilitySignature,
				visibleCommands.cacheHit);
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Game View Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				gameFrameData.shadowDrawLists = PrepareFrameShadowLists(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, renderableDrawCommandsByOrdinal, renderPreparationSettingsSignature, renderableSubmissionSignature, renderGraphWorkerCount, gameFrameData.lightCandidates, gameFrameData.cameraLocalPosition, gameFrameData.referenceFrame, RenderViewType::Game,2);
			}
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Queue Game View Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				QueueFramePreparedSubmission(visibilityInputs, preparedRenderSubmissions, std::move(gameFrameData),
					MakeFrameOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u, effectiveSceneTargetSize, effectiveGameTargetSize),
					gameCommands, visibleCommands.cachedRecord);
			}
			latestGameViewFrameId = activeRenderFrameId;
		}
	}

	else if (!sealedCameraRecipeActive && !gameViewSuppressed && gameViewTargetReady && cameras.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Blank Game View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderViewBuildState blankData{};
		blankData.frameId = activeRenderFrameId;
		blankData.viewMatrix = glm::mat4(1.0f);
		blankData.projectionMatrix = glm::mat4(1.0f);
		blankData.cameraLocalPosition = glm::vec3(0.0f);
		blankData.deltaTimeSeconds = deltaTime;
		blankData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
			blankData.cullingEpsilon = 0.001f;
			blankData.activePreset = currentPreset;
			blankData.customOverrideActive = (currentPreset == QualityPreset::Custom);
			blankData.settings = SanitizeRenderSettings(currentSettings);
			blankData.efficiencyOptions = renderFrameSnapshot.efficiencyOptions;
			blankData.efficiencyFixtureIntent = renderFrameSnapshot.efficiencyFixtureIntent;
			blankData.sky = frameData.sky;
			blankData.editorContextTint = glm::vec4(0.0f);
			blankData.virtualGeometryGpuPackets.clear();
			blankData.virtualGeometryGpuBatches.clear();
			blankData.virtualGeometryGpuFrameSets.clear();
			blankData.virtualGeometryGpuCompactionDispatches.clear();
				blankData.virtualGeometryGeneratedSubmissions.clear();
				blankData.virtualGeometryCounterReadbacks.clear();
				blankData.virtualGeometryGpuFallbackRecords.clear();
				blankData.virtualGeometryPageRetirements.clear();
				blankData.shadowDrawLists.clear();
			QueueFramePreparedSubmission(visibilityInputs, preparedRenderSubmissions, std::move(blankData),
				MakeFrameOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u, effectiveSceneTargetSize, effectiveGameTargetSize),
				{}, nullptr);
			latestGameViewFrameId = activeRenderFrameId;
	}
	else if (!sealedCameraRecipeActive && !gameViewSuppressed && gameViewTargetReady)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Runtime Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

		for (size_t cameraIndex = 0; cameraIndex < cameras.size(); ++cameraIndex)
		{
			RenderViewBuildState runtimeFrameData = frameData;
			const entt::entity cameraEntity = cameras[cameraIndex].entity;
			const auto& camera = cameras[cameraIndex].camera;
			const auto propagatedCameraTransform = worldTransforms->find(cameraEntity);
			TerraEngine::Scene::WorldPose cameraTransform =
				propagatedCameraTransform != worldTransforms->end()
					? propagatedCameraTransform->second : TerraEngine::Scene::WorldPose{};

			// Normalize the rotation so derived direction vectors remain unit length
			// even if authoring data drifts slightly off the identity quaternion.
			const glm::quat cameraRotation = glm::normalize(cameraTransform.rotation);
			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = cameraRotation * defaultForward;
			glm::vec3 up = cameraRotation * camera.up;

			if (glm::length(up) < 0.0001f)
			{
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			// Clamp projection inputs to keep glm::perspective from receiving invalid values.
			const float clampedFov = std::max(0.001f, camera.verticalFovDegrees);
			const float safeAspect = camera.aspectRatio > 0.0f ? camera.aspectRatio : 1.0f;
			const float nearPlane = std::max(0.0001f, camera.nearPlane);
			const float farPlane = std::max(nearPlane + 0.001f, camera.farPlane);
			runtimeFrameData.viewMatrix = glm::lookAt(
				glm::vec3(0.0F), forward, glm::normalize(up));
			// Use an explicit Vulkan depth projection. Relying on GLM's global
			// depth macro is fragile because this file includes engine headers
			// before local GLM configuration defines are visible.
			runtimeFrameData.projectionMatrix = glm::perspectiveRH_ZO(glm::radians(clampedFov), safeAspect, nearPlane, farPlane);
			runtimeFrameData.projectionMatrix[1][1] *= -1;
			runtimeFrameData.cameraLocalPosition = glm::vec3(0.0F);
			runtimeFrameData.referenceFrame = FindFrameReferenceFrame(renderFrameSnapshot,
				RenderOutputKind::MainRuntimeView, 2U);
			LocalizeFrameFixtureCamera(runtimeFrameData, cameraTransform.position, sealedFixtureCameras);
			LocalizeFrameLights(runtimeFrameData);
			// The editor publishes this descriptor through GameViewWindow. A
			// packaged runtime has no dock, so publish the exact authored camera
			// used for MainRuntimeView here as read-only capture metadata. This
			// lets exported-runtime validation request a real Vulkan readback
			// without inventing an editor-only camera owner.
			latestGameViewCamera = {
				runtimeFrameData.viewMatrix,
				runtimeFrameData.projectionMatrix,
				cameraTransform.position,
				false
			};
			hasLatestGameViewCamera = true;
			runtimeFrameData.editorContextTint = glm::vec4(0.0f);
			const VisibleCommandBuildResult visibleCommands = PrepareFrameVisibleCommands(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs, pendingStreamingTouchSets,
				runtimeFrameData.projectionMatrix * runtimeFrameData.viewMatrix,
				gameViewTargetSize, RenderOutputKind::MainRuntimeView,
				RenderViewType::Game, 2U, runtimeFrameData.cameraLocalPosition,
				runtimeFrameData.referenceFrame);
			std::vector<RenderItemRecord>& runtimeCommands =
				lifetimeState->viewCommandScratch;
			runtimeCommands.clear();
			std::uint64_t runtimeGroupingCompatibilitySignature = 0U;
			visibleCommands.PopulateSelectedItems(runtimeCommands, renderables,
				renderableDrawCommands,
				&visibleAnimatedRenderableFlags, &renderableCommandIndices,
				&runtimeGroupingCompatibilitySignature);
			runtimeFrameData.virtualGeometryVisibilitySetupSignature =
				runtimeGroupingCompatibilitySignature;
			CopyFrameVirtualGeometryPayload(runtimeFrameData, visibleCommands);
			gameSpatialDiagnostics = visibleCommands.diagnostics;
			PrepareFrameGroupedSubmissions(retainedRenderTopology, groupedSubmissionMembershipCache, snapshotRendererGeneration, renderTopologyRebuilt, runtimeFrameData, runtimeCommands,
				RenderOutputKind::MainRuntimeView,
				RenderViewType::Game, 2u,
				runtimeGroupingCompatibilitySignature,
				visibleCommands.cacheHit);
			runtimeFrameData.shadowDrawLists = PrepareFrameShadowLists(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, renderableDrawCommandsByOrdinal, renderPreparationSettingsSignature, renderableSubmissionSignature, renderGraphWorkerCount, runtimeFrameData.lightCandidates, runtimeFrameData.cameraLocalPosition, runtimeFrameData.referenceFrame, RenderViewType::Game,2);

			QueueFramePreparedSubmission(visibilityInputs, preparedRenderSubmissions, std::move(runtimeFrameData),
				MakeFrameOutputRequest(RenderOutputKind::MainRuntimeView,
					RenderViewType::Game, 2u, effectiveSceneTargetSize, effectiveGameTargetSize), runtimeCommands,
				visibleCommands.cachedRecord);
			latestGameViewFrameId = activeRenderFrameId;
		}
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested() && gameViewSuppressed)
	{
		renderGraphDiagnostics.delayedWork.push_back("Game View skipped because the editor suppressed it for prefab asset editing.");
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested() && !gameViewTargetReady)
	{
		renderGraphDiagnostics.delayedWork.push_back("Game View skipped because its render target is not ready.");
	}

}
void RenderSystem::PrepareFrameDedicatedViews(const FrameVisibilityInputs& visibilityInputs, const FrameVirtualGeometryInputs& virtualGeometryInputs,
	FrameVirtualGeometryPreparation& virtualGeometryPreparation, const FrameVisibleStreamingInputs& visibleStreamingInputs,
	const FrameViewSelectionSettings& settings, FrameViewBuildOutputs& outputs,
	std::vector<std::pair<RenderViewType, TerraEngine::Renderer::VisibleStreamingTouchSet>>& pendingStreamingTouchSets,
	const RenderViewBuildState& frameData,
	const TerraEngine::Renderer::BackendCapabilityProfile& backendCapabilities,
	const std::deque<TerraEngine::Renderer::RenderCaptureRequest>& captureRequestsForReadback)
{
	const auto& renderFrameSnapshot = visibilityInputs.renderFrameSnapshot;
	const auto renderables = visibilityInputs.renderables;
	const auto& renderableDrawCommands = visibilityInputs.renderableDrawCommands;
	const auto& renderableCommandIndices = visibilityInputs.identityCache.indicesByObjectId;
	const auto& retainedRenderTopology = visibilityInputs.retainedRenderTopology;
	auto& groupedSubmissionMembershipCache = lifetimeState->groupedSubmissionMembershipCache;
	const auto snapshotRendererGeneration = visibilityInputs.snapshotRendererGeneration;
	const auto renderTopologyRebuilt = settings.renderTopologyRebuilt;
	const auto renderPreparationSettingsSignature = settings.renderPreparationSettingsSignature;
	const auto renderableSubmissionSignature = settings.renderableSubmissionSignature;
	const auto renderGraphWorkerCount = settings.renderGraphWorkerCount;
	const auto renderableDrawCommandsByOrdinal = settings.renderableDrawCommandsByOrdinal;
	const auto effectiveSceneTargetSize = settings.effectiveSceneTargetSize;
	const auto effectiveGameTargetSize = settings.effectiveGameTargetSize;
	auto& renderGraphDiagnostics = virtualGeometryInputs.renderGraphDiagnostics;
	auto& preparedRenderSubmissions = outputs.preparedRenderSubmissions;
	auto& visibleAnimatedRenderableFlags = outputs.visibleAnimatedRenderableFlags;
	auto& sceneSpatialDiagnostics = outputs.sceneSpatialDiagnostics;
	auto& gameSpatialDiagnostics = outputs.gameSpatialDiagnostics;
	auto& sceneVisibleCommandTelemetry = outputs.sceneVisibleCommandTelemetry;
	auto& gameVisibleCommandTelemetry = outputs.gameVisibleCommandTelemetry;
	for (const TerraEngine::Renderer::RenderCaptureRequest& captureRequest : captureRequestsForReadback)
	{
		const bool needsDedicatedCaptureRender = (captureRequest.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
				|| captureRequest.dedicatedRenderTarget)
			&& captureRequest.format == "image/png"
			&& rhi
			&& backendCapabilities.supportsImageReadback;
		if (!needsDedicatedCaptureRender)
		{
			continue;
		}

		// Camera-object and custom MCP requests already carry immutable camera
		// matrices prepared by editor automation. Submit them as a dedicated
		// offscreen render target so readback sees live GPU pixels without
		// activating disabled authored cameras or changing the visible Game View.
		RenderViewBuildState captureFrameData = frameData;
		captureFrameData.viewMatrix = captureRequest.viewMatrix;
		captureFrameData.projectionMatrix = captureRequest.projectionMatrix;
		captureFrameData.cameraLocalPosition = glm::vec3(0.0F);
		captureFrameData.editorContextTint = glm::vec4(0.0f);
		captureFrameData.uiBatch = {};
		const TerraEngine::Renderer::RenderCaptureViewIdentity captureIdentity =
			TerraEngine::Renderer::ResolveDedicatedCaptureViewIdentity(
				captureRequest.viewType);
		captureFrameData.referenceFrame = FindFrameReferenceFrame(renderFrameSnapshot,
			RenderOutputKind::OffscreenTexture, captureIdentity.outputId);
		LocalizeFrameLights(captureFrameData);
		const VisibleCommandBuildResult visibleCommands = PrepareFrameVisibleCommands(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs, pendingStreamingTouchSets,
			captureFrameData.projectionMatrix * captureFrameData.viewMatrix,
			captureRequest.targetSize,
			RenderOutputKind::OffscreenTexture,
			captureRequest.viewType,
			captureIdentity.outputId,
			captureFrameData.cameraLocalPosition,
			captureFrameData.referenceFrame);
		std::vector<RenderItemRecord>& captureCommands =
			lifetimeState->viewCommandScratch;
		captureCommands.clear();
		std::uint64_t captureGroupingCompatibilitySignature = 0U;
		visibleCommands.PopulateSelectedItems(captureCommands, renderables,
			renderableDrawCommands,
			&visibleAnimatedRenderableFlags, &renderableCommandIndices,
			&captureGroupingCompatibilitySignature);
		captureFrameData.virtualGeometryVisibilitySetupSignature =
			captureGroupingCompatibilitySignature;
		CopyFrameVirtualGeometryPayload(captureFrameData, visibleCommands);
		PrepareFrameGroupedSubmissions(retainedRenderTopology, groupedSubmissionMembershipCache, snapshotRendererGeneration, renderTopologyRebuilt, captureFrameData, captureCommands,
			RenderOutputKind::OffscreenTexture,
			captureIdentity.viewType,
			captureIdentity.outputId,
			captureGroupingCompatibilitySignature,
			visibleCommands.cacheHit);
		captureFrameData.shadowDrawLists = PrepareFrameShadowLists(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, renderableDrawCommandsByOrdinal, renderPreparationSettingsSignature, renderableSubmissionSignature, renderGraphWorkerCount, captureFrameData.lightCandidates, captureFrameData.cameraLocalPosition, captureFrameData.referenceFrame, RenderViewType::Scene, 0U);

		RenderOutputRequest captureOutputRequest = MakeFrameOutputRequest(
			RenderOutputKind::OffscreenTexture, captureIdentity.viewType,
			captureIdentity.outputId, effectiveSceneTargetSize, effectiveGameTargetSize);
		captureOutputRequest.targetSize = captureRequest.targetSize;
		QueueFramePreparedSubmission(visibilityInputs, preparedRenderSubmissions, std::move(captureFrameData),
			std::move(captureOutputRequest), captureCommands,
			visibleCommands.cachedRecord);
	}
}
