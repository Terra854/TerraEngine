#include "Frame/RenderFrameSealing.h"
#include "Frame/RenderViewPreparation.h"
#include "Frame/RenderPreparationTasks.h"
// RenderSystem.cpp — Render System Implementation
//
// Implements the RenderSystem class, coordinating the high-level rendering flow for each frame.
// It handles the transition between different rendering states, manages the active camera, and integrates UI and scene drawing.
// The implementation ensures that scene data is correctly prepared and submitted to the RHI for GPU execution.

#include "RenderSystem.h"
#include "Frame/RenderFrameState.h"
#include "Frame/RenderCaptureOperationState.h"
#include "Frame/RenderScratchEpisode.h"
#include "Frame/RenderFrameCapture.h"
#include "Frame/RenderSnapshotPreparation.h"
#include "Frame/RenderVirtualGeometrySelection.h"
#include "Renderer/SyntheticCapture.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "UI/UISystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/ShadowWorkPlan.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureShadowBatchControls.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include "Core/FatalErrorReport.h"
#include "Core/Logger/Logger.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Window/WindowSystem.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/TerrainComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyComponent.h"
#include "Components/AnimatorComponent.h"
#include "Animation/AnimationSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SceneManager.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Utilities/PlatformUtils.h"
#include "ThreadPool/ThreadPool.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "UI/UISystem.h"
#include <entt/entt.hpp>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
#include <chrono>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <coroutine>
#include <exception>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace
{
	// Reserve one process-lifetime emergency owner before this singleton can
	// admit native work. Final static teardown cannot safely retry a failed idle
	// proof after window/logger teardown; the OS releases this exceptional owner.
	std::unique_ptr<RHIContext>& FinalCaptureBackendOwner()
	{
		static auto* owner = new std::unique_ptr<RHIContext>();
		return *owner;
	}














	// These facts are copied from the immutable, loader-validated TEMFPRD1
	// directory before route preparation.  Route selection must not use optional
	// visibility bounds as a proxy for geometry: bounds can be intentionally
	// omitted while the canonical runtime layout and production descriptors stay
	// complete.






























































































































	Transform ReflectTransformComponent(const Transform& transform)
	{
		// Render collection runs as protected main-thread work after scripting,
		// so it copies the canonical pose and bounded parent-relative pose directly. Avoiding the
		// reflection getter here removes repeated property lookup, variant
		// wrapping, and shared-lock overhead from every visible object each
		// frame.
		return transform;
	}






















}

namespace {
	// A nonblocking admission gate also detects same-thread reentry. Its owner
	// lives on RenderSystem, so replacing per-scene state cannot replace the gate.

}

// A render-frame coroutine keeps stack-owned immutable preparation state alive
// between scheduler callbacks. It never schedules itself: the scheduler resumes
// it at declared protected/worker boundaries, preserving normal exception flow.

RenderSystem& RenderSystem::Instance()
{
	// Provide a singleton instance so the main engine loop can register the
	// system without managing heap allocations or ownership semantics.
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem()

	: lifetimeState(std::make_unique<RenderSystemLifetimeState>()),
	objManager(ObjectManager::instance())
{
	(void)FinalCaptureBackendOwner();
	latestGameStats.viewType = RenderViewType::Game;
	latestSceneStats.viewType = RenderViewType::Scene;
	latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
	latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
	latestGameLightingDiagnostics.viewType = RenderViewType::Game;
	latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
	latestRendererRecoverySnapshot = TerraEngine::Renderer::MakeActiveRecoverySnapshot();
}

// The operation record exists before queue admission. Worker tasks never capture
// this record or RenderSystem; they own only copied synthetic preparation input.


RenderSystem::~RenderSystem()
{
	// Final teardown must not allocate a retirement batch. Detach one already
	// owned record at a time and join CPU captures outside diagnostics locks.
	for (;;) {
		std::shared_ptr<CaptureOperation> retired;
		{
			std::lock_guard lock(statsMutex);
			if (captureOperations.empty()) break;
			auto iterator = captureOperations.begin();
			retired = std::move(iterator->second);
			captureOperations.erase(iterator);
		}
		retired->synthetic.Join();
	}
	// Explicit Shutdown normally empties rhi. If its final native proof failed,
	// move without allocation or another Vulkan/logging call into the singleton's
	// preallocated emergency owner; never destroy an unproven native context.
	if (rhi && !rhi->IsShutdownComplete()) FinalCaptureBackendOwner() = std::move(rhi);
}

void TerraEngine::Renderer::SyntheticCaptureTestAccess::SetNextCheckpoint(RenderSystem& renderer,
	std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint)
{
	std::lock_guard lock(renderer.statsMutex);
	renderer.nextSyntheticCaptureCheckpoint = std::move(checkpoint);
}

bool TerraEngine::Renderer::SyntheticCaptureTestAccess::IsRetiringCaptures(RenderSystem& renderer)
{
	std::lock_guard lock(renderer.statsMutex);
	return renderer.retiringCaptures;
}

void RenderSystem::SetSceneViewContextTint(const std::unordered_set<TerraEngine::UUID>& focusedObjectIds)
{
	// The editor calls this once per Scene View frame while prefab context mode
	// is active. An empty set disables the cue so stale selections do not tint
	// unrelated views on later frames.
	sceneViewContextFocusedObjectIds = focusedObjectIds;
	sceneViewContextTintEnabled = !sceneViewContextFocusedObjectIds.empty();
}

void RenderSystem::SetRendererRecoveryPolicy(const TerraEngine::Renderer::RendererRecoveryPolicy& policy)
{
	rendererRecoveryPolicy = policy;
}

TerraEngine::Renderer::RendererRecoveryPolicy RenderSystem::GetRendererRecoveryPolicy() const
{
	return rendererRecoveryPolicy;
}

void RenderSystem::ReportFatalRendererRecoveryFailure(TerraEngine::Renderer::RendererRecoverySnapshot snapshot)
{
	if (!rhi || !snapshot.fatalDialogReportId.empty())
	{
		return;
	}

	const bool exportRuntime = ResourceManager::Instance().IsExportRuntime();
	TerraEngine::FatalErrorDialogReport report = TerraEngine::BuildFatalErrorDialogReport(
		exportRuntime ? TerraEngine::FatalErrorCategory::ExportFatal : TerraEngine::FatalErrorCategory::RendererRecoveryFailed,
		rendererRecoveryPolicy.fatalMessage,
		std::string("In-process Vulkan device recreation is unavailable after graphics device loss. First failure stage=")
			+ TerraEngine::Renderer::ToString(snapshot.firstFailureStage)
			+ ", result="
			+ snapshot.firstFailureResult
			+ ", lastError="
			+ snapshot.lastError);
	report.incidentId = snapshot.incidentId;
	report.recoverySessionId = snapshot.sessionId;
	const std::string crashProductName = exportRuntime
		? ResourceManager::Instance().GetProductName()
		: std::string(TerraEngine::kEditorFatalErrorProductName);
	report = TerraEngine::PlatformUtils::WriteCrashDumpForReport(report, crashProductName);

	TerraEngine::Renderer::RendererRecoverySnapshot reported = TerraEngine::Renderer::AttachFatalDialogReport(
		snapshot,
		report.reportId,
		report.crashDumpPath.generic_string(),
		report.crashSummaryPath.generic_string(),
		TerraEngine::ToString(report.crashDumpStatus));
	reported.fatalDialogShown = true;
	if (exportRuntime)
	{
		reported.state = TerraEngine::Renderer::RendererRecoveryStateKind::FatalExportFailure;
		reported.finalOutcome = TerraEngine::Renderer::RendererRecoveryOutcome::FatalExportFailure;
	}
	rhi->PublishRendererRecoverySnapshot(reported);
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererRecoverySnapshot = reported;
	}
	// The platform dialog returns only after OK is acknowledged, then exits with
	// a failure code without entering Vulkan's ordinary wait-idle teardown.
	WindowSystem::Instance().ShowFatalErrorDialog(report);
}

bool RenderSystem::RequestsEngineShutdown() const
{
	// Editors keep their process alive so their renderer-recovery UI can remain
	// available. A packaged game instead terminates after its fatal dialog has
	// published the final incident report and crash-dump paths.
	if (!initialized || !ResourceManager::Instance().IsExportRuntime())
	{
		return false;
	}

	const TerraEngine::Renderer::RendererRecoverySnapshot recovery = GetLatestRendererRecoverySnapshot();
	if (recovery.state != TerraEngine::Renderer::RendererRecoveryStateKind::FatalExportFailure)
	{
		return false;
	}

	TERRAENGINE_LOG(LogLevel::Critical, "Export runtime stopping after graphics device loss. incident="
		<< recovery.incidentId
		<< " report="
		<< recovery.fatalDialogReportId
		<< " dump="
		<< recovery.crashDumpPath);
	return true;
}

std::vector<TerraEngine::Renderer::Terrain::TerrainPreparedRenderPacket>
RenderSystem::PrepareCommittedTerrainPackets(
	std::span<const TerraEngine::Renderer::Terrain::TerrainViewPacket> views,
	std::span<const TerraEngine::Renderer::Terrain::TerrainPageCookResult> cooks,
	std::span<const TerraEngine::Resource::VirtualGeometryPagePublication> committedPages)
{
	std::vector<TerraEngine::Renderer::Terrain::TerrainPreparedRenderPacket> packets;
	packets.reserve(views.size());
	const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> committedSnapshot(
		committedPages.begin(), committedPages.end());
	for (const TerraEngine::Renderer::Terrain::TerrainViewPacket& view : views)
	{
		// A terrain view carries the authored root identity captured by the
		// protected terrain snapshot. Select only a cook with the same identity;
		// this prevents an old worker result from being mixed into a new frame.
		const auto cook = std::find_if(cooks.begin(), cooks.end(), [&view](const auto& candidate)
		{
			return candidate.assetId == view.assetId
				&& candidate.area == view.area
				&& candidate.authoredHash == view.authoredHash
				&& candidate.authoredGeneration == view.authoredGeneration;
		});
		if (cook != cooks.end())
		{
			if (const auto prepared = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, *cook, committedSnapshot))
				packets.push_back(*prepared);
			continue;
		}

		// No current cook exists. Pass an intentionally incomplete value to the
		// packet helper, which can select a matching committed last-known-good page
		// but will not fabricate a fresh candidate from stale data.
		TerraEngine::Renderer::Terrain::TerrainPageCookResult missingCook{};
		if (const auto fallback = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, missingCook, committedSnapshot))
			packets.push_back(*fallback);
	}
	return packets;
}

TerraEngine::Renderer::Terrain::TerrainAreaRenderPacketResult
RenderSystem::PrepareCommittedTerrainAreaPackets(
	const TerraEngine::Renderer::Terrain::TerrainAssetRenderSnapshot& snapshot,
	std::span<const TerraEngine::Renderer::Terrain::TerrainAreaRenderRequest> requests,
	std::span<const TerraEngine::Renderer::Terrain::TerrainPageResidencyEntry> residency)
{
	// The pure packet builder owns route parity, own-ancestor fallback, page
	// demand coalescing, and all-or-nothing cut admission. Keeping this wrapper on
	// RenderSystem makes the production frame path and focused tests share it.
	return TerraEngine::Renderer::Terrain::PrepareTerrainAreaRenderPackets(
		snapshot, requests, residency);
}

bool RenderSystem::SubmitPreparedTerrainPackets(
	std::span<const TerraEngine::Renderer::Terrain::TerrainPreparedRenderPacket> packets)
{
	if (packets.empty()) return true;
	// One batch is intentionally limited to one exact view/source/generation.
	// Callers split multi-view LOD choices before this boundary, preserving each
	// camera's independent cut while still giving VG a complete typed producer.
	const auto& first = packets.front();
	TerraEngine::Resource::TerrainVirtualGeometryProducerBatch batch{};
	batch.assetId = first.view.assetId;
	batch.sourceIdentity = first.view.pageSourceIdentity;
	batch.authoredGeneration = first.view.authoredGeneration;
	batch.viewId = first.view.viewId;
	batch.regionalRootPageId = first.view.pageId;
	batch.candidates.reserve(packets.size());
	for (const auto& packet : packets)
	{
		if (packet.usesLastKnownGood || packet.view.assetId != batch.assetId
			|| packet.view.pageSourceIdentity != batch.sourceIdentity
			|| packet.view.authoredGeneration != batch.authoredGeneration
			|| packet.view.viewId != batch.viewId) return false;
		batch.regionalRootPageId = std::min(batch.regionalRootPageId, packet.view.pageId);
		batch.candidates.push_back(packet.candidate);
	}
	return TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().SubmitTerrainProducerBatch(std::move(batch));
}









