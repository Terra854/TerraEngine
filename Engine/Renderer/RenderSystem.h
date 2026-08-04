// RenderSystem.h — Render System Definition
//
// Defines the RenderSystem class, the primary high-level coordinator for all engine rendering operations.
// It manages the frame lifecycle, integrates with the RHI to submit commands to the GPU, and handles scene-to-renderer data synchronization.
// The system oversees specialized features like deferred shading, shadow mapping, and post-processing.

#pragma once

#include "Core/SystemBase.h"
#include "RHI/RHIContext.h"
#include "Core/Object/ObjectManager.h"
#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <entt/entt.hpp>
#include "Components/Transform.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/LightComponent.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/RendererRecoveryPolicy.h"
#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryReplay.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryViewPolicy.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Scene/RuntimeLayout.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SpatialDiagnostics.h"
#include "Scene/SpatialIndex.h"

namespace TerraEngine::Renderer
{
	// RenderTransformCacheDiagnostics makes cache reuse observable without exposing
	// mutable scene data to the renderer's callers or diagnostics windows.
	struct RenderTransformCacheDiagnostics
	{
		bool rebuiltForHierarchy = false;
		bool reusedWithoutPropagation = false;
		bool usedFlatHierarchyFastPath = false;
		std::uint32_t scannedObjectCount = 0U;
		std::uint32_t changedObjectCount = 0U;
		std::uint32_t propagatedObjectCount = 0U;
		std::uint32_t hierarchyLevelCount = 0U;
		std::uint64_t hierarchyRevision = 0U;
	};

	// RenderWorldTransformCache owns copied world transforms for the protected
	// render boundary. It rebuilds the hierarchy only when its revision changes,
	// then propagates only locally changed branches on later frames.
	class RenderWorldTransformCache
	{
	public:
		const std::unordered_map<entt::entity, Transform>& Refresh(
			ObjectManager& objects,
			RenderTransformCacheDiagnostics& diagnostics);
		[[nodiscard]] const TerraEngine::Scene::SceneTransformPropagationResult& LatestPropagation() const;
		void Clear();

	private:
		TerraEngine::Scene::SceneGraph graph;
		std::unordered_map<entt::entity, Transform> worldTransforms;
		bool initialized = false;
		// Scene generation is independent from hierarchy revision. A bulk scene
		// replacement can rebuild the same topology and therefore reuse the same
		// hierarchy revision while every entity/component lifetime has changed.
		std::uint64_t sceneGeneration = 0U;
		std::uint64_t hierarchyRevision = 0U;
		std::uint64_t spatialRevision = 0U;
		std::uint32_t hierarchyLevelCount = 0U;
		bool flatHierarchy = false;
		TerraEngine::Scene::SceneTransformPropagationResult latestPropagation;
	};
}

class RenderSystem : public SystemsBase
{
public:
	// This summary proves diagnostics submission without exposing RHI/provider
	// pointers. CPU Scene View overlays can remain useful when no GPU exists.
	struct SceneViewPhysicsDebugSubmissionSummary
	{
		bool rendererAvailable = false;
		std::size_t packetCount = 0U;
		std::size_t primitiveCount = 0U;
	};
	static RenderSystem& Instance();

	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;
	RenderSystem(RenderSystem&&) = delete;
	RenderSystem& operator=(RenderSystem&&) = delete;
	~RenderSystem() override = default;

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	bool RequestsEngineShutdown() const override;
	std::string SystemName() override;
	TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
	// Split the renderer's frame contract into scheduler-visible boundaries. The
	// current renderer still executes its established frame body at the protected
	// snapshot boundary while later virtual-geometry work replaces those internal
	// sections with the typed plans named by these descriptors.
	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

	void ReloadMaterial(const TerraEngine::UUID& materialId);
	void ReloadMesh(const TerraEngine::UUID& meshId);
	void ReloadTexture(const TerraEngine::UUID& textureId);

