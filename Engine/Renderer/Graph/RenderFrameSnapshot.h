// RenderFrameSnapshot.h - immutable worker frame aggregate.

#pragma once

#include "Renderer/VirtualGeometry/Selection/RenderViewSelectionData.h"

namespace TerraEngine::Renderer
{
	struct RenderFrameSnapshot
	{
		std::uint64_t frameId = 0u;
		std::vector<RenderFrameViewRequest> viewRequests;
		std::vector<RenderFrameRenderableRecord> renderables;
		std::vector<ConventionalVisibilityIdentityRecord> conventionalVisibilityIdentityTable;
		std::vector<RenderFrameLightRecord> lights;
		std::vector<RenderFrameResourceRecord> materials;
		std::vector<RenderFrameResourceRecord> meshes;
		std::vector<RenderFrameResourceRecord> textures;
		std::vector<VirtualGeometryViewSelectionState> virtualGeometryViewSelections;
		std::vector<VirtualGeometryViewVisibilitySnapshot> virtualGeometryVisibilitySnapshots;
		std::vector<GeometryRouteDecision> geometryRouteDecisions;
		std::vector<DirectMeshletBatch> directMeshletBatches;
		std::vector<DeformationPageDemand> animatedGeometryPageDemands;
		std::size_t animatedGeometryPageDemandCount = 0u;
		RenderSettings renderSettings;
		std::uint64_t resourceRevision = 0u;
		std::uint64_t sceneRevision = 0u;
		bool valid = true;
		bool containsLiveMutableState = false;
	};

	TERRAENGINE_API void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API bool SnapshotContainsLiveMutableState(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API SharedRenderGroupKey BuildSharedRenderGroupKey(const RenderFrameRenderableRecord& renderable, const RenderFrameViewRequest& view) noexcept;
	TERRAENGINE_API PersistentRenderTopology BuildPersistentRenderTopology(const RenderFrameSnapshot& snapshot, std::uint64_t topologyGeneration);
	TERRAENGINE_API DynamicRenderFrameState BuildDynamicRenderFrameState(const RenderFrameSnapshot& snapshot, const PersistentRenderTopology& topology, const RenderFrameViewRequest& view, std::uint64_t frameGeneration);
	TERRAENGINE_API bool ValidateAnimationRenderGenerationRecord(const AnimationRenderGenerationRecord& record);
	TERRAENGINE_API bool CanScheduleGpuAnimatedPageDeformation(const AnimationRenderGenerationRecord& record);
	TERRAENGINE_API std::uint64_t BuildAnimationRenderGenerationHash(const AnimationRenderGenerationRecord& record);
	TERRAENGINE_API std::uint64_t BuildRenderInvalidationGenerationHash(const RenderInvalidationGenerationRecord& record);
	TERRAENGINE_API std::uint64_t BuildSnapshotDeterministicHash(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API std::vector<DeformationPageDemand> SelectAnimatedGeometryPageDemands(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryViewSelectionState BuildVirtualGeometryViewSelectionState(const RenderFrameViewRequest& view, const std::vector<RenderFrameRenderableRecord>& renderables);
	TERRAENGINE_API std::vector<VirtualGeometryViewSelectionState> BuildVirtualGeometryViewSelectionStates(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryCounterSnapshot BuildVirtualGeometryCounterSnapshot(const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source);
	TERRAENGINE_API VirtualGeometryHiZValidationState BuildDefaultVirtualGeometryHiZValidationState(const VirtualGeometryViewSelectionState& state, std::uint64_t cameraSignature);
	TERRAENGINE_API VirtualGeometryViewVisibilitySnapshot BuildVirtualGeometryViewVisibilitySnapshot(const RenderFrameSnapshot& snapshot, const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source);
	TERRAENGINE_API std::vector<VirtualGeometryViewVisibilitySnapshot> BuildVirtualGeometryViewVisibilitySnapshots(const RenderFrameSnapshot& snapshot);
}