std::optional<VirtualGeometryVisibilityPick>
RenderSystem::PickVirtualGeometryVisibilityRecord(
	std::span<const GpuVirtualGeometryPacketRecord> records,
	const glm::vec2& normalizedViewportPosition)
{
	if(!std::isfinite(normalizedViewportPosition.x)
		|| !std::isfinite(normalizedViewportPosition.y)
		|| normalizedViewportPosition.x < 0.0f
		|| normalizedViewportPosition.x > 1.0f
		|| normalizedViewportPosition.y < 0.0f
		|| normalizedViewportPosition.y > 1.0f)
	{
		return std::nullopt;
	}

	const GpuVirtualGeometryPacketRecord* bestRecord = nullptr;
	float bestArea = std::numeric_limits<float>::max();
	for(const GpuVirtualGeometryPacketRecord& record : records)
	{
		if(record.objectId == TerraEngine::EMPTY_UUID
			|| record.meshId == TerraEngine::EMPTY_UUID
			|| record.frameId == 0u
			|| record.meshletCount == 0u
			|| record.projectedBounds == glm::vec4(0.0f))
		{
			continue;
		}

		const float minimumX = std::clamp(
			std::min(record.projectedBounds.x, record.projectedBounds.z),
			0.0f,
			1.0f);
		const float maximumX = std::clamp(
			std::max(record.projectedBounds.x, record.projectedBounds.z),
			0.0f,
			1.0f);
		const float minimumY = std::clamp(
			std::min(record.projectedBounds.y, record.projectedBounds.w),
			0.0f,
			1.0f);
		const float maximumY = std::clamp(
			std::max(record.projectedBounds.y, record.projectedBounds.w),
			0.0f,
			1.0f);
		if(normalizedViewportPosition.x < minimumX
			|| normalizedViewportPosition.x > maximumX
			|| normalizedViewportPosition.y < minimumY
			|| normalizedViewportPosition.y > maximumY)
		{
			continue;
		}

		const float area = (maximumX - minimumX) * (maximumY - minimumY);
		const bool winsStableTie = bestRecord != nullptr
			&& area == bestArea
			&& record.debugIdentity < bestRecord->debugIdentity;
		if(bestRecord != nullptr && area > bestArea && !winsStableTie)
		{
			continue;
		}
		if(bestRecord != nullptr && area == bestArea && !winsStableTie)
		{
			continue;
		}
		bestRecord = &record;
		bestArea = area;
	}

	if(bestRecord == nullptr)
	{
		return std::nullopt;
	}

	VirtualGeometryVisibilityPick pick;
	pick.objectId = bestRecord->objectId;
	pick.meshId = bestRecord->meshId;
	pick.materialId = bestRecord->materialId;
	pick.frameId = bestRecord->frameId;
	pick.packetId = bestRecord->packetId;
	pick.clusterId = bestRecord->clusterId;
	pick.meshletId = bestRecord->meshletOffset;
	pick.selectedLod = bestRecord->selectedLod;
	pick.debugIdentity = bestRecord->debugIdentity;
	pick.fallback = (bestRecord->flags & 0x1u) != 0u;
	return pick;
}

bool RenderSystem::RequestConventionalVisibilityIdentityPick(
	RenderViewType viewType, const glm::uvec2& pixel,
	bool dedicatedCaptureTarget)
{
	if (!rhi) return false;
	TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackRequest request{};
	request.viewType = viewType;
	request.pixel = pixel;
	request.dedicatedCaptureTarget = dedicatedCaptureTarget;
	const auto targetMetadata =
		rhi->GetConventionalVisibilityIdentityTargetMetadata(
			viewType, dedicatedCaptureTarget);
	if(!targetMetadata || !targetMetadata->IsComplete()) return false;
	CaptureConventionalVisibilityIdentitySnapshot requestSnapshot{};
	std::vector<GpuVirtualGeometryPacketRecord> requestVisibilityRecords;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		// Conventional typed identity is valid for direct meshes as well as VG
		// packets. A dedicated capture target can outlive ordinary Scene/Game
		// frames, so select its own retained immutable table instead of asking the
		// Vulkan target to match a newer normal-frame id.
		const CaptureConventionalVisibilityIdentitySnapshot* captureSnapshot =
			viewType == RenderViewType::Scene
				? &latestSceneCaptureIdentitySnapshot : &latestGameCaptureIdentitySnapshot;
		if(dedicatedCaptureTarget)
		{
			requestSnapshot = *captureSnapshot;
			requestVisibilityRecords = viewType == RenderViewType::Scene
				? latestSceneCaptureVisibilityRecords
				: latestGameCaptureVisibilityRecords;
		}
		else
		{
			requestSnapshot.frameId = latestRenderFrameSnapshot.frameId;
			requestSnapshot.rendererGeneration =
				latestRendererRecoverySnapshot.renderSessionGeneration;
			requestSnapshot.targetGeneration = targetMetadata->targetGeneration;
			requestSnapshot.originGeneration = targetMetadata->originGeneration;
			requestSnapshot.spatialGeneration = targetMetadata->spatialGeneration;
			requestSnapshot.residencyGeneration = targetMetadata->residencyGeneration;
			requestSnapshot.table =
				latestRenderFrameSnapshot.conventionalVisibilityIdentityTable;
			requestVisibilityRecords = viewType == RenderViewType::Scene
				? latestSceneVirtualGeometryVisibilityRecords
				: latestGameVirtualGeometryVisibilityRecords;
		}
		if (!requestSnapshot.IsUsable()
			|| requestSnapshot.frameId != targetMetadata->frameId
			|| requestSnapshot.rendererGeneration != targetMetadata->rendererGeneration
			|| requestSnapshot.targetGeneration != targetMetadata->targetGeneration
			|| requestSnapshot.originGeneration != targetMetadata->originGeneration
			|| requestSnapshot.spatialGeneration != targetMetadata->spatialGeneration
			|| requestSnapshot.residencyGeneration != targetMetadata->residencyGeneration) {
			return false;
		}
		request.frameId = requestSnapshot.frameId;
		request.rendererGeneration = requestSnapshot.rendererGeneration;
		request.targetGeneration = requestSnapshot.targetGeneration;
		request.originGeneration = requestSnapshot.originGeneration;
		request.spatialGeneration = requestSnapshot.spatialGeneration;
		request.residencyGeneration = requestSnapshot.residencyGeneration;
	}
	if (!rhi->QueueConventionalVisibilityIdentityReadback(request)) {
		std::lock_guard<std::mutex> lock(statsMutex);
		std::string& failureReason = viewType == RenderViewType::Scene
			? latestSceneAuthoritativeIdentityFailureReason
			: latestGameAuthoritativeIdentityFailureReason;
		failureReason = dedicatedCaptureTarget
			? "Vulkan rejected the dedicated capture identity request; the retained capture frame no longer matches its target."
			: "Vulkan rejected the viewport identity request; the rendered frame no longer matches its target.";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		// A fresh GPU request invalidates any older value for this view. The
		// replacement is visible only when DrainConventional... observes its fence.
		if (viewType == RenderViewType::Scene) {
			pendingSceneAuthoritativeIdentityPixel = pixel;
			pendingSceneAuthoritativeIdentityFromCapture = dedicatedCaptureTarget;
			pendingSceneIdentitySnapshot = std::move(requestSnapshot);
			pendingSceneIdentityVisibilityRecords = std::move(requestVisibilityRecords);
			latestSceneAuthoritativeIdentityPick.reset();
			latestSceneAuthoritativeIdentityFailureReason.clear();
		} else {
			pendingGameAuthoritativeIdentityPixel = pixel;
			pendingGameAuthoritativeIdentityFromCapture = dedicatedCaptureTarget;
			pendingGameIdentitySnapshot = std::move(requestSnapshot);
			pendingGameIdentityVisibilityRecords = std::move(requestVisibilityRecords);
			latestGameAuthoritativeIdentityPick.reset();
			latestGameAuthoritativeIdentityFailureReason.clear();
		}
	}
	return true;
}

std::optional<VirtualGeometryVisibilityPick>
RenderSystem::GetLatestAuthoritativeConventionalVisibilityIdentityPick(
	RenderViewType viewType, const glm::uvec2& pixel) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	const std::optional<VirtualGeometryVisibilityPick>& pick =
		viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPick : latestGameAuthoritativeIdentityPick;
	const glm::uvec2 storedPixel = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPixel : latestGameAuthoritativeIdentityPixel;
	if (storedPixel != pixel || !pick || !pick->IsValid()) return std::nullopt;
	return pick;
}

TerraEngine::Renderer::AuthoritativeVisibilityPickState
RenderSystem::GetConventionalVisibilityIdentityPickState(
	RenderViewType viewType, const glm::uvec2& pixel,
	bool dedicatedCaptureTarget) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	TerraEngine::Renderer::AuthoritativeVisibilityPickState state{};
	state.pixel = pixel;
	state.dedicatedCaptureTarget = dedicatedCaptureTarget;
	const std::optional<VirtualGeometryVisibilityPick>& completed =
		viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPick : latestGameAuthoritativeIdentityPick;
	const glm::uvec2 completedPixel = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPixel : latestGameAuthoritativeIdentityPixel;
	const bool completedFromCapture = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityFromCapture : latestGameAuthoritativeIdentityFromCapture;
	const std::optional<glm::uvec2>& pending = viewType == RenderViewType::Scene
		? pendingSceneAuthoritativeIdentityPixel : pendingGameAuthoritativeIdentityPixel;
	const bool pendingFromCapture = viewType == RenderViewType::Scene
		? pendingSceneAuthoritativeIdentityFromCapture : pendingGameAuthoritativeIdentityFromCapture;
	const std::string& failureReason = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityFailureReason : latestGameAuthoritativeIdentityFailureReason;
	if (completedPixel == pixel && completed && completed->IsValid()
		&& completedFromCapture == dedicatedCaptureTarget) {
		state.status = TerraEngine::Renderer::AuthoritativeVisibilityPickStatus::Ready;
		state.pick = completed;
		state.dedicatedCaptureTarget = completedFromCapture;
		return state;
	}
	if (pending && *pending == pixel && pendingFromCapture == dedicatedCaptureTarget) {
		state.status = TerraEngine::Renderer::AuthoritativeVisibilityPickStatus::Pending;
		state.reason = "The typed Vulkan identity copy is waiting for its frame fence.";
		return state;
	}
	state.status = TerraEngine::Renderer::AuthoritativeVisibilityPickStatus::Unavailable;
	state.reason = failureReason.empty()
		? "No authoritative Vulkan identity result is available for this pixel."
		: failureReason;
	return state;
}

std::optional<TerraEngine::Renderer::RenderReferenceFrame>
RenderSystem::GetLatestRenderReferenceFrame(RenderViewType viewType) const
{
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication =
		renderFramePublicationStore.Load();
	if (!publication)
		return std::nullopt;
	const std::size_t count = std::min(publication->views.size(), publication->referenceFrames.size());
	for (std::size_t index = 0U; index < count; ++index)
	{
		if (publication->views[index].viewType == viewType
			&& publication->referenceFrames[index].IsValid())
			return publication->referenceFrames[index];
	}
	return std::nullopt;
}

bool RenderSystem::SetVirtualGeometryValidationRouteOverride(
	TerraEngine::Renderer::VirtualGeometryRasterRoute route,
	std::string* outReason)
{
	if (route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback) {
		if (outReason) *outReason = "Fallback is not a validation route; request Indexed explicitly.";
		return false;
	}
	if (!rhi) {
		if (outReason) *outReason = "The Vulkan renderer is unavailable.";
		return false;
	}
	const TerraEngine::Renderer::BackendCapabilityProfile capabilities =
		rhi->GetBackendCapabilityProfile();
	const bool supported = route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed
		|| (route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware
			&& rhi->SupportsVirtualGeometryMeshShaderRoute())
		|| (route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute
			&& capabilities.supportsVirtualGeometryGpuPacketBuffers);
	if (!supported) {
		if (outReason) *outReason = "The active Vulkan adapter does not support the requested validation route.";
		return false;
	}
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	virtualGeometryValidationRouteOverride = route;
	virtualGeometryHybridValidationSceneRequested = false;
	if (outReason) outReason->clear();
	return true;
}

void RenderSystem::SetVirtualGeometryHybridValidationSceneRequested(bool requested)
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	// Hybrid remains an intentional mixture of real packet routes. It never
	// becomes a synthetic backend enum or a force-route value in Vulkan work.
	virtualGeometryValidationRouteOverride.reset();
	virtualGeometryHybridValidationSceneRequested = requested;
}

void RenderSystem::ClearVirtualGeometryValidationRouteOverride()
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	virtualGeometryValidationRouteOverride.reset();
	virtualGeometryHybridValidationSceneRequested = false;
}

std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
RenderSystem::GetVirtualGeometryValidationRouteOverride() const
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	return virtualGeometryValidationRouteOverride;
}

bool RenderSystem::IsVirtualGeometryHybridValidationSceneRequested() const
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	return virtualGeometryHybridValidationSceneRequested;
}





















