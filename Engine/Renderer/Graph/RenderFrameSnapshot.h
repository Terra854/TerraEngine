// RenderFrameSnapshot.h - immutable renderer-facing frame data.
//
// Worker render jobs must never read live scene, editor, or resource-manager
// containers. This snapshot is the stable hand-off object: the main thread
// gathers the frame inputs once, then workers consume these value records.

#pragma once

#include "Renderer/Deformation/GpuDeformationPlan.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/VirtualGeometry/Selection/GeometryRoutePolicy.h"
#include "Utilities/UUID.h"
#include "Core/Export.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	enum class RenderResourceReadinessState
	{
		// Ready resources can be consumed by workers immediately. Other states
		// force the graph to pick a fallback, delay work, or reject the frame
		// before backend submission tries to use an unsafe resource.
		Ready,
		Pending,
		Fallback,
		Failed,
		Retired
	};

	struct RenderFrameViewRequest
	{
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		glm::uvec2 targetSize = glm::uvec2(0u);
		std::uint32_t outputId = 0u;
		// View generation invalidates visibility only. It is deliberately absent
		// from animation deformation records so moving one camera cannot rebuild a
		// pose that every Scene, Game, shadow, reflection, and capture view shares.
		std::uint64_t viewGeneration = 0u;
	};

	enum class AnimationRenderEvaluationSource : std::uint8_t
	{
		None = 0U,
		CpuPose = 1U,
		GpuVisual = 2U,
		HybridAuthoritative = 3U,
		SafeFallback = 4U
	};

	// One immutable deformation generation is referenced by every view that draws
	// the object. Offsets are renderer-neutral packet indices, never Vulkan handles
	// or CPU pointers, so worker plans can retain this record safely.
	struct AnimationRenderGenerationRecord
	{
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t poseGeneration = 0U;
		std::uint64_t morphGeneration = 0U;
		std::uint64_t paletteGeneration = 0U;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint64_t programGeneration = 0U;
		std::uint64_t animationPlanGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		// CPU-authoritative clones with byte-identical palettes and morph weights
		// may share one local-space deformation result. Both halves must be present;
		// GPU packets and unverified paths leave them zero and remain instance-local.
		std::uint64_t deformationContentIdLow = 0U;
		std::uint64_t deformationContentIdHigh = 0U;
		std::uint32_t firstGpuWorkPacket = 0U;
		std::uint32_t gpuWorkPacketCount = 0U;
		std::uint32_t gpuStageMask = 0U;
		AnimationRenderEvaluationSource source = AnimationRenderEvaluationSource::None;
		bool complete = false;
		bool safeFallback = false;
	};

	struct VirtualGeometryDrawRange
	{
		std::uint32_t selectedLodIndex = 0u;
		std::uint32_t meshletOffset = 0u;
		std::uint32_t meshletCount = 0u;
		bool virtualGeometryActive = false;
		bool failOpen = false;
		std::string fallbackReason;
		bool hiZSelfOcclusionGuardPassed = false;
		bool hiZNearPlaneGuardPassed = false;
		bool hiZThinGeometryGuardPassed = false;
		std::string hiZInactiveReason;
		std::uint32_t selectedClusterOffset = 0u;
		std::uint32_t selectedClusterCount = 0u;
		std::uint32_t pageNeedOffset = 0u;
		std::uint32_t pageNeedCount = 0u;
		// Page identity is part of the draw range because visible-command reuse
		// must invalidate when the same meshlet span starts reading a different
		// resident page, fallback page, or page-local byte slice.
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackPageId = 0xffffffffu;
		VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
		VirtualGeometryEmittedPageSource emittedSource = VirtualGeometryEmittedPageSource::None;
		std::uint64_t pagePayloadByteOffset = 0u;
		std::uint64_t pagePayloadByteSize = 0u;
		std::uint32_t payloadBufferIndex = 0xffffffffu;
		std::uint32_t payloadDescriptorIndex = 0xffffffffu;
	};

	struct VirtualGeometrySelectedClusterRecord
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint32_t clusterId = 0u;
		std::uint32_t lodIndex = 0u;
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackClusterId = 0xffffffffu;
		bool usingFallback = false;
	};

	struct VirtualGeometryPageNeedRecord
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackPageId = 0xffffffffu;
		std::uint32_t clusterId = 0u;
		std::uint32_t desiredLodIndex = 0u;
		float screenImportance = 0.0f;
		bool fallbackRequired = false;
		VirtualGeometryPagePayloadSource desiredPayloadSource = VirtualGeometryPagePayloadSource::Unknown;
		VirtualGeometryPagePayloadSource fallbackPayloadSource = VirtualGeometryPagePayloadSource::Unknown;
		std::uint32_t packetId = 0u;
		std::uint32_t batchId = 0u;
		std::uint32_t packetOffset = 0u;
		std::uint32_t packetCount = 0u;
	};

	struct VirtualGeometryPageRequestRecord
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackPageId = 0xffffffffu;
		std::uint32_t clusterId = 0u;
		std::uint32_t desiredLodIndex = 0u;
		std::int32_t priority = 0;
		float screenImportance = 0.0f;
		bool fallbackRequired = false;
		bool fallbackRequest = false;
		VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
		VirtualGeometryEmittedPageSource expectedEmission = VirtualGeometryEmittedPageSource::None;
		std::string reason;
	};

	struct VirtualGeometryResidentPageSetRecord
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint64_t residentBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		std::uint32_t residentPageCount = 0u;
		std::uint32_t pinnedFallbackPageCount = 0u;
		std::vector<std::uint32_t> residentPageIds;
		std::vector<std::uint32_t> fallbackPageIds;
	};

	struct VirtualGeometryClusterSelectionResult
	{
		// range stores fail-open guards and whole-object fallback data. Normal
		// virtual geometry draws are expanded from selectedClusters so the GPU
		// dispatches the non-overlapping sparse cluster cut directly.
		VirtualGeometryDrawRange range;
		std::vector<VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<VirtualGeometryPageNeedRecord> pageNeeds;
		VirtualGeometryLODTransitionState transitionState;
		std::uint32_t invalidSelectionReasons = 0u;
	};

	struct GeometryRouteSelectionRecord
	{
		// A renderable needs only the resolved route and the generations that prove
		// it belongs to this object/mesh. The complete decision contains candidate
		// vectors and diagnostic strings; retaining those for every instance made a
		// 4,000-instance crowd copy the same mesh-level explanation thousands of
		// times even when no diagnostics consumer was active.
		std::uint64_t decisionIdentity = 0u;
		std::uint64_t objectIdentity = 0u;
		std::uint64_t meshIdentity = 0u;
		std::uint64_t assetGeneration = 0u;
		GeometryRoute selected = GeometryRoute::DirectMeshlet;
		std::uint64_t policyIdentity = 0u;
		std::uint64_t calibrationIdentity = 0u;
		GeometryRouteSelectionSource source = GeometryRouteSelectionSource::Automatic;
		std::uint32_t stableFrameCount = 0u;
		GeometryRouteFallbackReason fallbackReason = GeometryRouteFallbackReason::None;
		std::uint64_t routeGeneration = 0u;
	};

	struct RenderFrameRenderableRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		glm::mat4 modelMatrix = glm::mat4(1.0f);
		VirtualGeometryDrawRange virtualGeometry;
		bool taskShaderCullingEnabled = true;
		bool usingFallback = false;
		AnimationRenderGenerationRecord animation;
		RenderInvalidationGenerationRecord invalidation;
		// Route input and the resulting decision are copied with the renderable so
		// worker stages never re-query a live mesh, material, or device object.
		GeometryRouteInputSummary geometryRouteInput;
		GeometryRouteSelectionRecord geometryRouteDecision;
	};

	struct VirtualGeometryViewDrawRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		VirtualGeometryDrawRange range;
		bool taskShaderCullingEnabled = true;
	};

	struct VirtualGeometryViewSelectionState
	{
		// One state belongs to one camera-like producer. Scene, Game, shadow,
		// and exported-runtime views may see the same object with different LOD,
		// culling, and Hi-Z decisions, so workers read these copied ranges
		// instead of sharing one mutable renderable-side answer.
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		glm::uvec2 targetSize = glm::uvec2(0u);
		std::uint32_t outputId = 0u;
		// This copied token invalidates only the camera-owned visibility result.
		// Deformation generations remain shared by every view of the renderable.
		std::uint64_t viewGeneration = 0U;
		VirtualGeometryDebugVisualizationMode debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
		bool shadowPass = false;
		bool exportRuntime = false;
		bool hiZOcclusionAllowed = true;
		bool hiZSelfOcclusionGuardPassed = true;
		bool hiZNearPlaneGuardPassed = true;
		bool hiZThinGeometryGuardPassed = true;
		std::string hiZInactiveReason;
		// Backends can publish the exact current-view Hi-Z resource state after
		// validating pyramid ownership and warm-up. When this is false, snapshot
		// builders synthesize a conservative default from target size and flags.
		bool hasHiZValidationState = false;
		VirtualGeometryHiZValidationState hiZValidationState;
		std::vector<VirtualGeometryViewDrawRecord> draws;
		std::vector<VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<GpuVirtualGeometryPacketRecord> gpuPacketRecords;
		std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatchDescriptors;
		std::vector<GpuVirtualGeometryPacketFrameSet> gpuPacketFrameSets;
		std::vector<GpuPacketCompactionDispatch> gpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> generatedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> gpuCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> gpuFallbackRecords;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryPageRequestRecord> pageRequests;
		std::vector<VirtualGeometryResidentPageSetRecord> residentPageSets;
		std::vector<VirtualGeometryPagePayloadRecord> residentPagePayloads;
		std::vector<VirtualGeometryEmittedPageSourceRecord> emittedPageSources;
		std::vector<VirtualGeometryPageRetirementRecord> pageRetirements;
		std::vector<PageResidencyRecord> pageResidency;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<HiZRegressionSnapshot> hiZRegressionSnapshots;
		std::vector<ConeSafetyMetadata> coneSafetyMetadata;
		std::vector<ShadowVisibilitySelection> shadowVisibilitySelections;
		std::vector<PerformanceGateResult> performanceGates;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
	};

	struct VirtualGeometryViewVisibilitySnapshot
	{
		// Hardening diagnostics publish one immutable record per rendered view.
		// The record contains only copied IDs, counters, and state labels so
		// tooling can inspect it without touching live Vulkan or scene objects.
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		std::uint64_t frameId = 0u;
		glm::uvec2 renderSize = glm::uvec2(0u);
		std::uint32_t outputId = 0u;
		std::uint64_t cameraSignature = 0u;
		VirtualGeometryDebugVisualizationMode debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
		VirtualGeometryCounterSnapshot counters;
		VirtualGeometryHiZValidationState hiZValidationState;
		std::vector<VirtualGeometryViewDrawRecord> drawRanges;
		std::vector<VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<GpuVirtualGeometryPacketRecord> gpuPacketRecords;
		std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatchDescriptors;
		std::vector<GpuVirtualGeometryPacketFrameSet> gpuPacketFrameSets;
		std::vector<GpuPacketCompactionDispatch> gpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> generatedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> gpuCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> gpuFallbackRecords;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryPageRequestRecord> pageRequests;
		std::vector<VirtualGeometryResidentPageSetRecord> residentPageSets;
		std::vector<VirtualGeometryPagePayloadRecord> residentPagePayloads;
		std::vector<VirtualGeometryEmittedPageSourceRecord> emittedPageSources;
		std::vector<VirtualGeometryPageRetirementRecord> pageRetirements;
		std::vector<PageResidencyRecord> pageResidency;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<HiZRegressionSnapshot> hiZRegressionSnapshots;
		std::vector<ConeSafetyMetadata> coneSafetyMetadata;
		std::vector<ShadowVisibilitySelection> shadowVisibilitySelections;
		std::vector<PerformanceGateResult> performanceGates;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
		std::vector<OcclusionDebugSample> occlusionDebugSamples;
		VirtualGeometryCounterSource source = VirtualGeometryCounterSource::Prepared;
		bool valid = false;
	};

	struct RenderFrameLightRecord
	{
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		std::uint32_t lightType = 0u;
		bool castsShadows = false;
		bool usingFallback = false;
	};

	struct RenderFrameResourceRecord
	{
		TerraEngine::UUID resourceId = TerraEngine::EMPTY_UUID;
		std::string debugName;
		bool resident = true;
		bool usingFallback = false;
		bool requiredForFrame = false;
		RenderResourceReadinessState readiness = RenderResourceReadinessState::Ready;
		std::string fallbackReason;
	};

	struct RenderFrameSnapshot
	{
		// RenderSystem builds this value object on the main thread. Worker jobs
		// must read only these copied records, never live scene containers or
		// editor-owned objects that can mutate during the frame.
		std::uint64_t frameId = 0u;
		std::vector<RenderFrameViewRequest> viewRequests;
		std::vector<RenderFrameRenderableRecord> renderables;
		std::vector<RenderFrameLightRecord> lights;
		// Resource tables contain one record per UUID. Per-instance mesh/material
		// identities remain on renderables; duplicating resource rows makes descriptor,
		// upload, sorting, and hashing work scale with instance count unnecessarily.
		std::vector<RenderFrameResourceRecord> materials;
		std::vector<RenderFrameResourceRecord> meshes;
		std::vector<RenderFrameResourceRecord> textures;
		std::vector<VirtualGeometryViewSelectionState> virtualGeometryViewSelections;
		std::vector<VirtualGeometryViewVisibilitySnapshot> virtualGeometryVisibilitySnapshots;
		// Each renderable owns the compact route selection used by normal rendering.
		// Full decisions below retain candidate lists and explanation strings only
		// while detailed diagnostics are requested, so ordinary crowd frames do not
		// duplicate heavyweight diagnostic storage for every instance.
		std::vector<GeometryRouteDecision> geometryRouteDecisions;
		std::vector<DirectMeshletBatch> directMeshletBatches;
		// Every view contributes page demands here. Worker preparation sorts and
		// deduplicates them into one renderer-owned deformation plan.
		std::vector<DeformationPageDemand> animatedGeometryPageDemands;
		RenderSettings renderSettings;
		std::uint64_t resourceRevision = 0u;
		std::uint64_t sceneRevision = 0u;
		bool valid = true;
		// This flag is a defensive tripwire for tests and diagnostics. If a
		// future collector accidentally hands workers live mutable state, graph
		// validation can reject the packet instead of racing the scene.
		bool containsLiveMutableState = false;
	};

	// RenderSystem publishes the prior frame's complete animated-object visibility
	// union after every camera and shadow list has been prepared. AnimationSystem
	// consumes this immutable value on the following frame so visual-only animators
	// can sleep without introducing a Render -> Animation dependency cycle.
	struct AnimatedObjectVisibilitySnapshot
	{
		std::uint64_t frameId = 0U;
		std::uint64_t sceneGeneration = 0U;
		std::vector<TerraEngine::UUID> visibleObjectIds;
		// A compact open-addressed table stores indices into visibleObjectIds. The
		// renderer builds it once when publishing; thousands of animation workers
		// then perform exact expected-O(1) membership tests instead of repeating a
		// binary search through the same immutable vector for every animator.
		std::vector<std::uint32_t> visibleObjectLookupIndices;
		std::uint32_t visibleObjectLookupMask = 0U;
		bool hasActiveView = false;
		bool valid = false;
		// RenderSystem can publish directly from its canonical renderable-index mask.
		// Standalone producers leave this false and receive defensive sort/unique
		// normalization in PublishAnimatedObjectVisibilitySnapshot.
		bool canonicalUnique = false;

		// BuildVisibleObjectLookup is called only by the protected publisher after
		// canonicalization. Allocation failure leaves the sorted-vector fallback
		// intact, so visibility remains fail-open and deterministic.
		void BuildVisibleObjectLookup() noexcept;
		[[nodiscard]] bool Contains(const TerraEngine::UUID& objectId) const noexcept;
	};

	// Publication swaps one complete shared snapshot atomically. Readers never
	// retain renderer-owned containers or observe a partially assembled union.
	TERRAENGINE_API void PublishAnimatedObjectVisibilitySnapshot(
		AnimatedObjectVisibilitySnapshot snapshot);
	[[nodiscard]] TERRAENGINE_API std::shared_ptr<const AnimatedObjectVisibilitySnapshot>
		LoadAnimatedObjectVisibilitySnapshot() noexcept;
	TERRAENGINE_API void ClearAnimatedObjectVisibilitySnapshot() noexcept;

	// Builder functions keep construction deterministic. Tests and future
	// backends can create snapshots without needing access to RenderSystem.
	// Deterministic sorting and hashing make worker completion order irrelevant
	// to the final renderer output.
	TERRAENGINE_API void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotContainsLiveMutableState(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool ValidateAnimationRenderGenerationRecord(const AnimationRenderGenerationRecord& record);
	// Page-local compute descriptors currently bind one GPU animation palette
	// arena. CPU-authoritative publications must stay on the owner deformation
	// path instead of being assigned an unrelated GPU work-packet index.
	TERRAENGINE_API bool CanScheduleGpuAnimatedPageDeformation(
		const AnimationRenderGenerationRecord& record);
	TERRAENGINE_API std::uint64_t BuildAnimationRenderGenerationHash(const AnimationRenderGenerationRecord& record);
	TERRAENGINE_API std::uint64_t BuildRenderInvalidationGenerationHash(const RenderInvalidationGenerationRecord& record);
	TERRAENGINE_API std::uint64_t BuildSnapshotDeterministicHash(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot);
	// Resolves the union of physical animated pages selected by all prepared
	// views. The returned demands preserve immutable generation identities while
	// narrowing requestedViewMask to the views that can actually draw each page.
	TERRAENGINE_API std::vector<DeformationPageDemand>
		SelectAnimatedGeometryPageDemands(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryViewSelectionState BuildVirtualGeometryViewSelectionState(const RenderFrameViewRequest& view, const std::vector<RenderFrameRenderableRecord>& renderables);
	TERRAENGINE_API std::vector<VirtualGeometryViewSelectionState> BuildVirtualGeometryViewSelectionStates(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryCounterSnapshot BuildVirtualGeometryCounterSnapshot(const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source);
	TERRAENGINE_API VirtualGeometryHiZValidationState BuildDefaultVirtualGeometryHiZValidationState(const VirtualGeometryViewSelectionState& state, std::uint64_t cameraSignature);
	TERRAENGINE_API VirtualGeometryViewVisibilitySnapshot BuildVirtualGeometryViewVisibilitySnapshot(const RenderFrameSnapshot& snapshot, const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source);
	TERRAENGINE_API std::vector<VirtualGeometryViewVisibilitySnapshot> BuildVirtualGeometryViewVisibilitySnapshots(const RenderFrameSnapshot& snapshot);

}
