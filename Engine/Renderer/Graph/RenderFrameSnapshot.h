// RenderFrameSnapshot.h - immutable worker frame aggregate.

#pragma once

#include "Renderer/VirtualGeometry/Selection/RenderViewSelectionData.h"

namespace TerraEngine::Renderer
{
	struct RenderViewTransformRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint64_t viewId = 0U;
		std::uint64_t originGeneration = 0U;
		glm::mat4 currentMatrix{1.0F};
		glm::mat4 previousMatrix{1.0F};
		bool valid = false;
	};

	struct RenderLocalizedPoseMatrices
	{
		glm::mat4 currentMatrix{1.0F};
		glm::mat4 previousMatrix{1.0F};
		bool valid = false;
		bool staleReferenceFrame = false;
	};

	struct RenderFrameSnapshot
	{
		std::uint64_t frameId = 0u;
		// This signature covers only inputs that can change the persistent renderable
		// records below. View/camera generations are deliberately excluded. The two
		// renderer construction buffers retain their own signature, allowing a warm
		// frame to reuse vector storage and values without borrowing the published
		// buffer currently visible to diagnostics.
		std::uint64_t renderableSourceSignature = 0u;
		bool reusableRenderableRecords = false;
		std::vector<RenderFrameViewRequest> viewRequests;
		std::vector<RenderFrameRenderableRecord> renderables;
		std::vector<RenderViewTransformRecord> viewTransforms;
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
	TERRAENGINE_API RenderLocalizationResult BuildLocalizedRenderPose(
		const TerraEngine::Scene::WorldPose& pose,
		const RenderReferenceFrame& currentFrame,
		const RenderReferenceFrame* previousFrame = nullptr);
	TERRAENGINE_API RenderLocalizedPoseMatrices BuildLocalizedRenderPoseMatrices(
		const TerraEngine::Scene::WorldPose& pose,
		const RenderReferenceFrame& currentFrame,
		const RenderReferenceFrame* previousFrame = nullptr);
	TERRAENGINE_API bool IsRenderReferenceFrameCurrent(
		const RenderReferenceFrame& expected,
		const RenderReferenceFrame& actual) noexcept;
	TERRAENGINE_API std::vector<DeformationPageDemand> SelectAnimatedGeometryPageDemands(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryViewSelectionState BuildVirtualGeometryViewSelectionState(const RenderFrameViewRequest& view, const std::vector<RenderFrameRenderableRecord>& renderables);
	TERRAENGINE_API std::vector<VirtualGeometryViewSelectionState> BuildVirtualGeometryViewSelectionStates(const RenderFrameSnapshot& snapshot);
	TERRAENGINE_API VirtualGeometryCounterSnapshot BuildVirtualGeometryCounterSnapshot(const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source);
	TERRAENGINE_API VirtualGeometryHiZValidationState BuildDefaultVirtualGeometryHiZValidationState(const VirtualGeometryViewSelectionState& state, std::uint64_t cameraSignature);
	TERRAENGINE_API VirtualGeometryViewVisibilitySnapshot BuildVirtualGeometryViewVisibilitySnapshot(const RenderFrameSnapshot& snapshot, const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source);
	TERRAENGINE_API std::vector<VirtualGeometryViewVisibilitySnapshot> BuildVirtualGeometryViewVisibilitySnapshots(const RenderFrameSnapshot& snapshot);
}