const std::unordered_map<entt::entity, TerraEngine::Scene::WorldPose>& TerraEngine::Renderer::RenderWorldTransformCache::Refresh(
	ObjectManager& objects,
	TerraEngine::Renderer::RenderTransformCacheDiagnostics& diagnostics)
{
	// The render system owns this copied snapshot at its protected boundary. It
	// must never retain pointers into live component storage because worker and
	// editor mutations can replace that storage after the snapshot is prepared.
	diagnostics = {};
	const std::uint64_t observedSceneGeneration = objects.GetSceneGeneration();
	const std::uint64_t observedHierarchyRevision = objects.GetHierarchyRevision();
	const std::uint64_t observedSpatialRevision = objects.GetSpatialRevision();
	if (!initialized || sceneGeneration != observedSceneGeneration
		|| hierarchyRevision != observedHierarchyRevision)
	{
		graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects, false);
		graph.revision = observedSpatialRevision;
		flatHierarchy = std::all_of(graph.objects.begin(), graph.objects.end(), [](const auto& entry) {
			const TerraEngine::Scene::SceneGraphObject& object = entry.second;
			return object.parentId == TerraEngine::EMPTY_UUID && object.children.empty();
		});
		latestPropagation = TerraEngine::Scene::PropagateWorldTransforms(
			graph,
			TerraEngine::Scene::BuildDirtyTransformState(graph));

		worldTransforms.clear();
		worldTransforms.reserve(latestPropagation.worldTransformsByEntity.size());
		for (const auto& [entity, transform] : latestPropagation.worldTransformsByEntity)
		{
			worldTransforms.emplace(entity, transform);
		}

		initialized = true;
		sceneGeneration = observedSceneGeneration;
		hierarchyRevision = observedHierarchyRevision;
		spatialRevision = observedSpatialRevision;
		hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
		diagnostics.rebuiltForHierarchy = true;
		diagnostics.scannedObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.changedObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.propagatedObjectCount = latestPropagation.processedObjectCount;
		diagnostics.hierarchyLevelCount = hierarchyLevelCount;
		diagnostics.hierarchyRevision = hierarchyRevision;
		return worldTransforms;
	}
	if (spatialRevision == observedSpatialRevision)
	{
		// No spatial mutation can have changed a local Transform revision, so
		// avoid even the compact revision scan on a completely unchanged frame.
		latestPropagation = {};
		latestPropagation.graphRevision = graph.revision;
		latestPropagation.hierarchyLevelCount = hierarchyLevelCount;
		latestPropagation.skippedCleanObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.reusedWithoutPropagation = true;
		diagnostics.hierarchyLevelCount = hierarchyLevelCount;
		diagnostics.hierarchyRevision = hierarchyRevision;
		return worldTransforms;
	}

	// A stable hierarchy lets the protected render path inspect only compact
	// per-object transform revisions instead of rebuilding every parent/child
	// edge. Children of a changed object are expanded by the shared propagator.
	std::vector<TerraEngine::Scene::SceneObjectId> changedObjectIds;
	changedObjectIds.reserve(graph.objects.size());
	for (auto& [objectId, graphObject] : graph.objects)
	{
		const std::optional<Object> object = objects.getObject(graphObject.entity);
		if (!object)
		{
			// A removed entity without a hierarchy revision is inconsistent state.
			// Rebuild defensively so the renderer fails closed to a fresh snapshot.
			initialized = false;
			return Refresh(objects, diagnostics);
		}

		const std::uint64_t transformRevision = objects.GetObjectTransformRevision(*object);
		if (transformRevision == graphObject.transformRevision)
		{
			continue;
		}

		// Copy the current authored local transform into the immutable graph before
		// scheduling the dependency-aware world transform propagation.
		const Transform* localTransform = objects.GetComponent<Transform>(*object);
		const Transform local = localTransform != nullptr ? *localTransform : Transform{};
		graphObject.worldPose = local.worldPose;
		graphObject.parentRelativePose = local.parentRelativePose;
		graphObject.transformRevision = transformRevision;
		graphObject.spatialRevision = objects.GetObjectSpatialRevision(*object);
		changedObjectIds.push_back(objectId);
	}

	graph.revision = observedSpatialRevision;
	spatialRevision = observedSpatialRevision;
	diagnostics.scannedObjectCount = static_cast<std::uint32_t>(graph.objects.size());
	diagnostics.changedObjectCount = static_cast<std::uint32_t>(changedObjectIds.size());
	diagnostics.hierarchyLevelCount = hierarchyLevelCount;
	diagnostics.hierarchyRevision = hierarchyRevision;
	if (changedObjectIds.empty())
	{
		// Preserve explicit zero-work diagnostics so profiler and editor tools can
		// distinguish an intentional cache hit from a skipped render update.
		latestPropagation = {};
		latestPropagation.graphRevision = graph.revision;
		latestPropagation.hierarchyLevelCount = hierarchyLevelCount;
		latestPropagation.skippedCleanObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.reusedWithoutPropagation = true;
		return worldTransforms;
	}
	if (flatHierarchy)
	{
		// Physics stress fixtures commonly contain independent root bodies. Their
		// world transform is exactly the authored local transform, so rebuilding
		// generic traversal plans, UUID sort keys, and worker graphs per body adds
		// latency without adding correctness. Cache-map writes stay on this
		// protected coordinator thread to keep the published snapshot coherent.
		latestPropagation = {};
		latestPropagation.graphRevision = graph.revision;
		latestPropagation.hierarchyLevelCount = graph.objects.empty() ? 0U : 1U;
		latestPropagation.dirtyRootCount = static_cast<std::uint32_t>(changedObjectIds.size());
		latestPropagation.expandedDirtyObjectCount = static_cast<std::uint32_t>(changedObjectIds.size());
		latestPropagation.processedObjectCount = static_cast<std::uint32_t>(changedObjectIds.size());
		latestPropagation.skippedCleanObjectCount = static_cast<std::uint32_t>(graph.objects.size() - changedObjectIds.size());
		for (const TerraEngine::Scene::SceneObjectId& objectId : changedObjectIds)
		{
			const auto graphObject = graph.objects.find(objectId);
			if (graphObject == graph.objects.end())
			{
				continue;
			}

			TerraEngine::Scene::SceneGraphObject& object = graphObject->second;
			worldTransforms[object.entity] = object.worldPose;
		}

		hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
		diagnostics.usedFlatHierarchyFastPath = true;
		diagnostics.propagatedObjectCount = latestPropagation.processedObjectCount;
		diagnostics.hierarchyLevelCount = hierarchyLevelCount;
		return worldTransforms;
	}

	latestPropagation = TerraEngine::Scene::PropagateWorldTransforms(
		graph,
		TerraEngine::Scene::BuildDirtyTransformState(
			graph,
			changedObjectIds,
			TerraEngine::Scene::SceneTransformDirtyReason::LocalTransform));
	for (const auto& [entity, transform] : latestPropagation.worldTransformsByEntity)
	{
		worldTransforms[entity] = transform;
	}

	diagnostics.propagatedObjectCount = latestPropagation.processedObjectCount;
	diagnostics.hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
	hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
	return worldTransforms;
}

const TerraEngine::Scene::SceneTransformPropagationResult& TerraEngine::Renderer::RenderWorldTransformCache::LatestPropagation() const
{
	return latestPropagation;
}

void TerraEngine::Renderer::RenderWorldTransformCache::Clear()
{
	// Scene loads and play-mode restoration can replace every entity at once, so
	// discard both the copied graph and transforms before the next protected frame.
	graph = {};
	worldTransforms.clear();
	latestPropagation = {};
	initialized = false;
	sceneGeneration = 0U;
	hierarchyRevision = 0U;
	spatialRevision = 0U;
	hierarchyLevelCount = 0U;
	flatHierarchy = false;
}

TerraEngine::Renderer::AnimationRenderBinding RenderSystem::ResolveAnimationRenderBinding(
	const TerraEngine::Renderer::AnimationRenderBindingRequest& request) noexcept
{
	using namespace TerraEngine::Renderer;
	AnimationRenderBinding result;
	result.meshId = request.meshId;
	result.animatorObjectId = request.animatorObjectId;
	result.skeletonAssetId = request.skeletonAssetId;
	result.animatorObjectGeneration = request.animatorObjectGeneration;
	result.animationAuthorityGeneration = request.animationAuthorityGeneration;
	result.animatorSlotIndex = request.animatorSlotIndex;
	result.animatorGeneration = request.animatorGeneration;
	result.deformablePayloadGeneration = request.deformablePayloadGeneration;
	result.componentGeneration = request.componentGeneration;
	result.skeletonGeneration = request.skeletonGeneration;
	result.skeletonCompatibilityFingerprint = request.skeletonCompatibilityFingerprint;
	result.pageLocalDeformationAvailable =
		request.pageLocalDeformationAvailable;
	result.invalidation = request.invalidation;

	// A current safe/error publication is an explicit dependency result even if
	// its missing skeleton prevented the producer from filling every identity.
	// Report that result before generic identity validation and never consult held.
	if(request.authorityRow != nullptr
		&& (request.authorityRow->safeFallback
			|| request.authorityRow->status != TerraEngine::Animation::AnimationStatus::Ok)) {
		result.status = AnimationRenderBindingStatus::SafePose;
		return result;
	}

	// A Visual GPU animator publishes its packet independently from the CPU pose
	// stream. Accept it only after the packet's complete immutable identity has
	// matched this draw's copied mesh/resource/scene values; Hybrid deliberately
	// falls through and still requires the CPU-authoritative pose path below.
	if(request.authorityRow == nullptr && request.gpuVisualIdentity.has_value()
		&& GpuVisualIdentityMatchesRequest(request, *request.gpuVisualIdentity)) {
		const AnimationGpuRenderBinding& identity = *request.gpuVisualIdentity;
		result.animatorSlotIndex = identity.instanceSlot;
		result.animatorGeneration = identity.instanceGeneration;
		result.componentGeneration = identity.componentGeneration;
		result.skeletonGeneration = identity.skeletonGeneration;
		result.source = AnimationRenderPoseSource::GpuVisual;
		result.status = AnimationRenderBindingStatus::Ready;
		result.posePublicationGeneration = identity.workGeneration;
		result.invalidation.pose = identity.workGeneration;
		result.invalidation.morph = identity.workGeneration;
		result.safePose = false;
		result.gpu = identity;
		return result;
	}

	// Missing identity fields cannot name one compatible mesh/component/skeleton
	// bundle. Preserve the default bind-pose result without copying any pointer.
	if(request.meshId == TerraEngine::EMPTY_UUID
		|| request.animatorObjectId == TerraEngine::EMPTY_UUID
		|| request.skeletonAssetId == TerraEngine::EMPTY_UUID
		|| request.animatorObjectGeneration == 0U
		|| request.animationAuthorityGeneration == 0U
		|| request.animatorSlotIndex == TerraEngine::Animation::kInvalidAnimatorInstanceSlot
		|| request.animatorGeneration == 0U
		|| request.deformablePayloadGeneration == 0U
		|| request.componentGeneration == 0U
		|| request.skeletonGeneration == 0U
		|| request.skeletonCompatibilityFingerprint == 0U) {
		result.status = AnimationRenderBindingStatus::InvalidPose;
		return result;
	}

	if(request.authorityRow != nullptr && request.presentationFrame != nullptr) {
		result.status = ValidateAnimatorPosePublication(request, *request.authorityRow);
		if(result.status != AnimationRenderBindingStatus::Ready) {
			return result;
		}

		result.source = AnimationRenderPoseSource::CpuPose;
		result.posePublicationGeneration = request.authorityRow->snapshot->publicationGeneration;
		result.invalidation.pose = result.posePublicationGeneration;
		result.invalidation.morph = result.posePublicationGeneration;
		result.safePose = false;
		result.presentationFrame = request.presentationFrame;
		result.authorityRow = request.authorityRow;
		return result;
	}

	if(request.fallbackPolicy == AnimationRenderFallbackPolicy::HoldLastCompatiblePose
		&& HeldAnimationBindingMatchesRequest(request)) {
		result.source = AnimationRenderPoseSource::HeldCpuPose;
		result.status = AnimationRenderBindingStatus::Ready;
		result.posePublicationGeneration = request.heldBinding.posePublicationGeneration;
		result.invalidation.pose = request.heldBinding.invalidation.pose;
		result.invalidation.morph = request.heldBinding.invalidation.morph;
		result.safePose = false;
		result.presentationFrame = request.heldBinding.presentationFrame;
		result.authorityRow = request.heldBinding.authorityRow;
		return result;
	}

	result.status = AnimationRenderBindingStatus::MissingPose;
	return result;
}






void RenderSystem::Initialize()
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	RetireFrameExecutionAtProtectedBoundary();
	// A failed initialization or a stopped backend cannot advertise readiness to
	// a later frame. Initialization runs at the existing protected lifecycle edge.
	initialized = false;
	// A failed native idle proof leaves the old device and exact admission
	// ledger alive. Retry that owner before allocating a replacement backend.
	RetireCaptureJobs();
	if (!TryReleaseCaptureBackend()) throw std::runtime_error("Previous renderer backend still owns native readbacks or an unproven device; initialization is deferred.");
	TERRAENGINE_LOG(LogLevel::Info, "Initializing RenderSystem...");
	if (const auto startupSettings = ResourceManager::Instance().GetConfiguredRenderSettings()) {
		// Editor direct-project and export profiles are resolved before system
		// initialization. Do not rediscover settings from the process cwd here.
		SetRenderSettings(*startupSettings);
	} else {
		LoadSettings();
	}

	auto vulkanContext = std::make_unique<VulkanContext>();
	// Vulkan validation is controlled by project/export render settings rather
	// than by the compiler configuration. Request it before instance creation so
	// Release and packaged builds can use validation layers when developers turn
	// the profile flag on, while machines without the SDK still fall back cleanly.
	vulkanContext->SetValidationLayersRequested(ResourceManager::Instance().IsRenderValidationEnabled());
	// VRS is a render-pass attachment decision in Vulkan. Apply the startup
	// render profile before backend initialization so exported builds that
	// declare VRS disabled never create or load a shading-rate attachment.
		vulkanContext->SetVariableRateShadingEnabled(SanitizeRenderSettings(currentSettings).variableRateShading > 0u);
	rhi = std::move(vulkanContext);
	rendererRecoveryPolicy = ResourceManager::Instance().GetRendererRecoveryPolicy();
	rhi->Initialize(WindowSystem::Instance());

	WindowSystem::Instance().SetResizeCallback([](int width, int height) {
		try {
			auto& renderSystem = RenderSystem::Instance();

			if (auto* vulkanContext = dynamic_cast<VulkanContext*>(renderSystem.GetContext())) {
				vulkanContext->Resize(width, height);
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Error, "Exception in resize callback: " << e.what());
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Error, "Unknown exception in resize callback");
		}
	});

	initialized = true;
}


void RenderSystem::Update(float deltaTime)
{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	UpdateAtExecutionBoundary(deltaTime);
}