	void DeleteResource(const TerraEngine::UUID& id);
	void RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName);
	
	void Clear();
	void SetTaskShaderCullingEnabled(bool enabled);
	// IsTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsTaskShaderCullingEnabled() const { return taskShaderCullingEnabled; }
	RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const;
	TerraEngine::Scene::SpatialDiagnosticsSnapshot GetLatestSpatialDiagnostics(RenderViewType viewType) const;
	FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const;
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const;
	RenderSnapshotDiagnosticsSnapshot GetLatestRenderSnapshotDiagnostics() const;
	TerraEngine::Renderer::RendererRecoverySnapshot GetLatestRendererRecoverySnapshot() const;
	TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
		GetLatestAnimationGpuBackendExecution() const;
	// Return a copied renderable list from the last protected frame. Automation
	// diagnostics use this to associate each animated mesh object with its exact
	// immutable CPU/GPU deformation generation without exposing live render data.
	std::vector<TerraEngine::Renderer::RenderFrameRenderableRecord>
		GetLatestRenderablesSnapshot() const;
	// Return only the demand count from the protected render-frame snapshot.
	// Automation can explain page-deformation eligibility without copying page
	// identities or backend-owned work records.
	std::size_t GetLatestAnimatedGeometryPageDemandCount() const;
	// Scene/editor tools receive copied semantic visibility records from the last
	// completed protected frame. They never map or retain backend-owned buffers.
	std::vector<GpuVirtualGeometryPacketRecord>
		GetLatestVirtualGeometryVisibilityRecords(RenderViewType viewType) const;
	// Resolve a normalized viewport point against already-compacted records. The
	// smallest covering cluster wins; stable debug identity breaks equal-area
	// ties so hardware, compute, and indexed routes choose the same object.
	static std::optional<VirtualGeometryVisibilityPick>
		PickVirtualGeometryVisibilityRecord(
			std::span<const GpuVirtualGeometryPacketRecord> records,
			const glm::vec2& normalizedViewportPosition);
	// Queue one non-blocking GPU identity copy. Returning false means callers
	// must use the explicit CPU-projection fallback and must not claim GPU proof.
	bool RequestConventionalVisibilityIdentityPick(
		RenderViewType viewType, const glm::uvec2& pixel);
	std::optional<VirtualGeometryVisibilityPick>
		GetLatestAuthoritativeConventionalVisibilityIdentityPick(
			RenderViewType viewType, const glm::uvec2& pixel) const;
	// Return the exact asynchronous state for an identity request. Callers may
	// poll this copied value but must never infer readiness from CPU projection.
	TerraEngine::Renderer::AuthoritativeVisibilityPickState
		GetConventionalVisibilityIdentityPickState(
			RenderViewType viewType, const glm::uvec2& pixel) const;
	// These validation controls are transient editor state. Hardware, Compute,
	// and Indexed are real packet routes; hybrid deliberately keeps no fourth
	// route and instead requests a mixed packet/material validation scene.
	bool SetVirtualGeometryValidationRouteOverride(
		TerraEngine::Renderer::VirtualGeometryRasterRoute route,
		std::string* outReason = nullptr);
	void SetVirtualGeometryHybridValidationSceneRequested(bool requested);
	void ClearVirtualGeometryValidationRouteOverride();
	std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
		GetVirtualGeometryValidationRouteOverride() const;
	bool IsVirtualGeometryHybridValidationSceneRequested() const;
	void SetRendererRecoveryPolicy(const TerraEngine::Renderer::RendererRecoveryPolicy& policy);
	TerraEngine::Renderer::RendererRecoveryPolicy GetRendererRecoveryPolicy() const;
	std::vector<TerraEngine::Renderer::OcclusionDebugSample> GetLatestVirtualGeometryOcclusionDebugSamples(RenderViewType viewType, std::size_t maxCount = TerraEngine::Renderer::kVirtualGeometryOcclusionDebugSampleCapacity) const;
	TerraEngine::Renderer::RenderDiagnosticsSummary GetLatestRenderGraphDiagnostics() const;
	// Profiler presentation needs only submit-stage timings. Returning that
	// projection avoids copying cost samples, stall messages, and per-view work
	// records under the renderer statistics mutex.
	std::vector<TerraEngine::Renderer::BackendStageTiming> GetLatestBackendStageTimings() const;
	TerraEngine::Renderer::RenderCaptureAvailability GetCaptureAvailability() const;
	TerraEngine::Renderer::RenderCaptureLimits GetCaptureLimits() const;
	TerraEngine::Renderer::RenderCaptureQueueState GetCaptureQueueState() const;
	std::string QueueCaptureRequest(TerraEngine::Renderer::RenderCaptureRequest request, std::string& errorMessage);
	std::optional<TerraEngine::Renderer::RenderCaptureResult> PollCaptureResult(std::string_view requestId);
	void ProcessQueuedCaptureRequests();
	TerraEngine::Renderer::RenderCaptureResult CaptureImage(const TerraEngine::Renderer::RenderCaptureRequest& request);
	void SetRenderGraphDiagnosticsRequested(bool requested);
	[[nodiscard]] TerraEngine::Scheduling::HardBoundary GetLiveGpuSubmissionBoundary() const;
	std::vector<TerraEngine::Scene::RenderRuntimeRecord> BuildCompactRenderRuntimeRecords();
	static bool CompactRenderRecordsMatchSnapshot(const std::vector<TerraEngine::Scene::RenderRuntimeRecord>& records, const TerraEngine::Renderer::RenderFrameSnapshot& snapshot);
	// This pure resolver accepts only copied identities and immutable publication
	// ownership. It is safe to unit test without a renderer, scene, or GPU.
	static TerraEngine::Renderer::AnimationRenderBinding ResolveAnimationRenderBinding(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request) noexcept;
	// Converts one sealed CPU pose to the exact affine palette and morph lanes
	// consumed by the default mesh shader. Inputs are copied asset metadata only;
	// no renderer or live scene state is accessed.
	static std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>
		BuildCpuAnimationDeformation(
			const TerraEngine::Animation::PublishedAnimatorPose& publication,
			std::span<const std::uint32_t> paletteJoints,
			std::span<const std::uint64_t> morphTargetIds) noexcept;
	static TerraEngine::Renderer::VirtualGeometryDrawRange SelectVirtualGeometryLodRange(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryQualityProfile& quality, float projectedRadiusPixels, bool taskShaderCullingEnabled, bool boundsValid);
	static TerraEngine::Renderer::VirtualGeometryDrawRange SelectDirectResidentLodRange(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryQualityProfile& quality, float projectedRadiusPixels);
	static TerraEngine::Renderer::VirtualGeometryDrawRange SelectVirtualGeometryResidentLodRange(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryQualityProfile& quality, float projectedRadiusPixels, bool taskShaderCullingEnabled, bool boundsValid);
	static TerraEngine::Renderer::VirtualGeometryDrawRange SelectVirtualGeometryShadowLodRange(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryQualityProfile& quality, bool boundsValid);
	static TerraEngine::Renderer::VirtualGeometryClusterSelectionResult SelectVirtualGeometryClusterCut(const VirtualGeometryAssetRuntime& asset, const TerraEngine::UUID& meshId, const TerraEngine::UUID& objectId, const VirtualGeometryQualityProfile& quality, float projectedRadiusPixels, bool taskShaderCullingEnabled, bool boundsValid);
	static TerraEngine::Renderer::VirtualGeometryClusterSelectionResult SelectVirtualGeometryClusterCut(const VirtualGeometryAssetRuntime& asset, const TerraEngine::UUID& meshId, const TerraEngine::UUID& objectId, const VirtualGeometryQualityProfile& quality, float projectedRadiusPixels, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, bool taskShaderCullingEnabled, bool boundsValid);
	static TerraEngine::Renderer::VirtualGeometryClusterSelectionResult SelectVirtualGeometryStableClusterCut(const VirtualGeometryAssetRuntime& asset, const TerraEngine::UUID& meshId, const TerraEngine::UUID& objectId, const VirtualGeometryQualityProfile& quality, float projectedRadiusPixels, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, bool taskShaderCullingEnabled, bool boundsValid, const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult* previousStableSelection, RenderViewType viewType, std::uint32_t outputId);
	static std::vector<VirtualGeometryClusterPacket> BuildVirtualGeometryClusterPackets(const VirtualGeometryAssetRuntime& asset, const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters, const TerraEngine::UUID& materialId, RenderViewType viewType, std::uint32_t viewId, std::uint32_t packetIdBase = 0u);
	static std::vector<ClusterWorkBatch> BuildVirtualGeometryClusterBatches(const std::vector<VirtualGeometryClusterPacket>& packets, std::uint32_t packetOffsetBase = 0u, std::uint32_t batchIdBase = 0u);
	// These helpers convert the proven CPU cluster cut into immutable GPU packet
	// mirrors. The render thread can build them as worker-safe values, while the
	// protected Vulkan boundary owns the actual buffer publication. GPU packets
	// preserve the CPU packet ids because page-local shader descriptors are
	// addressed by that stable identity after packet culling/compaction.
	static std::vector<GpuVirtualGeometryPacketRecord> BuildGpuVirtualGeometryPacketRecords(const std::vector<VirtualGeometryClusterPacket>& packets, std::uint64_t frameId, VirtualGeometryDebugVisualizationMode debugMode, std::uint32_t packetIdBase = 0u);
	static std::vector<GpuVirtualGeometryBatchDescriptor> BuildGpuVirtualGeometryBatchDescriptors(const std::vector<GpuVirtualGeometryPacketRecord>& packets, std::uint64_t frameId, VirtualGeometryDebugVisualizationMode debugMode, std::uint32_t batchIdBase = 0u);
	static GpuVirtualGeometryPacketFrameSet BuildGpuVirtualGeometryPacketFrameSet(RenderViewType viewType, std::uint32_t viewId, std::uint64_t frameId, const glm::uvec2& targetSize, VirtualGeometryDebugVisualizationMode debugMode, std::uint32_t packetOffset, std::uint32_t packetCount, std::uint32_t batchOffset, std::uint32_t batchCount);
	static bool ValidateGpuVirtualGeometryPacketRecords(const std::vector<VirtualGeometryClusterPacket>& cpuPackets, const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters, const std::vector<GpuVirtualGeometryPacketRecord>& gpuPackets, std::uint64_t frameId, VirtualGeometryDebugVisualizationMode debugMode, std::string* outReason = nullptr);
	static std::vector<GpuPacketCompactionDispatch> BuildGpuPacketCompactionDispatches(const std::vector<GpuVirtualGeometryPacketFrameSet>& frameSets, const VirtualGeometryHiZValidationState& hiZState, VirtualGeometryDebugVisualizationMode debugMode, std::uint32_t dispatchIdBase = 0u);
	static std::vector<GeneratedVirtualGeometrySubmissionRecord> BuildGeneratedVirtualGeometrySubmissionRecords(const std::vector<GpuVirtualGeometryBatchDescriptor>& batches, const std::vector<GpuVirtualGeometryPacketRecord>& packets, std::uint64_t frameId, std::uint32_t recordIdBase = 0u);
	static bool ValidateGeneratedVirtualGeometrySubmissionRecords(const std::vector<GeneratedVirtualGeometrySubmissionRecord>& submissions, const std::vector<GpuVirtualGeometryBatchDescriptor>& batches, std::uint64_t frameId, RenderViewType viewType, std::uint32_t viewId, std::uint32_t packetCapacity, std::string* outReason = nullptr);
	static VirtualGeometryCounterReadbackRecord BuildVirtualGeometryCounterReadbackRecord(const CompactedVisibilitySet& compaction, std::uint64_t frameId, VirtualGeometryCounterSource source, std::uint32_t latencyFrames = 0u, std::string unavailableReason = {});
	static VirtualGeometryGpuFallbackRecord BuildVirtualGeometryGpuFallbackRecord(RenderViewType viewType, std::uint32_t viewId, std::uint64_t frameId, std::string reason, std::uint32_t affectedPackets, std::uint32_t affectedBatches, std::string diagnostic = {});
	static bool AreVirtualGeometryClustersConeCullingEligible(const VirtualGeometryAssetRuntime& asset, const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters, std::string* outDisabledReason = nullptr);
	static CompactedVisibilitySet CompactVirtualGeometryClusterPackets(const std::vector<VirtualGeometryClusterPacket>& candidatePackets, std::vector<VirtualGeometryClusterPacket>& compactedPackets, RenderViewType viewType, std::uint32_t viewId, bool failOpenForMissingResourcePackets = true);

	void SetQualityPreset(QualityPreset preset);
	// GetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	QualityPreset GetQualityPreset() const { return currentPreset; }
	void SetRenderSettings(const RenderSettings& settings);
	// Managed gameplay callbacks enqueue game-wide runtime overrides here. The
	// render boundary applies the latest request before it builds frame snapshots,
	// and deliberately does not rewrite the project's RenderSettings.json file.
	bool QueueRuntimeQualityPreset(QualityPreset preset);
	bool QueueRuntimeRenderSettings(const RenderSettings& settings);
	RenderSettings GetRuntimeRenderSettingsSnapshot() const;
	QualityPreset GetRuntimeQualityPresetSnapshot() const;
	std::uint64_t GetRuntimeRenderSettingsGeneration() const;
	// Applies editor/user diagnostic preferences to the running renderer without
	// saving RenderSettings.json, keeping Preferences separate from project data.
	void ApplyTransientRenderDiagnostics(bool validationEnabled, bool debugEnabled);
	// Clears the editor override so tests and project-only flows fall back to
	// the diagnostics authored in RenderSettings.json.
	void ClearTransientRenderDiagnosticsOverride();
	const RenderSettings& GetRenderSettings() const { return currentSettings; }
	void SetVirtualGeometryQualityProfile(const VirtualGeometryQualityProfile& profile);
	VirtualGeometryQualityProfile GetVirtualGeometryQualityProfile() const;
	void SetVirtualGeometryDebugVisualizationMode(RenderViewType viewType, std::uint32_t outputId, VirtualGeometryDebugVisualizationMode mode);
	VirtualGeometryDebugVisualizationMode GetVirtualGeometryDebugVisualizationMode(RenderViewType viewType, std::uint32_t outputId) const;
	void ClearVirtualGeometryDebugVisualizationModes();
	VirtualGeometryInactiveReason GetVirtualGeometryInactiveReason() const;
	SkyRuntimeState GetLatestSkyRuntimeState() const;
	void MarkSkyStateDirty();

	void LoadSettings();
	void SaveSettings();

	// GetContext is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	RHIContext* GetContext() const { return rhi.get(); }

	struct OverrideCameraData {
		glm::mat4 view;
		glm::mat4 proj;
		glm::vec3 pos;
		bool orthographic = false;
	};
	// SetSceneViewCamera is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetSceneViewCamera(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& pos, bool orthographic = false) {
		sceneViewCamera = OverrideCameraData{view, proj, pos, orthographic};
		hasSceneViewCamera = true;
	}
	// Record the editor panel dimensions before rendering so Vulkan allocates
	// offscreen targets that match the ImGui image rectangle rather than the
	// whole operating-system window.
	void SetSceneViewTargetSize(const glm::uvec2& size) {
		sceneViewTargetSize = size;
	}
	void SetSceneViewGridVisible(bool visible) {
		sceneViewGridVisible = visible;
	}
	// Records immutable debug packets for the Scene View render boundary. A
	// missing RHI returns false and deliberately leaves prior diagnostics alone.
	bool SubmitSceneViewPhysicsDebugPackets(
		const std::vector<std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>>& packets,
		const TerraEngine::Physics::PhysicsDebugDisplaySelection& selection);
	[[nodiscard]] SceneViewPhysicsDebugSubmissionSummary GetLatestSceneViewPhysicsDebugSubmission() const;
	void SetSceneViewContextTint(const std::unordered_set<TerraEngine::UUID>& focusedObjectIds);
	// SetEditorModeEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetEditorModeEnabled(bool enabled) { editorModeEnabled = enabled; }
	// SetGameViewCamera is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetGameViewCamera(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& pos) {
		OverrideCameraData camera{view, proj, pos, false};
		gameViewCameras.push_back(camera);
		// Store a descriptor copy for MCP capture requests. This is read-only
		// metadata, not gameplay camera ownership, so capture can inspect the
		// most recent Game View without promoting or mutating scene cameras.
		latestGameViewCamera = camera;
		hasLatestGameViewCamera = true;
	}
	void SetGameViewTargetSize(const glm::uvec2& size) {
		gameViewTargetSize = size;
		if (size.x > 0u && size.y > 0u)
		{
			latestGameViewTargetSize = size;
		}
	}
	// Isolated prefab asset editing does not have an active game scene, so the
	// editor can suppress Game View submission for that frame and leave GPU time
	// available for Scene View and the rest of the desktop.
	void SetGameViewSuppressed(bool suppressed) { gameViewSuppressed = suppressed; }
	// ClearOverrideCameras is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ClearOverrideCameras() {
		hasSceneViewCamera = false;
		gameViewCameras.clear();
		gameViewSuppressed = false;
		sceneViewTargetSize = glm::uvec2(0u);
		gameViewTargetSize = glm::uvec2(0u);
		sceneViewGridVisible = true;
		sceneViewContextTintEnabled = false;
		sceneViewContextFocusedObjectIds.clear();
	}

