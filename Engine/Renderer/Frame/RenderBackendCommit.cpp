// Protected backend commit preserves streaming publication, BeginFrame, consume, render and EndFrame ordering.
#include "RenderFrameSealing.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
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
	if(!rhi->ConsumeRenderFramePublication(renderFramePublicationStore.Load()))
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Render backend rejected frame " << activeRenderFrameId
				<< " immutable publication before command recording.");
	}
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
		sealedPublication = renderFramePublicationStore.Load();
	for(std::uint32_t publicationViewIndex = 0u;
		sealedPublication != nullptr
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