void RenderSystem::RetireFrameExecutionAtProtectedBoundary()
{
	std::unique_ptr<RenderFrameExecution> retired;
	{
		std::lock_guard lock(lifetimeState->framePhaseMutex);
		auto& phase = lifetimeState->framePhase;
		retired = std::move(phase.execution);
		phase.captureReady = phase.routeReady = phase.virtualGeometryReady = false;
		phase.backendComplete = phase.publicationComplete = false;
	}
	// Cancellation destroys collection owners before the lease, outside handoff
	// and diagnostics locks. The outer admission gate proves no Resume is active.
	retired.reset();
	ObserveFrameScratchAtProtectedBoundary();
}

void RenderSystem::ObserveFrameScratchAtProtectedBoundary()
{
	const auto owner = lifetimeState->frameScratch;
	if(!owner) return;
	const auto terminal = owner->LastSnapshot();
	const auto reclamation = owner->ReclamationSnapshot();
	TerraEngine::Renderer::RenderFrameScratchDiagnostics latest;
	TerraEngine::Renderer::RenderFrameScratchDiagnostics previous;
	TerraEngine::Renderer::RenderFrameScratchReclamationDiagnostics reclaimed;
	{
		std::lock_guard lock(statsMutex);
		observedFrameScratchOwner = owner;
		if(terminal.episodeId != 0U) {
			if(latestFrameScratch.lifetimeId != terminal.lifetimeId
				|| latestFrameScratch.episodeId != terminal.episodeId) {
				previousFrameScratch = latestFrameScratch;
			}
			latestFrameScratch = terminal;
			latestFrameScratchReclamation = reclamation;
		}
		TerraEngine::Renderer::ApplyFrameScratchDiagnostics(latestRenderGraphDiagnostics,
			latestFrameScratch, previousFrameScratch, latestFrameScratchReclamation);
		latest = latestFrameScratch;
		previous = previousFrameScratch;
		reclaimed = latestFrameScratchReclamation;
	}
	TerraEngine::Renderer::PublishFrameScratchDiagnostics(latest, previous, reclaimed);
}

void RenderSystem::UpdateAtExecutionBoundary(float deltaTime)
{
	RetireFrameExecutionAtProtectedBoundary();
	try {
		RenderFrameExecution execution = BuildFrameExecution(deltaTime);
		while(execution.Resume()) {
			// Direct callers execute the same declared stages serially. All nested
			// borrowing jobs still settle before their owning stage returns.
		}
	} catch(...) {
		ObserveFrameScratchAtProtectedBoundary();
		throw;
	}
	ObserveFrameScratchAtProtectedBoundary();
}

