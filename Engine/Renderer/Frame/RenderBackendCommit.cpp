// Protected backend commit preserves streaming publication, BeginFrame, consume, render and EndFrame ordering.
#include "RenderFrameSealing.h"
#include "RenderFrameState.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "Resource/ResourceManager.h"
#include "Core/Logger/Logger.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> RenderSystem::CommitFrameBackend(
	const std::optional<TerraEngine::Profiling::ProfilerFramePublicationEvidence>& pendingProfilerPublicationEvidence,
	const std::vector<std::pair<RenderViewType, TerraEngine::Renderer::VisibleStreamingTouchSet>>& pendingStreamingTouchSets,
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot, FrameViewObservations& observations,
	std::deque<TerraEngine::Renderer::RenderCaptureRequest>& captureRequestsForReadback,
	const TerraEngine::Renderer::Vulkan::VulkanRendererPreparationSummary& rendererPreparationSummary,
	const TerraEngine::Renderer::BackendCapabilityProfile& backendCapabilities,
	std::uint64_t snapshotRendererGeneration, TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics)
{
	auto& resourceManager = ResourceManager::Instance();
	const auto& visibleCommandCache = lifetimeState->visibleCommandCache;
	const auto& shadowDrawListCache = lifetimeState->shadowDrawListCache;
	auto& sceneCaptureProcessed = observations.sceneCaptureProcessed;
	auto& gameCaptureProcessed = observations.gameCaptureProcessed;
	auto& completedSceneCaptureIdentitySnapshot = observations.completedSceneCaptureIdentitySnapshot;
	auto& completedGameCaptureIdentitySnapshot = observations.completedGameCaptureIdentitySnapshot;
	if(pendingProfilerPublicationEvidence.has_value())
	{
		TerraEngine::Profiling::Profiler::Instance()
			.RecordFramePublicationEvidence(*pendingProfilerPublicationEvidence);
	}
	for(const auto& [viewType, touchSet] : pendingStreamingTouchSets)
	{
		const char* viewReason = viewType == RenderViewType::Scene
			? "visible scene view renderable" : "visible game view renderable";
		resourceManager.PublishVisibleStreamingTouches(
			touchSet, activeRenderFrameId, viewReason);
	}
	rhi->SetGlobalTaskShaderCullingEnabled(taskShaderCullingEnabled);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI Begin Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->BeginFrame();
	}
	// BeginFrame first retires and clears the reused backend frame slot. Publish
	// the sealed all-view plan only after that reset; publishing it earlier would
	// let Vulkan erase pending compute work before the first view records it.
	const bool backendAccepted = rhi->ConsumeRenderFramePublication(renderFramePublicationStore.Load());
	if(!backendAccepted)
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Render backend rejected frame " << activeRenderFrameId
				<< " immutable publication before command recording.");
	}
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
		sealedPublication = renderFramePublicationStore.Load();
	for(std::uint32_t publicationViewIndex = 0u;
		backendAccepted && sealedPublication != nullptr
		&& publicationViewIndex < sealedPublication->views.size(); ++publicationViewIndex)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI Render Prepared View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->RenderFrame(sealedPublication, publicationViewIndex);
	}
	// Capture target generations are backend-owned and become authoritative only
	// after the corresponding offscreen view has been rendered. Attach that exact
	// tuple before exposing the retained identity table to asynchronous picking.
	if(sceneCaptureProcessed)
	{
		const auto metadata = rhi->GetConventionalVisibilityIdentityTargetMetadata(
			RenderViewType::Scene, true);
		if(metadata && metadata->frameId == completedSceneCaptureIdentitySnapshot.frameId
			&& metadata->rendererGeneration
				== completedSceneCaptureIdentitySnapshot.rendererGeneration
			&& metadata->originGeneration
				== completedSceneCaptureIdentitySnapshot.originGeneration
			&& metadata->spatialGeneration
				== completedSceneCaptureIdentitySnapshot.spatialGeneration
			&& metadata->residencyGeneration
				== completedSceneCaptureIdentitySnapshot.residencyGeneration)
			completedSceneCaptureIdentitySnapshot.targetGeneration =
				metadata->targetGeneration;
	}
	if(gameCaptureProcessed)
	{
		const auto metadata = rhi->GetConventionalVisibilityIdentityTargetMetadata(
			RenderViewType::Game, true);
		if(metadata && metadata->frameId == completedGameCaptureIdentitySnapshot.frameId
			&& metadata->rendererGeneration
				== completedGameCaptureIdentitySnapshot.rendererGeneration
			&& metadata->originGeneration
				== completedGameCaptureIdentitySnapshot.originGeneration
			&& metadata->spatialGeneration
				== completedGameCaptureIdentitySnapshot.spatialGeneration
			&& metadata->residencyGeneration
				== completedGameCaptureIdentitySnapshot.residencyGeneration)
			completedGameCaptureIdentitySnapshot.targetGeneration =
				metadata->targetGeneration;
	}
	if (!captureRequestsForReadback.empty())
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (TerraEngine::Renderer::RenderCaptureRequest& captureRequest : captureRequestsForReadback)
		{
			submittedCaptureReadbackRequests.push_back(std::move(captureRequest));
		}
	}
	if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Visible draw command cache hits: "
			+ std::to_string(visibleCommandCache.hitCount)
			+ "/"
			+ std::to_string(visibleCommandCache.hitCount + visibleCommandCache.missCount));
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Shadow draw list cache hits: "
			+ std::to_string(shadowDrawListCache.hitCount)
			+ "/"
			+ std::to_string(shadowDrawListCache.hitCount + shadowDrawListCache.missCount));
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Persistent render packet cache hits: "
			+ std::to_string(rendererPreparationSummary.renderPacketCache.cacheHits)
			+ "/"
			+ std::to_string(rendererPreparationSummary.renderPacketCache.packetCount));
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI End Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->EndFrame();
	}
	// Prepare the complete fixed-exposure target set from the actual sealed
	// camera outputs. New settings can become visible at the next protected
	// capture only after every eligible view's native candidate succeeds.
	using namespace TerraEngine::Renderer::GraphicsSettings;
	auto* vulkan = dynamic_cast<VulkanContext*>(rhi.get());
	const auto recorded = backendAccepted && vulkan && sealedPublication
		? vulkan->ReadRecordedGraphicsViews(sealedPublication->frameId) : std::vector<VulkanContext::RecordedGraphicsView>{};
	const bool hasDedicatedCapture = sealedPublication && std::any_of(sealedPublication->presentationInputs.begin(),
		sealedPublication->presentationInputs.end(), [](const auto& view) {
			return view.output.kind == RenderOutputKind::OffscreenTexture;
		});
	if(vulkan && sealedPublication && backendAccepted)
	{
		const auto& views = sealedPublication->presentationInputs;
		const auto plannedScene = std::find_if(views.begin(), views.end(), [](const auto& view) {
			return view.output.kind == RenderOutputKind::SceneView;
		});
		if(plannedScene == views.end())
		{
			// A synchronous camera capture can run between editor frames. It
			// doesn't close the existing Scene viewport or retire its settings.
			if(!hasDedicatedCapture) RegisterSceneViewGraphicsOutput({}, 1u);
		}
		else if(const auto accepted = std::find_if(recorded.begin(), recorded.end(), [&](const auto& view) {
			return view.publicationIndex == static_cast<std::size_t>(plannedScene - views.begin());
		}); accepted != recorded.end())
		{
			const glm::uvec2 extent{accepted->width, accepted->height};
			RegisterSceneViewGraphicsOutput(extent, vulkan->ReadSpatialAAModeMask(extent));
		}
		// A planned viewport with a skipped submission retains its last scope.
	}
	if(sealedPublication && backendAccepted)
	{
		auto cameraViews = TerraEngine::Renderer::CollectPublishedGraphicsCameraTargets(*sealedPublication);
		for(auto& view : cameraViews)
		{
			const auto evidence = std::find_if(recorded.begin(), recorded.end(), [&](const auto& admitted) {
				return admitted.publicationIndex == view.publicationIndex;
			});
			view.width = evidence == recorded.end() ? 0 : evidence->width;
			view.height = evidence == recorded.end() ? 0 : evidence->height;
			if(view.width != 0 && view.height != 0 && vulkan)
				view.spatialAAModeMask = vulkan->ReadSpatialAAModeMask({view.width, view.height});
		}
		std::erase_if(cameraViews, [](const auto& view) { return view.width == 0 || view.height == 0; });
		auto& owner = *graphicsSettingsState;
		std::lock_guard lock(owner.mutex);
		for(auto& view : cameraViews)
		{
			auto& identity = *view.target.view;
			// A camera or extent change retires the prior view generation even
			// when Vulkan reuses the same physical output index for both views.
			const auto known = std::find_if(owner.publishedCameraViews.begin(), owner.publishedCameraViews.end(), [&](const auto& previous) {
				const auto& prior = *previous.target.view;
				return previous.target.camera == view.target.camera && prior.viewType == identity.viewType
					&& prior.outputKind == identity.outputKind && prior.outputId == identity.outputId
					&& previous.width == view.width && previous.height == view.height;
			});
			if(known != owner.publishedCameraViews.end()) identity.lifetimeGeneration = known->target.view->lifetimeGeneration;
			else if(identity.lifetimeGeneration == 0) identity.lifetimeGeneration = owner.nextCameraViewLifetime++;
		}
		if(hasDedicatedCapture)
		{
			for(const auto& previous : owner.publishedCameraViews)
			{
				if(!previous.target.camera || !previous.target.view || previous.target.view->outputKind == RenderOutputKind::OffscreenTexture) continue;
				const auto& prior = *previous.target.view;
				const auto& camera = *previous.target.camera;
				if(std::find(renderFrameSnapshot.graphicsCameraIdentities.begin(), renderFrameSnapshot.graphicsCameraIdentities.end(), camera)
					== renderFrameSnapshot.graphicsCameraIdentities.end()) continue;
				if(std::any_of(cameraViews.begin(), cameraViews.end(), [&](const auto& fresh) { return fresh.target == previous.target; })) continue;
				bool plannedOutput = false, plannedPriorCamera = false;
				for(const auto& planned : sealedPublication->presentationInputs)
				{
					const auto& output = planned.output;
					if(output.kind != prior.outputKind || output.diagnosticsView != prior.viewType || output.outputId != prior.outputId) continue;
					plannedOutput = true;
					const bool nativeRuntimeExtent = output.kind == RenderOutputKind::MainRuntimeView && output.targetSize == glm::uvec2{};
					plannedPriorCamera |= planned.graphicsCamera == camera && (nativeRuntimeExtent
						|| output.targetSize == glm::uvec2{previous.width, previous.height});
				}
				if(plannedOutput && !plannedPriorCamera) continue;
				// Retain the existing authority without inventing a native row for
				// this capture frame. Genuine replacements still use recorded rows.
				auto retained = previous;
				retained.publicationIndex = std::numeric_limits<std::uint32_t>::max();
				cameraViews.push_back(std::move(retained));
			}
		}
		if(vulkan && !recorded.empty()) vulkan->RetireInactiveCameraPostProcessTargets(cameraViews);
		owner.publishedCameraViews = std::move(cameraViews);
		std::optional<std::uint32_t> gameModes;
		for(const auto& admitted : recorded)
		{
			const auto& view = sealedPublication->presentationInputs[admitted.publicationIndex];
			const auto& output = view.output;
			if(output.kind != RenderOutputKind::GameView && output.kind != RenderOutputKind::MainRuntimeView) continue;
			owner.publishedGameExtent = {admitted.width, admitted.height};
			// Query the intersection of views that will inherit a Game AA
			// change. The Scene receiver and explicit camera AA own their values.
			if(output.diagnosticsView != RenderViewType::Game || !vulkan
				|| (view.graphicsCamera && owner.controller.CameraOverrides(*view.graphicsCamera, GraphicsSettingId::AntiAliasingMode)))
				continue;
			const auto modes = vulkan->ReadSpatialAAModeMask({admitted.width, admitted.height});
			gameModes = gameModes ? (*gameModes & modes) : modes;
		}
		if(gameModes || !hasDedicatedCapture) owner.publishedGameSpatialAAModeMask = gameModes;
	}
	if(vulkan && sealedPublication && backendAccepted && !recorded.empty())
		PreparePostProcessAuthoringViews(*sealedPublication, *vulkan);
	const auto pending = GetPendingGraphicsSettingsResourcePreparation();
	if(pending && sealedPublication && !recorded.empty())
	{
		const auto receipt = GetGraphicsSettingsReceipt(pending->requestId);
		const bool processingChanged = receipt && std::any_of(receipt->affectedSettings.begin(), receipt->affectedSettings.end(),
			[](const auto id) { return id >= GraphicsSettingId::AntiAliasingMode; });
		if(receipt && !processingChanged)
		{
			// Shadow filtering uses a published Vulkan uniform and the terrain
			// sampler path consumes the committed quality on the next view. With
			// a real view accepted on this generation these require no temporary
			// image allocation at this boundary.
			CompleteGraphicsSettingsResourcePreparation(pending->requestId, pending->lifecycle,
				pending->lifecycle.backendResourceGeneration == sealedPublication->backendResourceGeneration);
		}
		else if(processingChanged)
		{
			std::vector<std::pair<std::uint32_t, GraphicsSettingsSnapshot>> views;
			bool waitingForAssets = false;
			bool prepared = vulkan && pending->lifecycle.backendResourceGeneration == sealedPublication->backendResourceGeneration;
			std::string reason;
			auto failure = GraphicsRequestReason::PreparationFailed;
			if(prepared)
			{
				auto& owner = *graphicsSettingsState;
				{
					std::lock_guard lock(owner.mutex);
					for(const auto& admitted : recorded)
					{
						const auto& view = sealedPublication->presentationInputs[admitted.publicationIndex];
						if(view.graphicsCamera && !view.graphicsView) continue;
						if(pending->snapshot.target.scope == GraphicsScope::Camera
							&& (view.graphicsCamera != pending->snapshot.target.camera || !view.graphicsView)) continue;
						const GraphicsTarget target = view.graphicsCamera && view.graphicsView
							? GraphicsTarget{GraphicsScope::Camera, view.graphicsCamera, view.graphicsView} : GraphicsTarget{};
						views.emplace_back(admitted.publicationIndex, owner.controller.ProjectCandidateForView(*pending, target));
					}
				}
				for(auto& [index, proposed] : views)
					if(sealedPublication->presentationInputs[index].output.kind == RenderOutputKind::SceneView)
						proposed = ProjectSceneViewGraphicsSettings(proposed);
				std::lock_guard lock(owner.mutex);
				for(const auto& [index, proposed] : views)
				{
					const auto color = ResolvePostProcessColorSettings(proposed.values);
					PostProcessEffectSettings effects;
					if(ResolvePostProcessEffectSettings(proposed.values, effects) != GraphicsRequestReason::None)
					{
						prepared = false;
						reason = "The requested camera effect values could not be resolved.";
						break;
					}
					const auto off = TerraEngine::EMPTY_UUID;
					for(const auto& id : {color.grading ? color.curve : off, color.grading ? color.lut : off,
						effects.lensDirt.Active() ? effects.lensDirt.texture : off})
					{
						if(id == TerraEngine::EMPTY_UUID || resourceManager.AcquireTextureSnapshot(id)) continue;
						auto load = std::find_if(owner.pendingPostProcessLookups.begin(), owner.pendingPostProcessLookups.end(),
							[&](const auto& entry) { return entry.first == id; });
						if(load == owner.pendingPostProcessLookups.end())
						{
							const auto path = resourceManager.GetRegisteredAssetPath(id, AssetType::Texture);
							if(path.empty())
							{
								prepared = false;
								reason = "A requested post-processing texture asset is unregistered.";
								break;
							}
							// The existing worker loader owns texture decoding. Poll its
							// receipt on the next frame without blocking this boundary.
							owner.pendingPostProcessLookups.emplace_back(id, resourceManager.LoadTextureAsync(path));
							load = std::prev(owner.pendingPostProcessLookups.end());
						}
						if(!load->second.valid() || (load->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready
							&& load->second.get() != id))
						{
							prepared = false;
							reason = "A requested post-processing texture asset could not be loaded.";
							break;
						}
						waitingForAssets = true;
					}
					if(!prepared) break;
				}
			}
			if(!waitingForAssets || !prepared)
			{
				bool qualifiedPostProcess = false;
				bool waitingForGeometry = false;
				if(prepared)
					for(const auto& [index, proposed] : views)
					{
						const auto settings = TerraEngine::Renderer::BuildPostProcessGraphSettings(proposed);
						GraphicsAAConfiguration raster;
						if (ResolveGraphicsAAConfiguration(proposed.values, raster) == GraphicsRequestReason::None
							&& raster.raster == GraphicsGeometryRasterMode::Indexed)
						{
							const bool ready = vulkan->PrepareIndexedRasterCandidate(*sealedPublication, index,
									proposed, reason, waitingForGeometry);
							if (waitingForGeometry) break;
							if (!ready) { prepared = false; break; }
							qualifiedPostProcess = true;
						}
						if(!settings.hdrCamera && !settings.toneAndGrade && !settings.spatialAA && !settings.coverageAA
							&& !settings.presentationEffects && !settings.temporalResolve && !settings.screenSpaceLighting && !settings.outputDithering) continue;
						if(settings.spatialAA)
						{
							const auto actual = std::find_if(recorded.begin(), recorded.end(), [&](const auto& view) {
								return view.publicationIndex == index;
							});
							if(actual == recorded.end()
								|| (vulkan->ReadSpatialAAModeMask({actual->width, actual->height}) & (1u << settings.antiAliasingMode)) == 0)
							{
								prepared = false;
								failure = GraphicsRequestReason::Unsupported;
								reason = "The requested spatial AA mode does not support an affected native view.";
								break;
							}
						}
						if(!vulkan->PreparePostProcessCandidate(sealedPublication->presentationInputs[index], proposed, reason))
						{
							prepared = false;
							break;
						}
						qualifiedPostProcess = true;
					}
				if(!reason.empty() && !waitingForGeometry)
					TERRAENGINE_LOG(LogLevel::Warning, "Graphics settings resource preparation failed: " << reason);
				if((!prepared || waitingForGeometry) && vulkan) vulkan->DiscardPostProcessCandidates();
				if(!waitingForGeometry && (!views.empty() || !prepared))
					CompleteGraphicsSettingsResourcePreparation(pending->requestId, pending->lifecycle, prepared,
						prepared && qualifiedPostProcess, failure);
			}
		}
	}
	else if(vulkan && sealedPublication)
	{
		// Scene work shares native candidate storage with Game. Only one owner
		// prepares and publishes in a given protected transaction.
		if(const auto pendingScene = GetPendingSceneViewGraphicsSettingsResourcePreparation())
		{
			const auto scene = std::find_if(recorded.begin(), recorded.end(), [&](const auto& row) {
				return sealedPublication->presentationInputs[row.publicationIndex].output.kind == RenderOutputKind::SceneView;
			});
			if(scene != recorded.end())
			{
				const auto plan = TerraEngine::Renderer::BuildPostProcessGraphSettings(pendingScene->snapshot);
				GraphicsAAConfiguration raster;
				const bool indexed = ResolveGraphicsAAConfiguration(pendingScene->snapshot.values, raster) == GraphicsRequestReason::None
					&& raster.raster == GraphicsGeometryRasterMode::Indexed;
				const bool compatible = !plan.spatialAA
					|| (vulkan->ReadSpatialAAModeMask({scene->width, scene->height}) & (1u << plan.antiAliasingMode)) != 0u;
				std::string failure;
				bool waitingForGeometry = false;
				const bool geometryReady = !indexed || (compatible && vulkan->PrepareIndexedRasterCandidate(
					*sealedPublication, scene->publicationIndex, pendingScene->snapshot, failure, waitingForGeometry));
				const bool prepared = compatible && geometryReady && vulkan->PreparePostProcessCandidate(
					sealedPublication->presentationInputs[scene->publicationIndex], pendingScene->snapshot, failure);
				if (!waitingForGeometry)
				{
					if(!failure.empty()) TERRAENGINE_LOG(LogLevel::Warning, failure);
					if(!prepared) vulkan->DiscardPostProcessCandidates();
					CompleteSceneViewGraphicsSettingsResourcePreparation(pendingScene->requestId, pendingScene->lifecycle,
						prepared, prepared && (indexed || plan.hdrCamera || plan.toneAndGrade || plan.spatialAA
							|| plan.coverageAA || plan.presentationEffects || plan.temporalResolve || plan.outputDithering),
						compatible ? GraphicsRequestReason::PreparationFailed : GraphicsRequestReason::Unsupported);
				}
			}
		}
	}
	TerraEngine::Renderer::ApplyVirtualGeometryCapabilityDiagnostics(
		renderGraphDiagnostics,
		CollectVirtualGeometryCapabilityDiagnostics(
			*rhi,
			backendCapabilities,
			{
				activeRenderFrameId,
				renderFrameSnapshot.viewRequests.front().outputId,
				snapshotRendererGeneration
			}));

	return sealedPublication;
}
