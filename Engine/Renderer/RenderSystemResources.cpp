// RenderSystem resources facade, preserving existing protected ownership and publication boundaries.
#include "Frame/RenderFrameState.h"
#include "Renderer/RHI/RHIContext.h"
#include "Resource/ResourceManager.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Core/Logger/Logger.h"
#include <algorithm>
#include <utility>

void RenderSystem::ReloadMaterial(const TerraEngine::UUID& materialId)
{
	ResourceManager::Instance().ReloadMaterial(materialId);
	// Surface runtime-contract fallbacks during hot reload so artists can see
	// when a material is rendering with authored defaults instead of textures.
	if (const Material* material = ResourceManager::Instance().GetMaterial(materialId))
	{
		for (const std::string& warning : material->fallbackWarnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Material " << TerraEngine::ToHexString(materialId) << ": " << warning);
		}
	}

	if (rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

bool RenderSystem::ReloadEditedMaterial(
	const TerraEngine::UUID& materialId,
	const std::filesystem::path& authoredPath)
{
	// The Inspector has already atomically validated and published authoredPath.
	// Replace the live CPU value first, then retire the GPU slot so no frame can
	// rebuild from the previous material generation.
	if(!ResourceManager::Instance().ReloadMaterialFromAuthoredPath(
		materialId, authoredPath))
	{
		return false;
	}
	if(rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
	return true;
}

void RenderSystem::ReloadMesh(const TerraEngine::UUID& meshId)
{
	ResourceManager::Instance().ReloadMesh(meshId);

	if (rhi)
	{
		rhi->InvalidateMesh(meshId);
	}
}

void RenderSystem::InvalidateGeneratedMesh(const TerraEngine::UUID& meshId)
{
	if(rhi && meshId != TerraEngine::EMPTY_UUID)
	{
		rhi->InvalidateMesh(meshId);
	}
}

void RenderSystem::InvalidateGeneratedTexture(const TerraEngine::UUID& textureId)
{
	if(rhi && textureId != TerraEngine::EMPTY_UUID)
	{
		rhi->InvalidateTexture(textureId);
	}
}

void RenderSystem::InvalidateGeneratedMaterial(const TerraEngine::UUID& materialId)
{
	// Generated terrain materials have no disk path for ReloadMaterial. Dropping
	// their backend slot makes the next visible draw rebuild MaterialData from the
	// freshly published CPU material and terrain runtime-layer binding.
	if(rhi && materialId != TerraEngine::EMPTY_UUID)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

void RenderSystem::ReloadTexture(const TerraEngine::UUID& textureId)
{
	auto future = ResourceManager::Instance().ReloadTexture(textureId);
	// Wait for the background transcode to finish before we swap the GPU resource
	future.get();

	if (rhi)
	{
		rhi->InvalidateTexture(textureId);
	}
}

void RenderSystem::DeleteResource(const TerraEngine::UUID& id)
{
	if (rhi)
	{
		rhi->WaitIdle();
		rhi->InvalidateResource(id);
	}
}

void RenderSystem::RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName)
{
	if (rhi)
	{
		// Residency cleanup is a cache-pressure path, not an editor delete.
		// Backends can defer physical destruction until submitted frames no
		// longer reference the resource instead of forcing a device-wide idle.
		rhi->RetireLoadedResource(id, frameId, debugName);
	}
}

void RenderSystem::Clear()
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	RetireCaptureJobs();
	RetireFrameExecutionAtProtectedBoundary();
	if(lifetimeState->frameScratch) lifetimeState->frameScratch->Retire();
	ObserveFrameScratchAtProtectedBoundary();
	{
		std::lock_guard lock(statsMutex);
		observedFrameScratchOwner.reset();
	}
	if (rhi)
	{
		rhi->ClearResources();
		DrainCaptureReadbacks();
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = {};
		latestSceneStats = {};
		latestGameBudgetDiagnostics = {};
		latestSceneBudgetDiagnostics = {};
		latestGameLightingDiagnostics = {};
		latestSceneLightingDiagnostics = {};
		latestRenderSnapshotDiagnostics = {};
		latestRendererRecoverySnapshot = TerraEngine::Renderer::MakeActiveRecoverySnapshot();
		latestAnimationGpuBackendExecution = {};
		latestRenderGraphDiagnostics = {};
		latestRenderFrameSnapshot = {};
		recycledRenderFrameSnapshot = {};
		latestGameVirtualGeometryVisibilityRecords.clear();
		latestSceneVirtualGeometryVisibilityRecords.clear();
		latestGameCaptureVisibilityRecords.clear();
		latestSceneCaptureVisibilityRecords.clear();
		latestGameCaptureIdentitySnapshot = {};
		latestSceneCaptureIdentitySnapshot = {};
		pendingGameIdentitySnapshot = {};
		pendingSceneIdentitySnapshot = {};
		pendingGameIdentityVisibilityRecords.clear();
		pendingSceneIdentityVisibilityRecords.clear();
		latestSceneAuthoritativeIdentityPick.reset();
		latestGameAuthoritativeIdentityPick.reset();
		pendingSceneAuthoritativeIdentityPixel.reset();
		pendingGameAuthoritativeIdentityPixel.reset();
		latestSceneAuthoritativeIdentityFromCapture = false;
		latestGameAuthoritativeIdentityFromCapture = false;
		pendingSceneAuthoritativeIdentityFromCapture = false;
		pendingGameAuthoritativeIdentityFromCapture = false;
		latestGameStats.viewType = RenderViewType::Game;
		latestSceneStats.viewType = RenderViewType::Scene;
		latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
		latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
		latestGameLightingDiagnostics.viewType = RenderViewType::Game;
		latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
		latestSkyRuntimeState = {};
		captureQueueState.pendingCaptureCount = 0;
		captureQueueState.completedResultCount = 0;
		// Retained native operations keep their physical charge across Clear.
		pendingCaptureRequests.clear();
		submittedCaptureReadbackRequests.clear();
		virtualGeometryDebugModes.clear();
	}
	hasLatestGameViewCamera = false;
	latestGameViewCamera = {};
	latestGameViewTargetSize = glm::uvec2(0u);
	latestGameViewFrameId = 0u;
	virtualGeometryTransitionCache.clear();
	geometryRoutePolicyStates.clear();
	latestGeometryRouteFramePlans.clear();
	latestGeometryRouteFramePlanSignature = 0U;
	latestGeometryRouteSelectionSignature = 0U;
	latestGeometryRouteSelectionCount = 0U;
	virtualGeometryViewPolicies.clear();
	virtualGeometryReplayReadbacks.Clear();
	latestVirtualGeometryReplayArtifacts.clear();
	// Scene clearing invalidates every stable slot lifetime.  The next render
	// update reopens the store under the backend's current recovery generation.
	renderFramePublicationStore.ResetForSession(
		renderFramePublicationStore.SessionGeneration() + 1u);
	retainedPersistentTopologyRoot.reset();
	retainedDynamicStateRoot.reset();
	retainedCpuDynamicPageIndices.clear();
	dynamicPageRecyclePool.Clear();
	retainedDynamicStateSourceSignature = 0u;
	lifetimeState = std::make_unique<RenderSystemLifetimeState>();
	compatibleAnimationPoseCache.clear();
	renderableMembershipCache.clear();
	retainedRenderableEntries.clear();
	retainedRuntimeMeshResources.clear();
	retainedRuntimeMaterialResources.clear();
	retainedRuntimeMeshOwners.clear();
	retainedRuntimeMaterialOwners.clear();
	retainedMeshLocalBounds.clear();
	retainedRenderableCaptureSourceSignature = 0U;
	retainedRenderableEntriesReusable = false;
	renderableMembershipSceneGeneration = 0U;
	renderableMembershipComponentRevision = 0U;
	renderableMembershipHierarchyRevision = 0U;
	renderableMembershipActiveRevision = 0U;
	renderableMembershipPacketRevision = 0U;
	renderableMembershipTerrainProxyGeneration = 0U;
	TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Clear();
	renderableWorldPoseCache.clear();
	renderableModelMatrixSceneGeneration = 0U;
	renderableModelMatrixSpatialRevision = 0U;
	renderableModelMatrixComponentRevision = 0U;
	renderableModelMatrixHierarchyRevision = 0U;
	worldTransformCache.Clear();
	latestTransformCacheDiagnostics = {};
	skyStateDirty = true;
	hasCachedSkyObjectResolution = false;
	cachedSkyObject.reset();
	cachedSkyHierarchyRevision = 0U;
	cachedSkyCandidateSignature = 0U;
	lastLoggedSkyIssueKey.clear();
	ClearOverrideCameras();
}

void RenderSystem::SetTaskShaderCullingEnabled(bool enabled)
{
	taskShaderCullingEnabled = enabled;

	if (rhi)
	{
		rhi->SetGlobalTaskShaderCullingEnabled(enabled);
	}
}