RenderFrameExecution RenderSystem::BuildFrameExecution(float deltaTime)
{
	if (!initialized) {
		co_return;
	}
	if (!editorModeEnabled)
	{
		// A standalone runtime derives Game View state from authored scene
		// cameras every frame. Clear the previous descriptor before collection so
		// a scene switch to a camera-less scene cannot leave capture requests
		// pointing at a stale camera from the prior scene.
		hasLatestGameViewCamera = false;
		latestGameViewCamera = {};
	}
	// Runtime C# settings become visible only at this protected render boundary.
	// Applying before snapshot construction gives every view one coherent profile.
	ApplyQueuedRuntimeRenderSettings();
	DrainCaptureReadbacks();

	// Standalone runtime UI should track the real framebuffer size. In the
	// editor, however, the Game View is letterboxed inside an ImGui window and
	// owns its own viewport size, so forcing the full GLFW framebuffer here
	// would make UI layout and hit testing react to the wrong rectangle.
	if (!editorModeEnabled)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Sync Runtime Viewport", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

		if (auto* windowHandle = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()))
		{
			int framebufferWidth = 0;
			int framebufferHeight = 0;
			glfwGetFramebufferSize(windowHandle, &framebufferWidth, &framebufferHeight);

			if (framebufferWidth > 0 && framebufferHeight > 0)
			{
				UISystem::Instance().SetGameViewportSize(glm::vec2(static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight)));
			}
		}
	}

	// Only protected capture allocates/resets this unsynchronized resource. The
	// previous continuation was retired before entry; lease release never resets.
	if(!lifetimeState->frameScratch)
		lifetimeState->frameScratch = TerraEngine::Renderer::RenderFrameScratch::Create();
	TerraEngine::Renderer::Frame::RenderScratchEpisode<CameraEntry, LightEntry> scratch(
		lifetimeState->frameScratch, nextRenderFrameId);
	auto& scratchLease = scratch.lease;
	const auto scratchValidation = frameScratchValidationCallbacks;
	{
		std::lock_guard lock(statsMutex);
		observedFrameScratchOwner = lifetimeState->frameScratch;
	}

	// Snapshot camera components so view/projection data is sourced from
	// authored entities instead of the legacy hardcoded values.
	const FrameCaptureRoots capturedRoots = CaptureFrameRoots();
	const auto& protectedRendererGeneration = capturedRoots.protectedRendererGeneration;
	const auto& protectedSceneGeneration = capturedRoots.protectedSceneGeneration;
	const auto& animationPresentationFrame = capturedRoots.animationPresentationFrame;
	const auto& immutableGpuAnimationPlan = capturedRoots.immutableGpuAnimationPlan;
	const auto& hasDenseAnimationTopologyCandidate = capturedRoots.hasDenseAnimationTopologyCandidate;
	const FrameRenderableCaptureState capturedRenderables = CaptureFrameRenderableInputs(capturedRoots,
		scratch.cameras, scratch.lights, scratchValidation);
	auto& cameras = scratch.cameras;
	auto& lights = scratch.lights;
	auto& resourceManager = ResourceManager::Instance();
	std::vector<RenderableEntry>& renderables = retainedRenderableEntries;
	auto& frameMeshResources = retainedRuntimeMeshResources;
	auto& frameMaterialResources = retainedRuntimeMaterialResources;
	auto& frameMeshLocalBounds = retainedMeshLocalBounds;
	std::vector<std::uint32_t> cpuDeformationRenderableIndices;
	std::optional<TerraEngine::Profiling::ProfilerFramePublicationEvidence> pendingProfilerPublicationEvidence;
	const auto& protectedRendererResourceOwnerRevision = capturedRenderables.protectedRendererResourceOwnerRevision;
	const auto& hasCpuDeformationCandidates = capturedRenderables.hasCpuDeformationCandidates;
	const auto& runtimeLayoutRejectedRecordCount = capturedRenderables.runtimeLayoutRejectedRecordCount;
	const auto& runtimeLayoutRejectionReasons = capturedRenderables.runtimeLayoutRejectionReasons;
	const auto& capturedScratchDiagnostics = capturedRenderables.capturedScratchDiagnostics;
	const auto& aliveCompatibleAnimationObjects = capturedRenderables.aliveCompatibleAnimationObjects;

	const FrameTerrainCapture terrainCapture = CaptureFrameTerrainInputs(std::span<const RenderableEntry>(renderables));
	RetireFrameAnimationPoseOwners(aliveCompatibleAnimationObjects);
	// Throttle warning spam so missing cameras do not flood the console every frame.
	static bool loggedMissingCamera = false;

	if (cameras.empty() && !hasSceneViewCamera)
	{
		if (!loggedMissingCamera)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "RenderSystem skipped frame because no CameraComponent was found in the scene.");
			loggedMissingCamera = true;
		}
	}
	else
	{
		loggedMissingCamera = false;
	}

	const FrameWorldTransforms worldCapture = CaptureFrameWorldTransforms();
	const auto* worldTransforms = worldCapture.worldTransforms;
	const auto* transformPropagation = worldCapture.transformPropagation;


	activeRenderFrameId = nextRenderFrameId++;
	// Mesh payloads and streaming publications are immutable for this protected
	// render update. Build their expensive runtime mirrors once per unique mesh,
	// then let every instance and view reuse the same frame-local value.
	TerraEngine::Renderer::Frame::FrameGeometryInputs geometryInputs(frameMeshResources.size());
	const std::size_t expectedUniqueMeshCount = geometryInputs.ExpectedUniqueMeshCount();
	auto cpuDeformationPlan = BuildFrameCpuDeformationPlan(
		std::span<RenderableEntry>(renderables), hasCpuDeformationCandidates,
		protectedRendererGeneration, cpuDeformationRenderableIndices);
	const FrameBackendCapture backendCapture = CaptureFrameBackendInputs();
	const auto& backendCapabilities = backendCapture.backendCapabilities;
	const auto& hardwareVirtualGeometryRouteAvailable = backendCapture.hardwareVirtualGeometryRouteAvailable;
	const auto& computeVirtualGeometryRouteAvailable = backendCapture.computeVirtualGeometryRouteAvailable;
	const auto& validationRouteOverride = backendCapture.validationRouteOverride;
	const auto& hasPendingDedicatedCaptureRender = backendCapture.hasPendingDedicatedCaptureRender;
	const auto& pendingDedicatedCaptureIdentity = backendCapture.pendingDedicatedCaptureIdentity;
	const auto& pendingDedicatedCaptureCanonicalOrigin = backendCapture.pendingDedicatedCaptureCanonicalOrigin;
	const auto& pendingDedicatedCaptureTargetSize = backendCapture.pendingDedicatedCaptureTargetSize;
	// Capture monotonic producer values once. These scalars are also the worker
	// reuse proof below, so no worker needs to query mutable scene ownership.
	const FrameSceneRevisions sceneRevisions = CaptureFrameSceneRevisions();
	const auto& capturedSpatialRevision = sceneRevisions.capturedSpatialRevision;
	const auto& capturedHierarchyRevision = sceneRevisions.capturedHierarchyRevision;
	const FrameResourceCohort resourceCohort = CaptureFrameResourceCohort(protectedRendererResourceOwnerRevision);
	const auto& rendererResourceOwnerCohortStable = resourceCohort.rendererResourceOwnerCohortStable;
	const auto& capturedVirtualGeometryResidencyGeneration = resourceCohort.capturedVirtualGeometryResidencyGeneration;
	FrameSnapshotCapture snapshotCapture = CaptureFrameSnapshotHeader(renderables, !cameras.empty(),
		capturedRenderables, capturedRoots, sceneRevisions, resourceCohort, backendCapture, worldCapture);
	auto& renderFrameSnapshot = snapshotCapture.renderFrameSnapshot;
	const auto& previousViewRequests = snapshotCapture.previousViewRequests;
	const bool reuseRenderableRecords = snapshotCapture.reuseRenderableRecords;
	FrameFixtureCapture fixtureCapture = CaptureFrameFixtureInputs(renderFrameSnapshot, renderables,
		cameras, lights, frameMeshResources, geometryInputs, rendererResourceOwnerCohortStable,
		worldCapture, hasPendingDedicatedCaptureRender);
	auto& fixtureTargets = fixtureCapture.fixtureTargets;
	const auto& sealedFixtureCameras = fixtureCapture.sealedFixtureCameras;
	// Sealed descriptors are coroutine-owned immutable copies. Window visibility,
	// orbit controls and panel dimensions cannot overwrite validation view inputs.
	const auto sealedCamera = [&](RenderViewType type, std::uint32_t output) -> const TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera* {
		if(!renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied) return nullptr;
		const auto found = std::find_if(sealedFixtureCameras.begin(),sealedFixtureCameras.end(),[&](const auto& camera) {return camera.viewType==type && camera.outputId==output;});
		return found==sealedFixtureCameras.end() ? nullptr : &*found;
	};
	const auto* fixedSceneCamera = sealedCamera(RenderViewType::Scene,1);
	const auto* fixedGameCamera = sealedCamera(RenderViewType::Game,2);
	const bool sealedCameraRecipeActive = renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied && !sealedFixtureCameras.empty();
	const glm::uvec2 effectiveSceneTargetSize = fixedSceneCamera ? fixedSceneCamera->nativeExtent : sceneViewTargetSize;
	const glm::uvec2 effectiveGameTargetSize = fixedGameCamera ? fixedGameCamera->nativeExtent : gameViewTargetSize;
	// Capture the renderer lifetime once. Every animated record in this immutable
	// frame must agree with it; querying recovery state per renderable could mix a
	// pre-loss and post-loss generation in one snapshot.
	const std::uint64_t snapshotRendererGeneration = protectedRendererGeneration;
	const auto makeViewRequest = [&](RenderOutputKind outputKind,
		RenderViewType viewType,
		const glm::uvec2& targetSize,
		std::uint32_t outputId,
		const TerraEngine::Scene::WorldPosition& canonicalOrigin)
	{
		TerraEngine::Renderer::RenderFrameViewRequest request{};
		request.outputKind = outputKind;
		request.viewType = viewType;
		request.targetSize = targetSize;
		request.outputId = outputId;
		request.viewGeneration = activeRenderFrameId;
		request.referenceFrame.frameId = activeRenderFrameId;
		request.referenceFrame.viewId = static_cast<std::uint64_t>(outputId);
		request.referenceFrame.outputId = outputId;
		request.referenceFrame.canonicalOrigin = canonicalOrigin;
		const auto* fixedCamera = sealedCamera(viewType,outputId);
		if(fixedCamera) request.referenceFrame.canonicalOrigin = fixedCamera->referenceOrigin;
		const auto& fixtureIntent = renderFrameSnapshot.efficiencyFixtureIntent;
		const auto originDelta = TerraEngine::Renderer::RendererEfficiencyFixtureOriginDelta(fixtureIntent, viewType, outputId);
		if (!fixedCamera && fixtureIntent.controlsApplied && originDelta
			&& (outputKind == RenderOutputKind::SceneView || outputKind == RenderOutputKind::GameView || outputKind == RenderOutputKind::MainRuntimeView))
		{
			const auto changed = TerraEngine::Scene::ApplyWorldDelta(canonicalOrigin, *originDelta);
			if (changed.Succeeded()) request.referenceFrame.canonicalOrigin = changed.position;
		}
		request.referenceFrame.originGeneration = activeRenderFrameId;
		request.referenceFrame.spatialGeneration =
			std::max<std::uint64_t>(1U, capturedSpatialRevision);
		request.referenceFrame.residencyGeneration = std::max<std::uint64_t>(
			1U, capturedVirtualGeometryResidencyGeneration);
		return request;
	};
	renderFrameSnapshot.viewRequests.reserve(3u + gameViewCameras.size() + cameras.size());
	if (fixedSceneCamera || (!sealedCameraRecipeActive && hasSceneViewCamera && (!editorModeEnabled || (sceneViewTargetSize.x > 0u && sceneViewTargetSize.y > 0u))))
	{
		renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
			RenderOutputKind::SceneView, RenderViewType::Scene,
			effectiveSceneTargetSize, 1U, fixedSceneCamera ? fixedSceneCamera->cameraPosition : sceneViewCamera.canonicalPosition));
	}
	if (fixedGameCamera || (!sealedCameraRecipeActive && !gameViewSuppressed && (!editorModeEnabled || (gameViewTargetSize.x > 0u && gameViewTargetSize.y > 0u))))
	{
		if (fixedGameCamera || !gameViewCameras.empty())
		{
			renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
				RenderOutputKind::GameView, RenderViewType::Game,
				effectiveGameTargetSize, 2U, fixedGameCamera ? fixedGameCamera->cameraPosition : gameViewCameras.front().canonicalPosition));
		}
		else if (!cameras.empty())
		{
			TerraEngine::Scene::WorldPosition runtimeCameraPosition{};
			const auto runtimeCamera = worldTransforms->find(cameras.front().entity);
			if (runtimeCamera != worldTransforms->end())
			{
				runtimeCameraPosition = runtimeCamera->second.position;
			}
			renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
				RenderOutputKind::MainRuntimeView, RenderViewType::Game,
				effectiveGameTargetSize, 2U, runtimeCameraPosition));
		}
	}
	if (hasPendingDedicatedCaptureRender)
	{
		renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
			RenderOutputKind::OffscreenTexture,
			pendingDedicatedCaptureIdentity.viewType,
			pendingDedicatedCaptureTargetSize,
			pendingDedicatedCaptureIdentity.outputId,
			pendingDedicatedCaptureCanonicalOrigin));
	}
	if(renderFrameSnapshot.viewRequests.empty()) {
		CompleteFrameWithoutViews(renderFrameSnapshot, protectedSceneGeneration,
			snapshotRendererGeneration, backendCapabilities);
		scratchLease.MarkCompleted();
		co_return;
	}

	// Capture every live generation and completed backend counter before leaving
	// the protected scheduler boundary. The continuation below may run on a worker,
	// so it must consume only these values, the immutable scene-change packet, and
	// the frame-owned component/resource snapshots prepared above.
	const FrameBackendObservations backendObservations = CaptureFrameBackendObservations();
	const auto& completedVirtualGeometryCounters = backendObservations.completedVirtualGeometryCounters;
	const auto& capturedBackendBreakdown = backendObservations.capturedBackendBreakdown;
	const auto& capturedRecoverySnapshot = backendObservations.capturedRecoverySnapshot;
	// Sky hierarchy selection is scene-owned work. Resolve and copy the selected
	// component before the worker boundary so sky preparation cannot traverse live
	// ECS storage or resolve an asset while another system is publishing changes.
	const FrameSkyCapture skyCapture = CaptureFrameSky(capturedHierarchyRevision);
	// Only protected capture can inspect live identity and loader ownership.
	// Copy lifetimes only when rebuilding rows; compatible warm snapshots retain
	// their sealed values. Shader owners stay alive across worker suspension even
	// when an asynchronous reload replaces the ResourceManager cache entry.
	const auto capturedShadowObjectLifetimes = CaptureFrameShadowLifetimes(
		std::span<const RenderableEntry>(renderables), reuseRenderableRecords);
	// Boundary 1 ends immediately after immutable capture. Route scoring, snapshot
	// assembly, topology transactions, and deterministic lowering are worker work;
	// keeping them out of the protected phase prevents scene mutation and editor UI
	// from waiting on a complete crowd traversal.
	co_await std::suspend_always{};
	const FrameSnapshotShadowView snapshotShadowInputs{resourceCohort, frameMaterialResources,
		std::span<const std::uint64_t>(capturedShadowObjectLifetimes), renderFrameSnapshot.resourceRevision};

	GeometryRouteStatsAggregate geometryRouteStats;
	std::uint64_t geometryRouteSelectionSignature = 1469598103934665603ULL;
	if(!reuseRenderableRecords)
	{
		const FrameSnapshotRouteView routeInputs{snapshotRendererGeneration,
			hardwareVirtualGeometryRouteAvailable, computeVirtualGeometryRouteAvailable,
			validationRouteOverride, immutableGpuAnimationPlan};
		AssembleFrameSnapshotRows(std::span<RenderableEntry>(renderables), renderFrameSnapshot,
			geometryInputs, routeInputs, snapshotShadowInputs, sceneRevisions, worldCapture,
			geometryRouteStats, geometryRouteSelectionSignature, expectedUniqueMeshCount);
	}
	else
	{
		TerraEngine::Renderer::Frame::ReuseFrameRouteDiagnostics(renderFrameSnapshot,
			geometryRouteStats, geometryRouteSelectionSignature);
	}
	TerraEngine::Renderer::Frame::BuildFrameViewTransforms(renderFrameSnapshot, previousViewRequests);
	TerraEngine::Renderer::Frame::PrepareFrameDirectMeshletBatches(renderFrameSnapshot,
		frameMeshResources, geometryRouteStats, reuseRenderableRecords);
	geometryRouteSelectionSignature ^=
		static_cast<std::uint64_t>(renderFrameSnapshot.renderables.size());
	AppendSnapshotLightRows(lights, renderFrameSnapshot);

	std::vector<TerraEngine::Renderer::RenderPassNode> renderGraphPasses;
	RetainedRenderTopologyState& retainedRenderTopology =
		lifetimeState->retainedRenderTopology;
	GroupedSubmissionMembershipCacheState& groupedSubmissionMembershipCache =
		lifetimeState->groupedSubmissionMembershipCache;
	if(groupedSubmissionMembershipCache.owner != this) {
		groupedSubmissionMembershipCache = {};
		groupedSubmissionMembershipCache.owner = this;
	}
	const bool renderTopologyRebuilt = PrepareFramePersistentTopology(renderFrameSnapshot,
		retainedRenderTopology, sceneRevisions, snapshotRendererGeneration, backendCapabilities, renderGraphPasses);
	// Draw-list materialization consults the retained map instead of reconstructing
	// a UUID lookup for every stable frame. The map contains only static snapshot
	// identities and is replaced atomically at this protected coordinator point.
	const auto& conventionalVisibilityIdentityIndices =
		retainedRenderTopology.conventionalVisibilityIdentityIndices;
	// The protected capture already copied replay counters and generation values.
	// Reaching this point completes worker-owned route/topology preparation.
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache& rendererPreparationCache =
		lifetimeState->rendererPreparationCache;
	// This wall span deliberately covers only preparation work that turns the
	// immutable frame snapshot into backend-ready cache and submission data. It
	// excludes diagnostics and later publication, which have independent timing
	// ownership and would otherwise make the profiler buckets overlap.
	const auto backendPreparationStart = std::chrono::steady_clock::now();
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationSummary rendererPreparationSummary;
	{
		// Backend caches must observe the same immutable root that Vulkan later
		// consumes. Preparation is therefore deferred until sealing below instead
		// of receiving the worker-only render collection directly.
	}
	const auto backendPreparationEnd = std::chrono::steady_clock::now();
	// Diagnostics begin from publication-neutral frame facts. Packet/submission
	// summaries belonged to the superseded intermediate plan and could disagree with
	// the sealed ranges that Vulkan actually consumes.
	TerraEngine::Renderer::RenderDiagnosticsSummary renderGraphDiagnostics{};
	renderGraphDiagnostics.frameId = activeRenderFrameId;
	renderGraphDiagnostics.valid = true;
	renderGraphDiagnostics.detailed = TerraEngine::Renderer::RenderDiagnosticsRequested();
	if(!backendCapabilities.supportsParallelRecording) {
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Backend profile requires serial render recording.");
	}
	renderGraphDiagnostics.runtimeLayoutRenderRecordCount = cameras.size() + renderables.size() + lights.size();
	renderGraphDiagnostics.runtimeLayoutRejectedRecordCount = runtimeLayoutRejectedRecordCount;
	renderGraphDiagnostics.runtimeLayoutRejectionReasons = runtimeLayoutRejectionReasons;
	if (TerraEngine::Renderer::RenderDiagnosticsRequested() && rhi)
	{
		TerraEngine::Renderer::BackendFrameBreakdown backendBreakdown =
			capturedBackendBreakdown;
		if (backendBreakdown.frameId == 0u)
		{
			backendBreakdown = TerraEngine::Renderer::BuildUnavailableBackendFrameBreakdown(renderFrameSnapshot);
		}
		TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(renderGraphDiagnostics, backendBreakdown);
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Vulkan preparation cache hits: "
			+ std::to_string(rendererPreparationSummary.cacheHitCount)
			+ "/"
			+ std::to_string(rendererPreparationSummary.cacheLookupCount));
		if (rendererPreparationSummary.uploadPlan.queuedForUploadWorker)
		{
			renderGraphDiagnostics.backendFallbacks.push_back("Immutable upload packets are queued for backend worker preparation.");
		}
	}
	const TerraEngine::Renderer::RendererRecoverySnapshot recoverySnapshot =
		capturedRecoverySnapshot;
	TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(renderGraphDiagnostics, recoverySnapshot);
	if (!TerraEngine::Renderer::CanSubmitNormalGpuWork(recoverySnapshot))
	{
		ReportFatalRendererRecoveryFailure(recoverySnapshot);
	}
	// Serial validation uses the same immutable inputs and protected boundaries,
	// with one preparation lane so comparisons can detect concurrency defects.
	const size_t renderGraphWorkerCount = renderFrameSnapshot.efficiencyOptions.options.validationPreparation
		== TerraEngine::Renderer::RendererEfficiencyPreparation::Serial ? 1U
		: std::max<size_t>(1u, backendCapabilities.maxRecordingWorkers);
	VisibleCommandCacheState& visibleCommandCache = lifetimeState->visibleCommandCache;
	ShadowDrawListCacheState& shadowDrawListCache = lifetimeState->shadowDrawListCache;
	visibleCommandCache.hitCount = 0u;
	visibleCommandCache.missCount = 0u;
	shadowDrawListCache.hitCount = 0u;
	shadowDrawListCache.missCount = 0u;
	const std::uint64_t renderPreparationSettingsSignature = BuildRenderSettingsSignature(renderFrameSnapshot.renderSettings, currentPreset, taskShaderCullingEnabled);

	SpatialPreparationCacheState& spatialPreparationCache = lifetimeState->spatialPreparationCache;
	bool spatialIndexRebuilt = false;
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestSpatialRefreshDiagnostics =
		PrepareFrameSpatialInputs(renderables, renderFrameSnapshot, spatialPreparationCache,
			sceneRevisions, worldCapture, frameMeshResources, frameMeshLocalBounds,
			expectedUniqueMeshCount, spatialIndexRebuilt);
	const std::vector<TerraEngine::Scene::SpatialObjectRecord>& spatialRecords =
		spatialPreparationCache.records;
	const std::unordered_map<entt::entity, std::size_t>& renderableLookup =
		spatialPreparationCache.renderableLookup;
	const std::vector<bool>& conservativeAnimationBounds =
		spatialPreparationCache.conservativeAnimationBounds;

	RenderableDrawWorkingSet& renderableDrawWorkingSet = lifetimeState->renderableDrawWorkingSet;
	PrepareFrameBaseCommands(renderables, renderFrameSnapshot, renderableDrawWorkingSet,
		lifetimeState->drawCommandCache, sceneRevisions, worldCapture, renderGraphWorkerCount, renderGraphDiagnostics);
	std::vector<RenderItemRecord>& renderableDrawCommands = renderableDrawWorkingSet.commands;
	std::vector<std::uint64_t>& drawCommandSignatures = renderableDrawWorkingSet.signatures;
	RenderableFrameIdentityCacheState& renderableFrameIdentityCache = lifetimeState->renderableFrameIdentityCache;
	const FrameCommandIdentity commandIdentity = PrepareFrameCommandIdentity(renderables, renderFrameSnapshot,
		renderableFrameIdentityCache, drawCommandSignatures, conservativeAnimationBounds,
		sceneRevisions, resourceCohort, animationPresentationFrame);
	const auto renderableWorldSignature = commandIdentity.world;
	const auto renderableSubmissionSignature = commandIdentity.submission;
	const auto renderableVisibilitySignature = commandIdentity.visibility;
	// A producer-sealed GPU topology always advances through its separate root.
	// CPU deformation remains a sparse companion plan and refreshes only the pages
	// that own those exceptional lanes; it must not disable dense publication for
	// the thousands of GPU-resident owners in the same frame.
	const bool hasDenseAnimationPublication =
		hasDenseAnimationTopologyCandidate;
	const std::unordered_map<TerraEngine::UUID, std::size_t>&
		renderableCommandIndices =
			renderableFrameIdentityCache.indicesByObjectId;





	FrameVirtualGeometryPreparation virtualGeometryPreparation;
	const FrameVirtualGeometryInputs virtualGeometryInputs{renderFrameSnapshot, frameMeshResources,
		frameMaterialResources, resourceCohort, geometryInputs, backendCapture, capturedVirtualGeometryResidencyGeneration,
		renderGraphDiagnostics, fixtureTargets};










	// Worker visibility records resource demand without mutating ResourceManager.
	// Boundary 3 publishes these compact touch sets after all view work is sealed.
	std::vector<std::pair<RenderViewType,
		TerraEngine::Renderer::VisibleStreamingTouchSet>> pendingStreamingTouchSets;
	pendingStreamingTouchSets.reserve(renderFrameSnapshot.viewRequests.size());




	const auto virtualGeometryResidencyGeneration = capturedVirtualGeometryResidencyGeneration;
	const FrameVisibilityInputs visibilityInputs{renderables, renderFrameSnapshot,
		spatialPreparationCache, renderableDrawCommands, retainedRenderTopology,
		renderableFrameIdentityCache, latestSpatialRefreshDiagnostics, renderableVisibilitySignature, snapshotRendererGeneration,
		capturedVirtualGeometryResidencyGeneration};
	const FrameVisibleStreamingInputs visibleStreamingInputs{frameMaterialResources, terrainCapture};



	std::vector<FrameLightCandidate> packedLights = PrepareFrameLightCandidates(lights,
		lifetimeState->lightCandidateCache, worldCapture, renderGraphWorkerCount,
		renderGraphDiagnostics, scratchValidation, capturedScratchDiagnostics);

	std::vector<const RenderItemRecord*> renderableDrawCommandsByOrdinal(
		retainedRenderTopology.topology.records.size(), nullptr);
	for(const RenderItemRecord& command : renderableDrawCommands)
	{
		if(command.conventionalVisibilityIdentityIndex == 0u) continue;
		const std::uint32_t ordinal = command.conventionalVisibilityIdentityIndex - 1u;
		if(ordinal < renderableDrawCommandsByOrdinal.size())
			renderableDrawCommandsByOrdinal[ordinal] = &command;
	}

	SkyRuntimeState skyRuntimeState{};
	RenderViewBuildState frameData = PrepareFrameHeader(lights, packedLights, renderFrameSnapshot,
		skyCapture, worldCapture, backendObservations, deltaTime, skyRuntimeState);

	// Boundary 2: shared route, spatial, light, upload, and frame-header
	// preparation is complete. Per-view visibility and publication sealing are a
	// separate worker stage so both costs remain independently diagnosable.
	co_await std::suspend_always{};

	FrameViewBuildOutputs viewOutputs;

	// Convert each camera submission into an explicit output owner so the RHI
	// can give Scene, Game, and future offscreen cameras independent snapshot
	// ranges instead of inferring ownership from call order alone.



	auto& preparedRenderSubmissions = viewOutputs.preparedRenderSubmissions;
	preparedRenderSubmissions.reserve(
		static_cast<std::size_t>(hasSceneViewCamera ? 1u : 0u)
		+ gameViewCameras.size()
		+ cameras.size()
		+ captureLimits.maxConcurrentCaptures);

	// Each materialized view marks an index in the canonical renderable table.
	// The final publication walks this mask once, producing sorted unique UUIDs
	// without rescanning every camera command and sorting duplicates afterward.
	auto& visibleAnimatedRenderableFlags = viewOutputs.visibleAnimatedRenderableFlags;
	visibleAnimatedRenderableFlags.assign(renderables.size(), 0u);











	const FrameViewSelectionSettings viewSelectionSettings{renderableDrawCommandsByOrdinal,
		renderPreparationSettingsSignature, renderableSubmissionSignature, renderGraphWorkerCount,
		renderTopologyRebuilt, effectiveSceneTargetSize, effectiveGameTargetSize};
	PrepareFrameSceneView(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs,
		viewSelectionSettings, viewOutputs, pendingStreamingTouchSets, frameData, fixedSceneCamera,
		sealedCameraRecipeActive, sealedFixtureCameras);
	PrepareFrameGameViews(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs,
		viewSelectionSettings, viewOutputs, pendingStreamingTouchSets, frameData, cameras, worldCapture,
		fixedGameCamera, sealedCameraRecipeActive, sealedFixtureCameras, deltaTime);
	std::deque<TerraEngine::Renderer::RenderCaptureRequest> captureRequestsForReadback;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		while (!pendingCaptureRequests.empty() && captureQueueState.activeCaptureCount < captureLimits.maxConcurrentCaptures)
		{
			if (auto found = captureOperations.find(pendingCaptureRequests.front().requestId); found != captureOperations.end()) found->second->activeCharged = true;
			captureRequestsForReadback.push_back(std::move(pendingCaptureRequests.front()));
			pendingCaptureRequests.pop_front();
			++captureQueueState.activeCaptureCount;
		}
		captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	}

	PrepareFrameDedicatedViews(visibilityInputs, virtualGeometryInputs, virtualGeometryPreparation, visibleStreamingInputs,
		viewSelectionSettings, viewOutputs, pendingStreamingTouchSets, frameData,
		backendCapabilities, captureRequestsForReadback);
	OrderFramePreparedViews(renderFrameSnapshot, preparedRenderSubmissions);
	RetireFrameViewHistories(virtualGeometryPreparation);

	PublishFrameAnimatedVisibility(renderFrameSnapshot, renderables, preparedRenderSubmissions,
		visibleAnimatedRenderableFlags, protectedSceneGeneration);

	FrameViewObservations viewObservations = ObserveFrameViewOutputs(renderFrameSnapshot,
		preparedRenderSubmissions, snapshotRendererGeneration);

	// Materialization already merged every camera, shadow set, and dedicated
	// capture into this canonical renderable-index bitset. Reuse it directly;
	// rescanning draw, cluster, and packet streams would hash the same object
	// identity up to three times before deformation selection even begins.
	const AnimatedGeometryBuildResult animatedGeometry =
		BuildFrameAnimatedGeometryPlan(renderFrameSnapshot, frameMeshResources,
			snapshotRendererGeneration, protectedSceneGeneration, visibleAnimatedRenderableFlags,
			renderableCommandIndices);
	renderFrameSnapshot.animatedGeometryPageDemandCount =
		animatedGeometry.demandCount;
	TerraEngine::Renderer::DeformationBackendWorkPlan selectedDeformation;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Deformation Backend Plan",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		selectedDeformation = TerraEngine::Renderer::BuildDeformationBackendWorkPlan(
			activeRenderFrameId, animatedGeometry.immutablePlan);
	}
	PrepareFrameVirtualGeometryEnvelope(renderFrameSnapshot, preparedRenderSubmissions, backendCapabilities,
		snapshotRendererGeneration, completedVirtualGeometryCounters, renderGraphDiagnostics);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Seal Frame Publication",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto publicationSealStart = std::chrono::steady_clock::now();
		auto publication = BeginFramePublication(renderFrameSnapshot, capturedRoots,
			retainedRenderTopology, renderGraphDiagnostics);
		const auto publicationSessionGeneration = publication->rendererSessionGeneration;
		const auto persistentPublicationEnd = std::chrono::steady_clock::now();

		PrepareFrameDynamicPages(publication, retainedRenderTopology, renderFrameSnapshot,
			commandIdentity, sceneRevisions, resourceCohort, cpuDeformationRenderableIndices,
			hasDenseAnimationPublication, renderGraphWorkerCount);
		PrepareFrameBackendWork(publication, capturedRoots, cpuDeformationPlan, selectedDeformation);
		const auto dynamicPublicationEnd = std::chrono::steady_clock::now();

		LowerFrameViewPublications(publication, preparedRenderSubmissions, renderFrameSnapshot);
		const auto viewPublicationEnd = std::chrono::steady_clock::now();
		pendingProfilerPublicationEvidence = FinalizeFramePublication(std::move(publication),
			rendererPreparationSummary, renderGraphPasses, backendCapabilities, capturedRoots,
			FramePublicationTimings{backendPreparationStart, backendPreparationEnd, publicationSealStart,
				persistentPublicationEnd, dynamicPublicationEnd, viewPublicationEnd});

	}

	// Boundary 3: every fallible publication and per-view plan is sealed. Only
	// protected backend ownership and final diagnostics remain below this point.
	co_await std::suspend_always{};
	const auto sealedPublication = CommitFrameBackend(pendingProfilerPublicationEvidence, pendingStreamingTouchSets,
		renderFrameSnapshot, viewObservations, captureRequestsForReadback, rendererPreparationSummary,
		backendCapabilities, snapshotRendererGeneration, renderGraphDiagnostics);

	PublishFrameDiagnostics(renderFrameSnapshot, virtualGeometryPreparation, viewOutputs, viewObservations,
		geometryRouteStats, geometryRouteSelectionSignature, fixtureTargets, skyRuntimeState,
		recoverySnapshot, sealedPublication, renderGraphDiagnostics);

	skyStateDirty = false;

	PublishFrameStreamingFeedback();

	scratchLease.MarkCompleted();
}


