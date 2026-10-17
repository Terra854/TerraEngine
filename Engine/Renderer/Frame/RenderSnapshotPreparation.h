// Owned unique-mesh derived facts; source payloads remain backed by captured resource owners.
#pragma once
#include "Renderer/RenderTypes.h"
#include "RenderFrameCapture.h"
#include <unordered_map>
struct Mesh;
namespace TerraEngine::Renderer::Frame {
	struct CanonicalGeometryRouteFacts
	{
		bool complete = false;
		bool directUsable = false;
		bool virtualGeometryUsable = false;
		std::uint32_t representationFlags = 0u;
		std::uint32_t sourceTriangleCount = 0u;
		std::uint32_t packedMeshletCount = 0u;
		std::uint32_t physicalPageCount = 0u;
		std::uint32_t physicalBlockCount = 0u;
		std::uint64_t storedBytes = 0u;
		std::uint64_t decodedBytes = 0u;
		std::uint64_t calibrationIdentity = 0u;
	};

	bool FitsCanonicalDirectoryRange(
		const std::vector<std::uint8_t>& storage,
		std::uint64_t offset,
		std::uint32_t count,
		std::size_t elementSize) noexcept;
	CanonicalGeometryRouteFacts BuildCanonicalGeometryRouteFacts(const Mesh& mesh);
	VirtualGeometryAssetRuntime BuildVirtualGeometryRuntimeFromMesh(const TerraEngine::UUID& meshId, const Mesh& mesh);

	GeometryRouteDecision CopyRouteDecisionForFrame(const GeometryRouteDecision& source, bool retainDetailedRouteCandidates);

	struct GeometryRouteStatsAggregate
	{
		std::uint32_t directCount = 0U;
		std::uint32_t virtualCount = 0U;
		std::uint32_t sharedInstances = 0U;
		std::uint64_t policyIdentity = 0U;
		std::uint64_t calibrationIdentity = 0U;
		std::uint32_t hysteresisFrames = 0U;
		std::string fallbackReason;
		TerraEngine::Renderer::GeometryRouteSelectionSource source =
			TerraEngine::Renderer::GeometryRouteSelectionSource::Automatic;
	};

	void ReuseFrameRouteDiagnostics(const RenderFrameSnapshot& renderFrameSnapshot,
		GeometryRouteStatsAggregate& geometryRouteStats, std::uint64_t& geometryRouteSelectionSignature);
	void BuildFrameViewTransforms(RenderFrameSnapshot& renderFrameSnapshot,
		std::span<const RenderFrameViewRequest> previousViewRequests);
	void PrepareFrameDirectMeshletBatches(RenderFrameSnapshot& renderFrameSnapshot,
		const std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources,
		GeometryRouteStatsAggregate& geometryRouteStats, bool reuseRenderableRecords);

	class FrameGeometryInputs final {
	public:
		explicit FrameGeometryInputs(std::size_t uniqueResourceCount);
		std::size_t ExpectedUniqueMeshCount() const noexcept { return expectedUniqueMeshCount_; }
		const VirtualGeometryAssetRuntime& Runtime(const TerraEngine::UUID& meshId, const Mesh& mesh);
		const CanonicalGeometryRouteFacts& Facts(const TerraEngine::UUID& meshId, const Mesh& mesh);
	private:
		std::size_t expectedUniqueMeshCount_;
		std::unordered_map<TerraEngine::UUID, VirtualGeometryAssetRuntime> virtualGeometryRuntimeByMesh;
		std::unordered_map<TerraEngine::UUID, CanonicalGeometryRouteFacts> geometryRouteFactsByMesh;
	};
}

// This view is synchronous: resourceCohort retains shader owners and renderer
// retainedRuntimeMaterialOwners backs material pointers; lifetimes are copied values.
struct RenderSystem::FrameSnapshotShadowView {
	const FrameResourceCohort& resourceCohort;
	const std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources;
	std::span<const std::uint64_t> capturedShadowObjectLifetimes;
	std::uint64_t frameResourceRevision;
};

// Only snapshot-row selection consumes these capabilities and this exact strong plan alias.
struct RenderSystem::FrameSnapshotRouteView {
	std::uint64_t snapshotRendererGeneration;
	bool hardwareVirtualGeometryRouteAvailable, computeVirtualGeometryRouteAvailable;
	const std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>& validationRouteOverride;
	const std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan>& immutableGpuAnimationPlan;
};
