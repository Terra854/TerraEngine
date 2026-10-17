// Typed virtual selection inputs, packet helpers and bounded persistent-history evidence.
#pragma once
#include "RenderFrameCaches.h"
#include "RenderFrameAlgorithms.h"
#include "RenderSnapshotPreparation.h"

namespace TerraEngine::Renderer::Frame {
	inline constexpr float kVirtualGeometryClusterRefinementErrorPixels = 1.5f;
	struct VirtualGeometryClusterMeshletRange
	{
		std::uint32_t meshletOffset = 0u;
		std::uint32_t meshletCount = 0u;
		bool valid = false;
	};
	struct VirtualGeometryTransitionMetrics
	{
		float candidateMaxErrorPixels = 0.0f;
		float transitionMarginPixels = std::numeric_limits<float>::max();
		std::uint32_t transitionCandidateCount = 0u;
	};
	bool GeometryPageCoversLod(const GeometryPageRuntime& page, std::uint32_t lodIndex);
	bool GeometryPageIsResident(const GeometryPageRuntime& page);
	const GeometryPageRuntime* FindGeometryPageForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex);
	const GeometryPageRuntime* FindGeometryPageById(const VirtualGeometryAssetRuntime& asset, std::uint32_t pageId);
	const VirtualGeometryClusterRuntime* FindVirtualGeometryClusterById(const VirtualGeometryAssetRuntime& asset, std::uint32_t clusterId);
	bool VirtualGeometryClusterPageIsResident(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryClusterRuntime& cluster);
	VirtualGeometryPagePayloadSource ResolveVirtualGeometryPayloadSource(const GeometryPageRuntime* page, bool fallbackPacket);
	VirtualGeometryEmittedPageSource ResolveVirtualGeometryEmittedSource(VirtualGeometryPagePayloadSource source);
	std::uint64_t ResolveVirtualGeometryPayloadBytes(const GeometryPageRuntime* page, const VirtualGeometryClusterMeshletRange& meshlets);
	std::pair<glm::vec3, glm::vec3> ResolveVirtualGeometryHiZPacketBounds(const VirtualGeometryClusterRuntime& cluster);
	void HashVirtualGeometryPageSourceKey(std::uint64_t& key, std::uint64_t value);
	void PopulateVirtualGeometryPageSourceRecords(TerraEngine::Renderer::VirtualGeometryViewSelectionState& state, std::uint64_t frameId);
	void PopulateVirtualGeometryPageRequestRecords(TerraEngine::Renderer::VirtualGeometryViewSelectionState& state);
	VirtualGeometryClusterMeshletRange ResolveVirtualGeometryClusterMeshletRange(
		const VirtualGeometryAssetRuntime& asset,
		const VirtualGeometryClusterRuntime& cluster);
	std::size_t DetailIndexForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex);
	std::size_t ResolveResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::size_t desiredIndex,
		std::string& outFallbackReason);
	std::size_t ResolveBestResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::string& outFallbackReason);
	float EstimateProjectedMeshRadiusPixels(const Mesh& mesh, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize);
	float EstimateProjectedClusterRadiusPixels(const VirtualGeometryClusterRuntime& cluster, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, float fallbackProjectedRadiusPixels);
	std::uint64_t BuildVirtualGeometryTransitionKey(
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId,
		const glm::uvec2& targetSize,
		const TerraEngine::UUID& objectId,
		const TerraEngine::UUID& meshId);
	std::uint64_t BuildVirtualGeometryDebugModeKey(RenderViewType viewType, std::uint32_t outputId);
	std::uint64_t BuildVirtualGeometrySelectionSignature(const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult& selection);
	VirtualGeometryTransitionMetrics EvaluateVirtualGeometryTransitionMetrics(
		const VirtualGeometryAssetRuntime& asset,
		const VirtualGeometryQualityProfile& quality,
		float projectedRadiusPixels,
		const glm::mat4& modelMatrix,
		const glm::vec3& cameraPosition,
		const glm::uvec2& targetSize);
	std::uint64_t BuildVirtualGeometryTransitionStateSignature(
		const std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult>& transitionCache,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId);
	void PruneStaleVirtualGeometryTransitionCache(
		std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult>& transitionCache,
		const std::unordered_set<TerraEngine::UUID>& aliveObjectIds);
}

// Only virtual-geometry operations consume this view; raw mesh pointers are
// backed by RenderSystem's retained immutable owners until protected retirement.
struct RenderSystem::FrameVirtualGeometryInputs {
	TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot;
	const std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources;
	const std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources;
	const FrameResourceCohort& resourceCohort;
	TerraEngine::Renderer::Frame::FrameGeometryInputs& geometryInputs;
	const FrameBackendCapture& backendCapture;
	std::uint64_t capturedVirtualGeometryResidencyGeneration;
	TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics;
	const std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof>& fixtureTargets;
};
// Selection history keys and diagnostics are owned once per actual coroutine;
// they contain no live ECS pointer, arena borrower or duplicate crowd snapshot.
struct RenderSystem::FrameVirtualGeometryPreparation {
	std::vector<VirtualGeometryAssetRuntime> gameVirtualGeometryDiagnosticsAssets;
	std::vector<VirtualGeometryAssetRuntime> sceneVirtualGeometryDiagnosticsAssets;
	std::vector<VirtualGeometryAssetRuntime> shadowVirtualGeometryDiagnosticsAssets;
	std::unordered_set<std::uint64_t> activeVirtualGeometryViewPolicyKeys;
	std::unordered_set<std::uint64_t> activeVirtualGeometryTransitionKeys;
	std::unordered_set<std::uint64_t> activeVirtualGeometryDebugModeKeys;
	std::atomic<bool> fixtureShadowSelectionApplied{false};
};

// Synchronous packet raster preparation needs only captured material/cohort/capability facts.
struct RenderSystem::FrameVirtualRasterInputs {
	const std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources;
	const FrameResourceCohort& resourceCohort;
	const FrameBackendCapture& backendCapture;
};