void RenderSystem::Shutdown()
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	RetireCaptureJobs();
	RetireFrameExecutionAtProtectedBoundary();
	initialized = false;
	if(lifetimeState->frameScratch) lifetimeState->frameScratch->Retire();
	ObserveFrameScratchAtProtectedBoundary();
	{
		std::lock_guard lock(statsMutex);
		// The current lifetime still owns scratch. Drop the observer now so later
		// lifetime replacement destroys backing outside this lock, protected.
		observedFrameScratchOwner.reset();
	}
	// Restore tokens and pending requests belong to this renderer lifetime, not
	// just Vulkan's counter (which may restart with a replacement instance).
	{
		std::lock_guard lock(runtimeRenderSettingsMutex);
		rendererEfficiencyOptionsQueue.ResetForNewLifetime();
		pendingRendererEfficiencyFixtureIntent.reset();
		capturedRendererEfficiencyFixtureIntent = {};
		pendingRendererResourceInvalidations.clear();
		efficiencyObservedBackendSession = 0U;
	}
	{
		std::lock_guard lock(statsMutex);
		latestRendererEfficiencyEvidence.clear();
		latestRendererEfficiencyFixtureTargets.clear();
		TerraEngine::Renderer::ResetRendererEfficiencyFixtureEvidenceBatch(latestRendererEfficiencyEvidenceBatch);
	}
	// Drop copied scene data before backend teardown so a later editor restart
	// cannot reuse transforms or sky entities from the previous scene session.
	worldTransformCache.Clear();
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
	virtualGeometryViewPolicies.clear();
	virtualGeometryReplayReadbacks.Clear();
	latestVirtualGeometryReplayArtifacts.clear();
	// Detach the immutable hand-off before destroying backend resources so no
	// retained reader can mistake the shutdown generation for a live session.
	renderFramePublicationStore.ResetForSession(
		renderFramePublicationStore.SessionGeneration() + 1u);
	retainedPersistentTopologyRoot.reset();
	retainedDynamicStateRoot.reset();
	retainedCpuDynamicPageIndices.clear();
	dynamicPageRecyclePool.Clear();
	retainedDynamicStateSourceSignature = 0u;
	// Release every retained topology/view/backend cache with the renderer
	// session. A later Initialize starts from an empty, instance-owned state.
	lifetimeState = std::make_unique<RenderSystemLifetimeState>();
	hasCachedSkyObjectResolution = false;
	TerraEngine::Renderer::ClearAnimatedObjectVisibilitySnapshot();
	cachedSkyObject.reset();
	cachedSkyHierarchyRevision = 0U;
	cachedSkyCandidateSignature = 0U;
	if (!TryReleaseCaptureBackend()) {
		TERRAENGINE_LOG(LogLevel::Error, "Renderer shutdown retained its backend because native retirement is not yet proven.");
	}
}


std::string RenderSystem::SystemName()
{
	return "RenderSystem";
}