private:
	RenderSystem();
	void ApplyQualityPreset(QualityPreset preset, bool persistProjectSettings);
	void ApplyQueuedRuntimeRenderSettings();
	void CompleteCapture(TerraEngine::Renderer::RenderCaptureResult result);
	// Moves non-blocking backend fence completions into the bounded public
	// result queue. This runs only at the protected render boundary.
	void DrainCaptureReadbacks();
	void DrainConventionalVisibilityIdentityReadbacks();
	bool ResolveRenderDebugDiagnosticsEnabled() const;
	void ApplyRenderDiagnosticsRequest();
	void ReportFatalRendererRecoveryFailure(TerraEngine::Renderer::RendererRecoverySnapshot snapshot);
	std::unique_ptr<RHIContext> rhi;
	ObjectManager& objManager = ObjectManager::instance();
	bool initialized = false;
	bool taskShaderCullingEnabled = true;
	bool editorModeEnabled = false;
	bool sceneViewGridVisible = true;
	SceneViewPhysicsDebugSubmissionSummary latestSceneViewPhysicsDebugSubmission{};
	bool renderGraphDiagnosticsRequestedByUi = false;
	std::optional<bool> transientRenderValidationOverride;
	std::optional<bool> transientRenderDebugDiagnosticsOverride;

	bool hasSceneViewCamera = false;
	OverrideCameraData sceneViewCamera;
	std::vector<OverrideCameraData> gameViewCameras;
	bool hasLatestGameViewCamera = false;
	OverrideCameraData latestGameViewCamera{};
	glm::uvec2 latestGameViewTargetSize = glm::uvec2(0u);
	std::uint64_t latestGameViewFrameId = 0u;
	glm::uvec2 sceneViewTargetSize = glm::uvec2(0u);
	glm::uvec2 gameViewTargetSize = glm::uvec2(0u);
	bool gameViewSuppressed = false;
	// Scene View prefab-context editing keeps the selected prefab branch normal
	// and submits a light tint for every other draw command in that view only.
	std::unordered_set<TerraEngine::UUID> sceneViewContextFocusedObjectIds;
	bool sceneViewContextTintEnabled = false;

	// Internal structs for data gathering
	// struct CameraEntry groups related state and operations so callers have one documented contract.
	struct CameraEntry
	{
		entt::entity entity;
		CameraComponent camera;
	};
	// struct RenderableEntry groups related state and operations so callers have one documented contract.
	struct RenderableEntry
	{
		entt::entity entity;
		TerraEngine::UUID objectId;
		bool isStatic = false;
		MeshRenderComponent mesh;
		// Protected collection resolves this pointer bundle before any worker-safe
		// signature, visibility, or draw preparation sees the renderable.
		TerraEngine::Renderer::AnimationRenderBinding animationBinding{};
	};
	// struct LightEntry groups related state and operations so callers have one documented contract.
	struct LightEntry
	{
		entt::entity entity;
		TerraEngine::UUID lightId;
		LightComponent light;
	};

	mutable std::mutex statsMutex;
	TerraEngine::Scene::SpatialIndex spatialIndex;
	RenderStatsSnapshot latestGameStats{};
	RenderStatsSnapshot latestSceneStats{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestGameSpatialDiagnostics{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestSceneSpatialDiagnostics{};
	FrameBudgetDiagnosticsSnapshot latestGameBudgetDiagnostics{};
	FrameBudgetDiagnosticsSnapshot latestSceneBudgetDiagnostics{};
	LightingDiagnosticsSnapshot latestGameLightingDiagnostics{};
	LightingDiagnosticsSnapshot latestSceneLightingDiagnostics{};
	RenderSnapshotDiagnosticsSnapshot latestRenderSnapshotDiagnostics{};
	TerraEngine::Renderer::RendererRecoverySnapshot latestRendererRecoverySnapshot{};
	TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
		latestAnimationGpuBackendExecution{};
	TerraEngine::Renderer::RendererRecoveryPolicy rendererRecoveryPolicy = TerraEngine::Renderer::MakeEditorRendererRecoveryPolicy();
	TerraEngine::Renderer::RenderDiagnosticsSummary latestRenderGraphDiagnostics{};
	TerraEngine::Renderer::RenderFrameSnapshot latestRenderFrameSnapshot{};
	// Worker route preparation publishes only copied counters here. Protected
	// backend/publication stages consume a snapshot of these records, never a
	// live mesh or resource-manager object.
	std::vector<TerraEngine::Renderer::GeometryRouteFramePlan> latestGeometryRouteFramePlans;
	// Route plans depend only on the ordered selected-route sequence. Reusing an
	// exact signature avoids rebuilding identical tiny plans while the protected
	// snapshot still owns all live scene/resource capture.
	std::uint64_t latestGeometryRouteFramePlanSignature = 0u;
	// Render publication folds the route sequence while it already scans the
	// decisions for stats. The following scheduler workload can compare these two
	// scalar values before copying any per-instance route records.
	std::uint64_t latestGeometryRouteSelectionSignature = 0u;
	std::size_t latestGeometryRouteSelectionCount = 0u;
	std::vector<GpuVirtualGeometryPacketRecord> latestGameVirtualGeometryVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> latestSceneVirtualGeometryVisibilityRecords;
	std::optional<VirtualGeometryVisibilityPick> latestSceneAuthoritativeIdentityPick;
	std::optional<VirtualGeometryVisibilityPick> latestGameAuthoritativeIdentityPick;
	glm::uvec2 latestSceneAuthoritativeIdentityPixel = glm::uvec2(0u);
	glm::uvec2 latestGameAuthoritativeIdentityPixel = glm::uvec2(0u);
	// At most one identity copy per view is tracked as pending. A later request
	// replaces the earlier one so automation cannot accidentally read stale GPU
	// proof after changing its requested pixel.
	std::optional<glm::uvec2> pendingSceneAuthoritativeIdentityPixel;
	std::optional<glm::uvec2> pendingGameAuthoritativeIdentityPixel;
	std::string latestSceneAuthoritativeIdentityFailureReason;
	std::string latestGameAuthoritativeIdentityFailureReason;
	mutable std::mutex virtualGeometryValidationMutex;
	std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
		virtualGeometryValidationRouteOverride;
	bool virtualGeometryHybridValidationSceneRequested = false;
	TerraEngine::Renderer::RenderCaptureLimits captureLimits{};
	TerraEngine::Renderer::RenderCaptureQueueState captureQueueState{};
	// LOD transition history is private renderer state keyed by view/object/mesh.
	// Workers only receive immutable snapshots of the resulting state, so Scene
	// View, Game View, and future render targets cannot share mutable decisions.
	std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult> virtualGeometryTransitionCache;
	// Protected frame capture advances hysteresis only from prior immutable route
	// decisions. Workers receive the resulting state but never mutate this map.
	std::unordered_map<std::uint64_t, TerraEngine::Renderer::GeometryRoutePolicyState> geometryRoutePolicyStates;
	// Each rendered target owns its visibility history policy. Compatible views
	// may reuse immutable geometry/residency, but never share a mutable Hi-Z
	// history unless the policy explicitly assigns the same owner.
	std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryViewPolicyState> virtualGeometryViewPolicies;
	// Pending replay counters remain unavailable until RHI publishes a completed
	// counter-copy receipt. Keeping them on RenderSystem makes the protected frame
	// boundary, rather than a worker, the only publication owner.
	TerraEngine::Renderer::VirtualGeometryReplayReadbackQueue virtualGeometryReplayReadbacks;
	std::vector<TerraEngine::Renderer::VirtualGeometryReplayArtifact> latestVirtualGeometryReplayArtifacts;
	struct CompatibleAnimationPoseCacheRecord
	{
		TerraEngine::Renderer::AnimationRenderBinding binding{};
		std::uint64_t lastTouchedFrameId = 0U;
	};
	// One prior complete pose per render object supports an explicit hold-last
	// request without retaining unbounded publication generations.
	std::unordered_map<TerraEngine::UUID, CompatibleAnimationPoseCacheRecord> compatibleAnimationPoseCache;
	struct RenderableMembershipCacheRecord
	{
		// These values are authored membership/identity facts. Per-frame poses,
		// resource pointers, transforms, and visibility remain freshly resolved.
		Object object;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		MeshRenderComponent mesh{};
		std::optional<Object> nearestAnimatorObject;
		TerraEngine::UUID animatorObjectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID skeletonOverrideAssetId = TerraEngine::EMPTY_UUID;
		std::uint64_t animatorObjectLifetime = 0U;
		std::uint32_t animatorComponentGeneration = 0U;
	};
	// Static render membership is reused until a scene, component, or hierarchy
	// publication changes. This removes repeated hierarchy walks for large prefab
	// crowds without caching dynamic animation publications or resource residency.
	std::vector<RenderableMembershipCacheRecord> renderableMembershipCache;
	std::uint64_t renderableMembershipSceneGeneration = 0U;
	std::uint64_t renderableMembershipComponentRevision = 0U;
	std::uint64_t renderableMembershipHierarchyRevision = 0U;
	// Visualizer modes are view-local runtime preferences. Project settings keep
	// only the default, while each editor view can request a different mode for
	// the next prepared frame without mutating global quality state.
	std::unordered_map<std::uint64_t, VirtualGeometryDebugVisualizationMode> virtualGeometryDebugModes;
	std::deque<TerraEngine::Renderer::RenderCaptureRequest> pendingCaptureRequests;
	std::deque<TerraEngine::Renderer::RenderCaptureRequest> submittedCaptureReadbackRequests;
	std::deque<TerraEngine::Renderer::RenderCaptureResult> completedCaptureResults;
	// The cache contains copied transforms and is touched only by RenderSystem's
	// protected frame work, so render preparation never aliases live components.
	TerraEngine::Renderer::RenderWorldTransformCache worldTransformCache;
	TerraEngine::Renderer::RenderTransformCacheDiagnostics latestTransformCacheDiagnostics{};
	SkyRuntimeState latestSkyRuntimeState{};
	bool skyStateDirty = true;
	bool hasCachedSkyObjectResolution = false;
	std::optional<entt::entity> cachedSkyObject;
	std::uint64_t cachedSkyHierarchyRevision = 0U;
	std::uint64_t cachedSkyCandidateSignature = 0U;
	std::string lastLoggedSkyIssueKey;
	std::uint64_t nextRenderFrameId = 1u;
	std::uint64_t activeRenderFrameId = 0u;
	std::uint64_t nextCaptureRequestSequence = 1u;

	struct RuntimeRenderSettingsRequest
	{
		RenderSettings settings{};
		QualityPreset preset = QualityPreset::Custom;
		bool usePreset = false;
	};
	// Script workers only publish copied value records into this one-slot queue.
	// RenderSystem consumes the newest request at its protected frame boundary,
	// so no managed callback races currentSettings or backend resource changes.
	mutable std::mutex runtimeRenderSettingsMutex;
	std::optional<RuntimeRenderSettingsRequest> pendingRuntimeRenderSettings;
	std::uint64_t runtimeRenderSettingsGeneration = 0U;
	QualityPreset currentPreset = QualityPreset::High;
	RenderSettings currentSettings;
};
