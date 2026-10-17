// RenderSystem diagnostics facade, preserving existing protected ownership and publication boundaries.
#include "Frame/RenderFrameState.h"
#include "Renderer/RHI/RHIContext.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

RenderStatsSnapshot RenderSystem::GetLatestRenderStats(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);

	if (viewType == RenderViewType::Scene)
	{
		return latestSceneStats;
	}
	return latestGameStats;
}

TerraEngine::Scene::SpatialDiagnosticsSnapshot RenderSystem::GetLatestSpatialDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);

	if (viewType == RenderViewType::Scene)
	{
		return latestSceneSpatialDiagnostics;
	}
	return latestGameSpatialDiagnostics;
}

FrameBudgetDiagnosticsSnapshot RenderSystem::GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);

	if (viewType == RenderViewType::Scene)
	{
		return latestSceneBudgetDiagnostics;
	}
	return latestGameBudgetDiagnostics;
}

LightingDiagnosticsSnapshot RenderSystem::GetLatestLightingDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);

	if (viewType == RenderViewType::Scene)
	{
		return latestSceneLightingDiagnostics;
	}
	return latestGameLightingDiagnostics;
}

RenderSnapshotDiagnosticsSnapshot RenderSystem::GetLatestRenderSnapshotDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderSnapshotDiagnostics;
}

TerraEngine::Renderer::RendererRecoverySnapshot RenderSystem::GetLatestRendererRecoverySnapshot() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRendererRecoverySnapshot;
}

TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
RenderSystem::GetLatestAnimationGpuBackendExecution() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestAnimationGpuBackendExecution;
}

std::vector<TerraEngine::Renderer::RenderFrameRenderableRecord>
RenderSystem::GetLatestRenderablesSnapshot() const
{
	// The protected renderer owns the source snapshot. Copying while holding the
	// statistics mutex gives editor/MCP readers one internally consistent frame
	// and prevents them from retaining references into the next render update.
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderFrameSnapshot.renderables;
}

std::size_t RenderSystem::GetLatestAnimatedGeometryPageDemandCount() const
{
	// Read the same immutable frame publication as renderable diagnostics so the
	// count cannot be paired with a newer or partially prepared demand list.
	std::lock_guard<std::mutex> lock(statsMutex);
	return std::max(latestRenderFrameSnapshot.animatedGeometryPageDemandCount,
		latestRenderFrameSnapshot.animatedGeometryPageDemands.size());
}

std::vector<GpuVirtualGeometryPacketRecord>
RenderSystem::GetLatestVirtualGeometryVisibilityRecords(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	if(viewType == RenderViewType::Scene)
	{
		return latestSceneVirtualGeometryVisibilityRecords;
	}
	return latestGameVirtualGeometryVisibilityRecords;
}

std::vector<TerraEngine::Renderer::OcclusionDebugSample> RenderSystem::GetLatestVirtualGeometryOcclusionDebugSamples(RenderViewType viewType, std::size_t maxCount) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	std::vector<TerraEngine::Renderer::OcclusionDebugSample> samples;
	const std::size_t cappedCount = std::min<std::size_t>(maxCount, TerraEngine::Renderer::kVirtualGeometryOcclusionDebugSampleCapacity);
	for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : latestRenderGraphDiagnostics.virtualGeometryViews)
	{
		if (view.viewType != viewType)
		{
			continue;
		}
		for (const TerraEngine::Renderer::OcclusionDebugSample& sample : view.occlusionDebugSamples)
		{
			if (samples.size() >= cappedCount)
			{
				return samples;
			}
			samples.push_back(sample);
		}
	}
	return samples;
}

TerraEngine::Renderer::RenderDiagnosticsSummary RenderSystem::GetLatestRenderGraphDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	auto result = latestRenderGraphDiagnostics;
	TerraEngine::Renderer::ApplyFrameScratchDiagnostics(result,
		latestFrameScratch, previousFrameScratch, latestFrameScratchReclamation);
	return result;
}

void RenderSystem::SetFrameScratchValidationCallbacks(
	std::shared_ptr<const TerraEngine::Renderer::RenderFrameScratchValidationCallbacks> callbacks)
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
#if !defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	if(callbacks) throw std::logic_error("Native renderer scratch validation is unavailable in this build");
#endif
	frameScratchValidationCallbacks = std::move(callbacks);
}

TerraEngine::Renderer::RenderFrameScratchValidationState RenderSystem::GetFrameScratchValidationState() const
{
	std::lock_guard lock(statsMutex);
	TerraEngine::Renderer::RenderFrameScratchValidationState result;
	result.active = observedFrameScratchOwner && observedFrameScratchOwner->IsActive();
	result.latest = latestFrameScratch;
	result.previous = previousFrameScratch;
	result.reclamation = latestFrameScratchReclamation;
	return result;
}

std::vector<TerraEngine::Renderer::BackendStageTiming> RenderSystem::GetLatestBackendStageTimings() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderGraphDiagnostics.backendFrameBreakdown.stageTimings;
}