TerraEngine::Scheduling::SystemDeclaration RenderSystem::SchedulerDeclaration()
{
	TerraEngine::Scheduling::SystemDeclaration declaration;
	declaration.name = "RenderSystem";
	declaration.phase = "Render";
	declaration.reads = {
		"RendererEfficiencyOptionsQueue", "RendererEfficiencyOptions",
		"LightingUploadMirrors", "LightingFrameSlotCompletion",
		"LightingDepthPreparedInputs", "LightingDepthQueryOwners",
		"Window",
		"ActivePresentationState",
		"SceneGraph",
		"ObjectManager",
		"UI",
		"Resources",
		TerraEngine::Scheduling::kResourceTerrainRenderProxies,
		TerraEngine::Scheduling::kResourceAnimatorComponents,
		TerraEngine::Scheduling::kResourceAnimationRuntimeState,
		TerraEngine::Scheduling::kResourceVirtualGeometryQualityProfile,
		TerraEngine::Scheduling::kResourceVirtualGeometryResidencySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageResidencySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryHiZValidationState,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuPacketFrameSets,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuCompactionDispatches,
		TerraEngine::Scheduling::kResourceVirtualGeometryGeneratedSubmissions,
		TerraEngine::Scheduling::kResourceVirtualGeometryCounterReadbacks,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourcePhysicalVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourceExactResidencySnapshots,
		TerraEngine::Scheduling::kResourceBackendGpuAllocationSnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications,
		TerraEngine::Scheduling::kResourceVisibleStreamingTouchSets,
		TerraEngine::Scheduling::kResourceRendererRecoveryStateSnapshot,
		TerraEngine::Scheduling::kResourceRendererDeviceIncidentLog,
		TerraEngine::Scheduling::kResourceRuntimeRenderSettingsQueue,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPageDemands,
		TerraEngine::Scheduling::kResourceAnimatedGeometryOutputs,
		TerraEngine::Scheduling::kResourceAnimatedGeometryDynamicBounds,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPublications
	};
	declaration.writes = {
		"RendererEfficiencyOptions", "RendererEfficiencyEvidence",
		// Protected capture may admit a deduplicated async mesh/material request;
		// accepting a ready resource also changes its publication cohort.
		"Resources", "StreamingNeeds",
		"Renderer",
		TerraEngine::Scheduling::kResourcePersistentRenderTopology,
		TerraEngine::Scheduling::kResourceDynamicRenderInstanceCandidates,
		TerraEngine::Scheduling::kResourceRenderCompatibilityGroups,
		TerraEngine::Scheduling::kResourceRenderInstanceStreamPublication,
		TerraEngine::Scheduling::kResourceCachedRenderViewEvidence,
		TerraEngine::Scheduling::kResourceRenderOptimizationEvidence,
		"PresentationState",
		"PresentationGeneration",
		"EditorSubmissionCompletion",
		"RendererDiagnostics",
		TerraEngine::Scheduling::kResourceVirtualGeometryClusterPackets,
		TerraEngine::Scheduling::kResourceVirtualGeometryBatchPlans,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuPacketFrameSets,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuCompactionDispatches,
		TerraEngine::Scheduling::kResourceVirtualGeometryCompactionResults,
		TerraEngine::Scheduling::kResourceVirtualGeometryGeneratedSubmissions,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourcePhysicalVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourceExactResidencySnapshots,
		TerraEngine::Scheduling::kResourceBackendGpuAllocationSnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageRequests,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageNeedPackets,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageRetirements,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageDiagnostics,
		TerraEngine::Scheduling::kResourceVirtualGeometryVisibilitySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryCounterSnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryCounterReadbacks,
		TerraEngine::Scheduling::kResourceVirtualGeometryCullingDiagnostics,
		TerraEngine::Scheduling::kResourceVirtualGeometryPerformanceCounters,
		TerraEngine::Scheduling::kResourceVirtualGeometryShadowVisibilitySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryDiagnostics,
		TerraEngine::Scheduling::kResourceVisibleCommandReuseDiagnostics,
		TerraEngine::Scheduling::kResourceVisibleStreamingTouchSets,
		TerraEngine::Scheduling::kResourceRendererRecoveryStateSnapshot,
		TerraEngine::Scheduling::kResourceRendererDeviceIncidentLog,
		TerraEngine::Scheduling::kResourceRuntimeRenderSettings,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPageDemands,
		TerraEngine::Scheduling::kResourceAnimatedGeometryDeformationPlans,
		TerraEngine::Scheduling::kResourceAnimatedGeometryOutputs,
		TerraEngine::Scheduling::kResourceAnimatedGeometryDynamicBounds,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPublications,
		TerraEngine::Scheduling::kResourceAnimatedGeometryRetirements,
		TerraEngine::Scheduling::kResourceAnimatedObjectVisibilitySnapshot
	};
	// Live ECS collection still runs protected inside RenderSystem, while draw,
	// light, upload, adaptive virtual geometry LOD/cluster/page intent, and
	// backend packet preparation consume immutable snapshots. The worker-safe
	// side may calculate selections and diagnostics only; the protected side
	// still owns RHI submission, resource publication, and the final diagnostic
	// swap visible to editor windows and automation.
	declaration.mainThread = false;
	declaration.requiresFlushedSceneMutations = true;
	declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
	declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
	declaration.hardBoundary = true;
	declaration.safeSubwork = "Immutable render snapshots, exact shadowcaster membership preparation from copied input proofs, generation-validated animation pose bindings, frame-wide animated page demand union, checked morph-before-skin deformation prefix planning, history compatibility lookup, visible-command cache lookup, draw packets, culling and lighting preparation, generation-tagged submission plans, virtual geometry LOD/page preparation, page request planning, page payload source planning, resource readiness, secondary command buffers, and submit descriptions.";
	declaration.protectedCommit = "Runtime render-settings queue application, protected deduplicated async admission of missing mesh/material publications, live hierarchy/AnimatorComponent/deformable-resource and immutable shadowcaster lifetime/resource/deformation proof capture, immutable AnimationRuntimeState pose capture, Swapchain acquire, acquired-image generation validation, animated geometry allocation and atomic geometry/history/bounds publication with one cross-view generation, descriptor writes, deformation command recording, fence-delayed animated output retirement, command/fence ownership, queue submit/present, presentation recreation, completion publication, ResourceManager visible streaming touch publication, virtual geometry page payload publication, page retirement draining, virtual geometry GPU publication, virtual geometry diagnostics publication, renderer diagnostics, and device-loss containment.";
	declaration.safeSubwork += " Canonical lighting payload serialization from immutable copied final/analysis records and exact physical dirty-range planning.";
	declaration.protectedCommit += " Lighting allocation/frame/snapshot/purpose owner validation, fence-proven host map/write/atom-flush, success-only mirror publication and completion-qualified lighting evidence.";
	declaration.writes.push_back("LightingUploadMirrors");
	// BeginFrame and submission failure paths also update the fence-backed
	// physical reuse proof inside this same protected renderer boundary.
	declaration.writes.push_back("LightingFrameSlotCompletion");
	declaration.writes.push_back("LightingUploadEvidence");
	declaration.writes.push_back("LightingDepthPreparedInputs");
	declaration.writes.push_back("LightingDepthQueryOwners");
	declaration.writes.push_back("LightingDepthEvidence");
	declaration.reads.push_back("ShadowGroupedPreparedInputs");
	declaration.reads.push_back("ShadowGroupedPhysicalOwners");
	declaration.writes.push_back("ShadowGroupedPreparedInputs");
	declaration.writes.push_back("ShadowGroupedPhysicalOwners");
	declaration.writes.push_back("ShadowGroupedExecutionEvidence");
	declaration.reads.push_back("StaticShadowClassificationInputs");
	declaration.reads.push_back("StaticShadowPhysicalOwners");
	declaration.writes.push_back("StaticShadowClassificationInputs");
	declaration.writes.push_back("StaticShadowPhysicalOwners");
	declaration.writes.push_back("StaticShadowExecutionEvidence");
	declaration.safeSubwork += " Partitions copied complete light-selected caster proofs and plans deterministic queried byte admission in settled jobs.";
	declaration.protectedCommit += " Owns static allocation, CLEAR/copy/LOAD recording and separate static/final submit tokens; exact command completion retains retiring charges without adding an idle or scene flush.";
	declaration.safeSubwork += " Seals full light-selected instance streams from immutable roots and plans copied exact native compatibility in settled jobs.";
	declaration.protectedCommit += " Publishes separate frame/snapshot shadow instance ranges only after complete upload/descriptor proof and records actual grouped/singleton receipts; rejection restores the complete original draws.";
	declaration.safeSubwork += " Materializes exact-bit isolated depth recipes into immutable worker-owned CPU inputs.";
	declaration.protectedCommit += " Records physical depth query pairs and optional validation transfers; successful submit and existing fence completion qualify copied bounds, coverage, input and same-command color without query waits.";
	declaration.protectedReason = "Live GPU queue and presentation ownership remain protected after worker preparation.";
	declaration.reads.push_back(TerraEngine::Scheduling::kResourceRendererFrameScratch);
	declaration.writes.push_back(TerraEngine::Scheduling::kResourceRendererFrameScratch);
	declaration.safeSubwork += " Reads completed renderer-owned camera/light collections under one retained scratch episode; workers never allocate or reset that resource.";
	declaration.protectedCommit += " Retires prior continuation borrowers before opening renderer scratch, then publishes copied terminal episode/reclamation facts at existing protected boundaries.";
	declaration.profilingLabel = declaration.name;
	declaration.usesDefaultMetadata = false;
	return declaration;
}

