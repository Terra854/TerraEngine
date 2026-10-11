// RenderViewSelectionData.h - immutable renderer-facing value records.
//
// Worker render jobs and backend planners consume these immutable value records
// without reading live scene, editor, or resource-manager containers.

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

	// Page payloads are view-owned evidence. Passing this compact record keeps
	// upload planning independent of the snapshot that produced the selection.
	struct VirtualGeometryPagePayloadSelection
	{
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t outputId = 0u;
		std::vector<VirtualGeometryPagePayloadRecord> residentPayloads;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
	};

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
		// Reference-frame identity is copied with the request so per-view workers
		// can reject stale origins before producing culling or draw matrices.
		RenderReferenceFrame referenceFrame{};
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

		[[nodiscard]] bool IsComplete() const noexcept
		{
			if(source == AnimationRenderEvaluationSource::None)
			{
				return stableInstanceId == 0U && poseGeneration == 0U
					&& morphGeneration == 0U && paletteGeneration == 0U
					&& deformationContentIdLow == 0U
					&& deformationContentIdHigh == 0U
					&& gpuWorkPacketCount == 0U && !complete && !safeFallback;
			}
			if(stableInstanceId == 0U || deformablePayloadGeneration == 0U
				|| rendererGeneration == 0U
				|| ((deformationContentIdLow == 0U)
					!= (deformationContentIdHigh == 0U)))
			{
				return false;
			}
			if(source == AnimationRenderEvaluationSource::SafeFallback)
			{
				return safeFallback && !complete && gpuWorkPacketCount == 0U;
			}
			if(programGeneration == 0U || !complete || safeFallback
				|| poseGeneration == 0U || morphGeneration == 0U
				|| paletteGeneration == 0U)
			{
				return false;
			}
			if(source == AnimationRenderEvaluationSource::CpuPose)
			{
				return gpuWorkPacketCount == 0U && gpuStageMask == 0U;
			}
			return animationPlanGeneration != 0U && gpuWorkPacketCount != 0U
				&& gpuStageMask != 0U;
		}

		bool operator==(const AnimationRenderGenerationRecord&) const = default;
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
		// vectors and diagnostic strings; retaining those for every member of a large
		// repeated crowd copied the same mesh-level explanation many times even when
		// no diagnostics consumer was active.
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
		// Neutral/view-local staging only. Shared spatial identity is always the
		// canonical pose below; consumers must materialize this matrix per view.
		glm::mat4 modelMatrix = glm::mat4(1.0F);
		VirtualGeometryDrawRange virtualGeometry;
		bool taskShaderCullingEnabled = true;
		bool usingFallback = false;
		AnimationRenderGenerationRecord animation;
		RenderInvalidationGenerationRecord invalidation;
		// Route input and the resulting decision are copied with the renderable so
		// worker stages never re-query a live mesh, material, or device object.
		GeometryRouteInputSummary geometryRouteInput;
		GeometryRouteSelectionRecord geometryRouteDecision;
		// One-based, deterministic key used by conventional uint4 visibility
		// readback. It refers only to the immutable table below.
		std::uint32_t conventionalVisibilityIdentityIndex = 0u;
		// Worker preparation retains the complete immutable presentation binding.
		// Protected publication later strips its worker-only owners and duplicated
		// GPU identity, leaving one compact backend binding beside this generation
		// record for every object rather than one copy per output view.
		AnimationRenderBinding animationBinding;
		// This object-local generation advances only when its propagated world
		// transform changes. Dynamic publication can therefore copy one affected
		// page instead of treating the global frame/view generation as a transform.
		std::uint64_t transformGeneration = 0u;
		// Shared topology retains only the canonical pose. A model matrix does not
		// exist until one view-local reference frame materializes it.
		TerraEngine::Scene::WorldPose canonicalWorldPose{};
		// Protected capture seals resource and deformation proof once per object;
		// every camera and shadow command retains the same immutable owner.
		std::shared_ptr<const ShadowCasterStamp> shadowCasterInputs;
	};

	// A compact lookup table lets a conventional raster identity resolve direct
	// and virtual-geometry objects through the same immutable frame snapshot.
	// Packet metadata remains an optional refinement, never the source of object
	// ownership for a direct draw.
	struct ConventionalVisibilityIdentityRecord
	{
		std::uint32_t index = 0u;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	};

	struct VirtualGeometryViewDrawRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		VirtualGeometryDrawRange range;
		bool taskShaderCullingEnabled = true;
		// This zero-based index addresses the same immutable renderable table as the
		// draw command's one-based visibility identity. CPU consumers can therefore
		// join view results to animation records without hashing the object UUID.
		std::uint32_t renderableIndex = 0xffffffffu;
	};

	struct VirtualGeometryRetainedSelectionEvidence
	{
		std::vector<VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
		std::vector<VirtualGeometryPageRequestRecord> pageRequests;
		std::vector<VirtualGeometryPagePayloadRecord> residentPagePayloads;
		std::vector<VirtualGeometryEmittedPageSourceRecord> emittedPageSources;
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
		// A stable visible-command cache may retain immutable draw evidence across
		// frames. The accessor keeps ordinary standalone snapshots value-owned while
		// allowing live cache hits to avoid copying thousands of unchanged ranges.
		std::shared_ptr<const std::vector<VirtualGeometryViewDrawRecord>> retainedDraws;

		[[nodiscard]] const std::vector<VirtualGeometryViewDrawRecord>& DrawRecords()
			const noexcept
		{
			return retainedDraws ? *retainedDraws : draws;
		}
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
		std::shared_ptr<const VirtualGeometryRetainedGpuEvidence> retainedGpuEvidence;

		[[nodiscard]] const auto& GpuPacketRecords() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->packetRecords : gpuPacketRecords;
		}
		[[nodiscard]] const auto& GpuBatchDescriptors() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->batchDescriptors : gpuBatchDescriptors;
		}
		[[nodiscard]] const auto& GpuPacketFrameSets() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->packetFrameSets : gpuPacketFrameSets;
		}
		[[nodiscard]] const auto& GpuCompactionDispatches() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->compactionDispatches : gpuCompactionDispatches;
		}
		[[nodiscard]] const auto& GeneratedSubmissions() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->generatedSubmissions : generatedSubmissions;
		}
		[[nodiscard]] const auto& GpuCounterReadbacks() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->counterReadbacks : gpuCounterReadbacks;
		}
		[[nodiscard]] const auto& GpuFallbackRecords() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->fallbackRecords : gpuFallbackRecords;
		}
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
		// These records are immutable for a cache signature. A live cache hit shares
		// their retained owner; authored and standalone snapshots continue using the
		// value-owned vectors above and below.
		std::shared_ptr<const VirtualGeometryRetainedSelectionEvidence>
			retainedSelectionEvidence;

		[[nodiscard]] const std::vector<VirtualGeometrySelectedClusterRecord>&
			SelectedClusters() const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->selectedClusters : selectedClusters;
		}
		[[nodiscard]] const std::vector<VirtualGeometryPageNeedRecord>& PageNeeds()
			const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->pageNeeds : pageNeeds;
		}
		[[nodiscard]] const std::vector<VirtualGeometryClusterPacket>& ClusterPackets()
			const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->clusterPackets : clusterPackets;
		}
		[[nodiscard]] const std::vector<ClusterWorkBatch>& ClusterBatches()
			const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->clusterBatches : clusterBatches;
		}
		[[nodiscard]] const std::vector<CompactedVisibilitySet>& CompactionResults()
			const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->compactionResults : compactionResults;
		}
		[[nodiscard]] const std::vector<FallbackCoverageChain>& FallbackCoverage()
			const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->fallbackCoverage : fallbackCoverage;
		}
		[[nodiscard]] const std::vector<VirtualGeometryLODTransitionState>&
			LodTransitionStates() const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->lodTransitionStates : lodTransitionStates;
		}
		[[nodiscard]] const std::vector<VirtualGeometryPageRequestRecord>& PageRequests()
			const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->pageRequests : pageRequests;
		}
		[[nodiscard]] const std::vector<VirtualGeometryPagePayloadRecord>&
			ResidentPagePayloads() const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->residentPagePayloads : residentPagePayloads;
		}
		[[nodiscard]] const std::vector<VirtualGeometryEmittedPageSourceRecord>&
			EmittedPageSources() const noexcept
		{
			return retainedSelectionEvidence
				? retainedSelectionEvidence->emittedPageSources : emittedPageSources;
		}
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
		std::shared_ptr<const VirtualGeometryRetainedGpuEvidence> retainedGpuEvidence;
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

		[[nodiscard]] const auto& GpuPacketRecords() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->packetRecords : gpuPacketRecords;
		}
		[[nodiscard]] const auto& GpuBatchDescriptors() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->batchDescriptors : gpuBatchDescriptors;
		}
		[[nodiscard]] const auto& GpuPacketFrameSets() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->packetFrameSets : gpuPacketFrameSets;
		}
		[[nodiscard]] const auto& GpuCompactionDispatches() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->compactionDispatches : gpuCompactionDispatches;
		}
		[[nodiscard]] const auto& GeneratedSubmissions() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->generatedSubmissions : generatedSubmissions;
		}
		[[nodiscard]] const auto& GpuCounterReadbacks() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->counterReadbacks : gpuCounterReadbacks;
		}
		[[nodiscard]] const auto& GpuFallbackRecords() const noexcept
		{
			return retainedGpuEvidence ? retainedGpuEvidence->fallbackRecords : gpuFallbackRecords;
		}

		[[nodiscard]] const auto& SelectedClusters() const noexcept
		{
			return selectedClusters;
		}
		[[nodiscard]] const auto& PageNeeds() const noexcept
		{
			return pageNeeds;
		}
		[[nodiscard]] const auto& ClusterPackets() const noexcept
		{
			return clusterPackets;
		}
		[[nodiscard]] const auto& ClusterBatches() const noexcept
		{
			return clusterBatches;
		}
		[[nodiscard]] const auto& CompactionResults() const noexcept
		{
			return compactionResults;
		}
		[[nodiscard]] const auto& FallbackCoverage() const noexcept
		{
			return fallbackCoverage;
		}
		[[nodiscard]] const auto& LodTransitionStates() const noexcept
		{
			return lodTransitionStates;
		}
		[[nodiscard]] const auto& PageRequests() const noexcept
		{
			return pageRequests;
		}
		[[nodiscard]] const auto& ResidentPagePayloads() const noexcept
		{
			return residentPagePayloads;
		}
		[[nodiscard]] const auto& EmittedPageSources() const noexcept
		{
			return emittedPageSources;
		}
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

	// Snapshot aggregation and snapshot-only builders live in
	// RenderFrameSnapshot.h. Keep the reusable view records above independent.
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


}