std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> RenderSystem::DeclareFrameWorkloads(float deltaTime)
{
	using namespace TerraEngine::Scheduling;
	std::array<FrameWorkloadDeclaration, 5> workloads;
	std::size_t priorRouteDecisionCount = 0u;
	{
		// The last immutable snapshot is sufficient for dispatch granularity. A
		// first frame reports zero and therefore safely takes the inline path.
		std::lock_guard<std::mutex> lock(statsMutex);
		priorRouteDecisionCount = latestRenderFrameSnapshot.renderables.size();
	}

	// Protected capture publishes only immutable inputs. The route worker turns
	// those inputs into persistent topology and dynamic-state candidates; later
	// workers never reopen live ObjectManager or renderer-owned backend state.
	FrameWorkloadDeclaration& snapshot = workloads[0];
	snapshot.workloadName = "RenderPublicationCapture";
	snapshot.ownerSystem = "RenderSystem";
	snapshot.phase = "Render";
	snapshot.stableDescriptorId = "RenderSystem/Render/RenderPublicationCapture";
	snapshot.activationPredicateKey = "render-system-frame";
	snapshot.reads = {
		"Window", "SceneGraph", "Resources",
		kResourceRuntimeRenderSettingsQueue, "RendererEfficiencyOptionsQueue",
		kResourceSceneChangePublication,
		kResourceAnimationPresentationFrame,
		kResourceVirtualGeometryPagePublications,
		kResourceVirtualGeometryPageResidencySnapshots,
		kResourceVirtualGeometryCounterReadbacks,
		kResourceAnimatedGeometryPublications
	};
	snapshot.writes = {
		kResourceRuntimeRenderSettings, "RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		// Request admission stays at this protected boundary; workers consume
		// only the captured owners and never request or wait for a live load.
		"Resources", "StreamingNeeds",
		kResourceRenderCaptureInputs,
		kResourceAnimatedObjectVisibilitySnapshot
	};
	snapshot.explicitDependencies = { "VirtualGeometryStreamingSystem/Render/PagePublish" };
	snapshot.workerEligible = false;
	snapshot.mainThreadRequired = true;
	snapshot.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
	snapshot.phaseKind = WorkerSystemPhaseKind::Snapshot;
	snapshot.boundaryClassification = HardBoundaryClassification::Protected;
	snapshot.hardBoundary = true;
	snapshot.protectedCommit = "Admits deduplicated async requests for missing mesh/material publications, then captures the already-published scene change packet, exact transforms, resources, immutable shadowcaster lifetime/resource/deformation inputs, animation presentation, page publication, readback, requested views, backend capabilities, and renderer generations without lowering the crowd.";
	snapshot.protectedCommit += " Retires the previous continuation before allocating/resetting the exclusive renderer-owned camera/light scratch episode.";
	snapshot.protectedReason = snapshot.protectedCommit;
	snapshot.profilingLabel = "RenderSystem.RenderPublicationCapture";

	FrameWorkloadDeclaration& routePrepare = workloads[1];
	routePrepare.workloadName = "GeometryRoutePrepare";
	routePrepare.ownerSystem = "RenderSystem";
	routePrepare.phase = "Render";
	routePrepare.stableDescriptorId = "RenderSystem/Render/GeometryRoutePrepare";
	routePrepare.activationPredicateKey = snapshot.activationPredicateKey;
	routePrepare.reads = {
		"RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		kResourceRenderCaptureInputs,
		kResourceVirtualGeometryQualityProfile
	};
	routePrepare.writes = { "GeometryRouteDecisions", "DirectMeshletBatches",
		kResourcePersistentRenderSceneState, kResourceDynamicRenderFrameState,
		kResourcePersistentRenderTopology, kResourceRenderCompatibilityGroups };
	routePrepare.explicitDependencies = { snapshot.stableDescriptorId };
	routePrepare.workerEligible = true;
	routePrepare.mainThreadRequired = false;
	routePrepare.estimatedWorkItemCount = priorRouteDecisionCount;
	routePrepare.inlineWorkItemThreshold = 16u;
	routePrepare.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
	routePrepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
	routePrepare.safeSubwork = "Assembles immutable render records, scores representation candidates, applies calibrated hysteresis, commits transactional topology pages, and groups direct instances without touching live resources.";
	routePrepare.profilingLabel = "RenderSystem.GeometryRoutePrepare";
	routePrepare.safeSubwork += " Borrows completed camera/light scratch collections read-only; no allocation/reset occurs on workers.";

	FrameWorkloadDeclaration& prepare = workloads[2];
	prepare.workloadName = "VirtualGeometryPrepare";
	prepare.ownerSystem = "RenderSystem";
	prepare.phase = "Render";
	prepare.stableDescriptorId = "RenderSystem/Render/VirtualGeometryPrepare";
	prepare.activationPredicateKey = snapshot.activationPredicateKey;
	prepare.reads = {
		"RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		kResourcePersistentRenderSceneState, kResourceDynamicRenderFrameState,
		kResourceVirtualGeometryQualityProfile,
		kResourceVirtualGeometryPageResidencySnapshots,
		kResourceVirtualGeometryPagePublications,
		kResourceAnimatedGeometryOutputs, "GeometryRouteDecisions", "DirectMeshletBatches"
	};
	prepare.writes = {
		kResourceRenderViewSelections, kResourceDynamicRenderInstanceCandidates,
		kResourceCachedRenderViewEvidence,
		"RenderGroupingEvidence",
		kResourceVirtualGeometryClusterPackets, kResourceVirtualGeometryBatchPlans,
		kResourceVirtualGeometryPageNeedPackets, kResourceAnimatedGeometryDeformationPlans
	};
	prepare.explicitDependencies = { routePrepare.stableDescriptorId };
	prepare.workerEligible = true;
	prepare.mainThreadRequired = false;
	prepare.estimatedWorkItemCount = priorRouteDecisionCount;
	prepare.inlineWorkItemThreshold = 16u;
	prepare.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
	prepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
	prepare.safeSubwork = "Consumes only immutable persistent scene state, dynamic lanes, compact view selections, and prior immutable publications to produce typed virtual geometry selection, packet, page-need, and deformation plans.";
	prepare.writes.push_back("LightingDepthPreparedInputs");
	prepare.writes.push_back("ShadowGroupedPreparedInputs");
	prepare.writes.push_back("StaticShadowClassificationInputs");
	prepare.safeSubwork += " Seals each light's immutable instance rows without using camera-visible membership.";
	prepare.safeSubwork += " Per-view preparation materializes admitted exact-bit depth recipes into immutable owned bytes.";
	prepare.profilingLabel = "RenderSystem.VirtualGeometryPrepare";
	prepare.safeSubwork += " Reads the retained camera/light scratch episode, with all child borrowers settled before continuation retirement.";

	FrameWorkloadDeclaration& commit = workloads[3];
	commit.workloadName = "BackendCommit";
	commit.ownerSystem = "RenderSystem";
	commit.phase = "Render";
	commit.stableDescriptorId = "RenderSystem/Render/BackendCommit";
	commit.activationPredicateKey = snapshot.activationPredicateKey;
	commit.reads = {
		"RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		"LightingDepthPreparedInputs", "LightingDepthQueryOwners",
		kResourcePersistentRenderSceneState, kResourceDynamicRenderFrameState,
		kResourceRenderViewSelections, kResourceVirtualGeometryClusterPackets,
		kResourceVirtualGeometryBatchPlans,
		kResourceVirtualGeometryPageNeedPackets, kResourceAnimatedGeometryDeformationPlans
	};
	commit.writes = {
		"LightingDepthQueryOwners", "LightingDepthEvidence",
		kResourceRenderFramePublication, kResourceRenderInstanceStreamPublication,
		kResourceVirtualGeometryGpuPacketFrameSets, kResourceVirtualGeometryGeneratedSubmissions,
		kResourceVirtualGeometryCounterReadbacks, kResourceVirtualGeometryVisibilitySnapshots
	};
	commit.reads.push_back("ShadowGroupedPreparedInputs");
	commit.reads.push_back("ShadowGroupedPhysicalOwners");
	commit.writes.push_back("ShadowGroupedPhysicalOwners");
	commit.writes.push_back("ShadowGroupedExecutionEvidence");
	commit.reads.push_back("StaticShadowClassificationInputs");
	commit.reads.push_back("StaticShadowPhysicalOwners");
	commit.writes.push_back("StaticShadowPhysicalOwners");
	commit.writes.push_back("StaticShadowExecutionEvidence");
	commit.explicitDependencies = { prepare.stableDescriptorId };
	commit.workerEligible = false;
	commit.mainThreadRequired = true;
	commit.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
	commit.boundaryClassification = HardBoundaryClassification::Protected;
	commit.hardBoundary = true;
	commit.protectedCommit = "Validates and seals one render-frame publication, then owns backend resource mutation, typed-range recording, queue submission, pending counters, and visibility targets.";
	commit.protectedCommit += " Depth query recording, actual submit/completion ownership and optional completed validation copies stay here; workers do not issue Vulkan commands or mutate the scene.";
	commit.protectedCommit += " Consumes captured camera/light scratch collections without reclaiming their resource while borrowed.";
	commit.protectedReason = commit.protectedCommit;
	commit.profilingLabel = "RenderSystem.BackendCommit";

	FrameWorkloadDeclaration& publication = workloads[4];
	publication.workloadName = "RenderPublication";
	publication.ownerSystem = "RenderSystem";
	publication.phase = "Render";
	publication.stableDescriptorId = "RenderSystem/Render/RenderPublication";
	publication.activationPredicateKey = snapshot.activationPredicateKey;
	publication.reads = {
		kResourceRendererFrameScratch,
		kResourceRenderFramePublication,
		kResourceVirtualGeometryGeneratedSubmissions,
		kResourceVirtualGeometryVisibilitySnapshots
	};
	publication.writes = {
		"RendererEfficiencyEvidence",
		"RendererDiagnostics", kResourceVirtualGeometryDiagnostics,
		kResourceProfilerFramePublicationEvidence
	};
	publication.explicitDependencies = { commit.stableDescriptorId };
	publication.workerEligible = false;
	publication.mainThreadRequired = true;
	publication.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
	publication.boundaryClassification = HardBoundaryClassification::Protected;
	publication.hardBoundary = true;
	publication.sameFrameVisibilityRequired = true;
	publication.protectedCommit = "Publishes complete render-frame receipt, diagnostics, profiler evidence, and visibility results only after protected backend work completes.";
	publication.protectedCommit += " Retires the completed continuation and copies terminal scratch diagnostics; reclamation stays at the next protected capture or lifecycle boundary.";
	publication.protectedReason = publication.protectedCommit;
	publication.profilingLabel = "RenderSystem.RenderPublication";

	// Each callback resumes one explicit section of the same frame continuation.
	// This preserves local immutable state without repeating scene capture or
	// reconstructing commands between scheduler phases.
	snapshot.work = [this, deltaTime](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Protected Publication Capture",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RetireFrameExecutionAtProtectedBoundary();
		RenderSystemLifetimeState::FramePhaseState captured;
		captured.deltaTime = deltaTime;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			captured.captureSequence = lifetimeState->framePhase.captureSequence + 1U;
			captured.execution = std::make_unique<RenderFrameExecution>(
				BuildFrameExecution(deltaTime));
			lifetimeState->framePhase = std::move(captured);
		}
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.captureReady = hasMore;
	};
	routePrepare.work = [this](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Geometry Route Preparation",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.captureReady) return;
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.routeReady = hasMore;
	};
	prepare.work = [this](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Virtual Geometry Preparation",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.routeReady) return;
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.virtualGeometryReady = hasMore;
	};
	commit.work = [this](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Protected Backend Commit",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.virtualGeometryReady) return;
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.backendComplete = execution != nullptr && !hasMore;
	};
	publication.work = [this](JobGraphContext&) {
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		std::unique_ptr<RenderFrameExecution> retired;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.backendComplete) return;
		// Update sealed the render root, submitted the backend frame, and swapped
		// complete diagnostics. This final receipt makes that completed boundary
		// explicit to scheduler diagnostics without rebuilding any scene data.
			lifetimeState->framePhase.publicationComplete = true;
			retired = std::move(lifetimeState->framePhase.execution);
		}
		retired.reset();
		ObserveFrameScratchAtProtectedBoundary();
	};
	return { workloads.begin(), workloads.end() };
}

TerraEngine::Scheduling::HardBoundary RenderSystem::GetLiveGpuSubmissionBoundary() const
{
	for (const TerraEngine::Scheduling::HardBoundary& boundary : TerraEngine::Scheduling::InitialWorkerHardBoundaries())
	{
		if (boundary.boundaryName == "LiveGpuSubmission")
		{
			return boundary;
		}
	}
	return {};
}

std::vector<TerraEngine::Scene::RenderRuntimeRecord> RenderSystem::BuildCompactRenderRuntimeRecords()
{
	std::vector<TerraEngine::Scene::RenderRuntimeRecord> records;
	objManager.forEachObjectWithComponents<Transform, CameraComponent>(
		[&](Object object, Transform&, CameraComponent&)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Camera;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
		[&](Object object, Transform&, MeshRenderComponent& mesh)
		{
			// TerrainComponent has its own derived binding and is recorded below.
			// Skipping it here prevents a legacy proxy component from double-counting.
			if(objManager.getRegistry().any_of<TerrainComponent>(object.EntityId())) return;
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable;
			record.entity = object.EntityId();
			record.objectId = mesh.spatialObjectId != TerraEngine::EMPTY_UUID ? mesh.spatialObjectId : objManager.getUUID(object);
			record.resourceIds = { mesh.meshId, mesh.materialId };
			record.renderFlags = mesh.taskShaderCullingEnabled ? 1u : 0u;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			if (mesh.meshId == TerraEngine::EMPTY_UUID || mesh.materialId == TerraEngine::EMPTY_UUID)
			{
				record.rejectionReason = "missing-render-resource-id";
			}
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, TerrainComponent>(
		[&](Object object, Transform&, TerrainComponent& terrain)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.renderFlags = 0U;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			const auto proxy = terrain.visible && terrain.terrainAssetId != TerraEngine::EMPTY_UUID
				? TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
					.Resolve(terrain.terrainAssetId)
				: std::nullopt;
			if(proxy && proxy->IsComplete())
			{
				record.resourceIds = {proxy->meshId, proxy->materialId};
			}
			else
			{
				record.rejectionReason = "terrain-render-proxy-unavailable";
			}
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, LightComponent>(
		[&](Object object, Transform&, LightComponent& light)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Light;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.renderFlags = light.castsShadows ? 1u : 0u;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			records.push_back(std::move(record));
		});

	return records;
}

bool RenderSystem::CompactRenderRecordsMatchSnapshot(const std::vector<TerraEngine::Scene::RenderRuntimeRecord>& records, const TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
{
	std::size_t renderableCount = 0u;
	std::size_t lightCount = 0u;
	for (const TerraEngine::Scene::RenderRuntimeRecord& record : records)
	{
		if (!record.rejectionReason.empty())
		{
			continue;
		}
		if (record.recordKind == TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable)
		{
			++renderableCount;
		}
		else if (record.recordKind == TerraEngine::Scene::RuntimeLayoutRecordKind::Light)
		{
			++lightCount;
		}
	}
	return renderableCount == snapshot.renderables.size() && lightCount == snapshot.lights.size();
}






















































































































































TerraEngine::Renderer::RendererEfficiencyFixtureIntentReceipt RenderSystem::QueueRendererEfficiencyFixtureIntent(
	const TerraEngine::Renderer::RendererEfficiencyFixtureIntent& intent)
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	TerraEngine::Renderer::RendererEfficiencyFixtureIntentReceipt receipt;
	receipt.correlation = intent.correlation;
	const auto options = rendererEfficiencyOptionsQueue.Snapshot();
	const auto& correlation = intent.correlation;
	if (correlation.operationId.empty() || correlation.fixtureStepId.empty()
		|| correlation.rendererSessionGeneration == 0U
		|| correlation.rendererSessionGeneration != options.rendererSessionGeneration
		|| correlation.settingsGeneration != options.settingsGeneration)
	{ receipt.reason = "Fixture correlation belongs to an unavailable settings/renderer generation."; return receipt; }
	if (intent.viewOrder.size() > 2 || intent.viewOrigins.size() > 2 || intent.cameras.size() > 2 || intent.casterSelections.size() > 2 || (intent.referenceOriginDelta && !intent.viewOrigins.empty()))
	{ receipt.reason = "Fixture view controls exceed bounded outputs or specify ambiguous origins."; return receipt; }
	if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureCameras(intent, receipt.reason)) return receipt;
	if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureCasterSelections(intent, receipt.reason)) return receipt;
	if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureLightingControls(intent, receipt.reason)) return receipt;
	if (!ValidateDepthFixtureIntent(intent,receipt.reason)) return receipt;
#if !defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
	if (intent.injection != TerraEngine::Renderer::RendererEfficiencyFixtureInjection::None)
	{ receipt.reason = "Recorded-submit injection is unavailable until its protected backend control is installed."; return receipt; }
#else
	if (intent.injection > TerraEngine::Renderer::RendererEfficiencyFixtureInjection::ShadowResourceRecreation)
	{ receipt.reason = "Unknown validation-only submit injection."; return receipt; }
#endif
	const auto& activeOperation = capturedRendererEfficiencyFixtureIntent.correlation.operationId;
	if ((!activeOperation.empty() && activeOperation != correlation.operationId)
		|| (pendingRendererEfficiencyFixtureIntent && pendingRendererEfficiencyFixtureIntent->correlation.operationId != correlation.operationId))
	{ receipt.reason = "Another fixture operation already owns the renderer intent."; return receipt; }
	// Ingress copies the whole intent. Only the existing settings/capture boundary
	// publishes it, preserving frame identity while jobs prepare immutable work.
	auto copied = intent;
	const auto& prior = pendingRendererEfficiencyFixtureIntent ? *pendingRendererEfficiencyFixtureIntent : capturedRendererEfficiencyFixtureIntent;
	const bool sameControl = prior.correlation.operationId == correlation.operationId && prior.correlation.fixtureStepId == correlation.fixtureStepId
		&& prior.referenceOriginDelta == intent.referenceOriginDelta && prior.shadowSelection == intent.shadowSelection && prior.injection == intent.injection
		&& prior.viewOrder == intent.viewOrder && prior.viewOrigins == intent.viewOrigins && prior.cameras == intent.cameras
		&& prior.casterSelections == intent.casterSelections && prior.lightingControls == intent.lightingControls
		&& prior.depthControls == intent.depthControls
		&& prior.shadowBatchControls == intent.shadowBatchControls
		&& prior.staticShadowControls == intent.staticShadowControls;
	copied.controlGeneration = sameControl && prior.controlGeneration ? prior.controlGeneration : ++nextRendererEfficiencyFixtureControlGeneration;
	copied.controlsApplied = false; copied.controlReason.clear();
	receipt.controlGeneration = copied.controlGeneration;
	pendingRendererEfficiencyFixtureIntent = std::move(copied);
	receipt.accepted = true;
	return receipt;
}
































// Settings persistence is implemented by SaveSettings in RenderSystemSettings.cpp.
// Queued runtime changes apply at protected capture before input snapshots.
