// VulkanRendererWorkCache.h - cached renderer preparation work for Vulkan.
//
// This layer stores only immutable plan data and stable identifiers. It does
// not own VkDevice, VkDescriptorSet, VkCommandBuffer, or fence handles, so the
// cache entries cannot accidentally inherit Vulkan object lifetime rules.

#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanPipelineStateKeys.h"
#include "Renderer/RHI/Vulkan/VulkanTransitionPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"
#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct CachedDescriptorBindingPlan
	{
		DescriptorBindingPlan plan;
		std::uint64_t signature = 0u;
		bool cacheHit = false;
	};

	struct CachedPipelineStateKeyPlan
	{
		std::vector<PipelineStateKey> keys;
		std::uint64_t signature = 0u;
		bool cacheHit = false;
	};

	struct CachedUploadPreparationPlan
	{
		UploadPreparationPlan plan;
		std::uint64_t signature = 0u;
		bool cacheHit = false;
		bool queuedForUploadWorker = false;
	};

	struct PersistentRenderPacketCacheStats
	{
		std::size_t packetCount = 0u;
		std::size_t cacheHits = 0u;
		std::size_t cacheMisses = 0u;
	};

	struct VisibilityPreparationPlan
	{
		std::size_t renderableCount = 0u;
		std::size_t lightCount = 0u;
		std::size_t chunkCount = 0u;
		bool workerEligible = false;
	};

	struct IndirectDrawPreparationPlan
	{
		std::size_t drawCount = 0u;
		std::size_t instancePayloadCount = 0u;
		std::size_t virtualGeometryClusterPacketCount = 0u;
		std::size_t virtualGeometryBatchCount = 0u;
		std::size_t virtualGeometryCompactionResultCount = 0u;
		std::size_t virtualGeometryGpuPacketRecordCount = 0u;
		std::size_t virtualGeometryGpuBatchDescriptorCount = 0u;
		std::size_t virtualGeometryGpuPacketFrameSetCount = 0u;
		std::size_t virtualGeometryGpuCompactionDispatchCount = 0u;
		std::size_t virtualGeometryGeneratedSubmissionCount = 0u;
		std::size_t virtualGeometryCounterReadbackCount = 0u;
		std::uint64_t virtualGeometryBatchPacketSignature = 0u;
		bool gpuDrivenEligible = false;
		bool gpuPacketBuffersEligible = false;
		bool gpuCompactionEligible = false;
		bool generatedSubmissionEligible = false;
		bool counterReadbackEligible = false;
	};

	// This value-only plan keeps command-buffer capacity decisions testable without
	// constructing a Vulkan device. Command offsets are counted in indirect-command
	// records so callers can convert them to bytes only at the Vulkan boundary.
	struct IndirectDrawBufferCapacityPlan
	{
		std::size_t commandOffset = 0u;
		std::size_t requiredCommandCount = 0u;
		std::size_t targetCommandCapacity = 0u;
		bool requiresGrowth = false;
		bool valid = false;
	};

	// The protected Vulkan owner validates this value-only plan before allocating
	// a frame-slot buffer. A rejected plan publishes no partial grouped range;
	// its callers retain the pre-existing per-model RenderItemRecord route.
	struct GroupedInstanceStreamPlan
	{
		std::uint64_t frameGeneration = 0u;
		std::uint64_t topologyGeneration = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::vector<SharedRenderGroup> groups;
		std::size_t instanceCount = 0u;
		std::size_t instanceBytes = 0u;
		std::size_t avoidedSubmissionCount = 0u;
		RenderGroupRejectionReason rejectionReason = RenderGroupRejectionReason::None;
		bool valid = false;
		bool requiresCompleteFallback = false;
	};

	struct ShadowRecordingPreparationPlan
	{
		std::size_t shadowLightCount = 0u;
		std::size_t secondaryRecordingCount = 0u;
		bool workerEligible = false;
	};

	struct ShaderMaterialPreparationPlan
	{
		std::size_t materialCount = 0u;
		std::size_t shaderVariantCount = 0u;
		bool workerEligible = false;
	};

	struct UiGeometryPreparationPlan
	{
		std::size_t viewCount = 0u;
		std::size_t estimatedVertexCount = 0u;
		bool workerEligible = false;
	};

	struct FrameResourceRetirement
	{
		std::uint64_t resourceId = 0u;
		std::uint64_t retireAfterFrameId = 0u;
		std::string debugName;
		bool cancelled = false;
	};

	struct ThreadDiagnosticRecord
	{
		std::uint32_t workerId = 0u;
		std::string message;
	};

	struct VulkanRendererPreparationSummary
	{
		std::uint64_t frameId = 0u;
		std::size_t cacheLookupCount = 0u;
		std::size_t cacheHitCount = 0u;
		std::vector<CachedDescriptorBindingPlan> descriptorPlans;
		CachedPipelineStateKeyPlan pipelineKeys;
		CachedUploadPreparationPlan uploadPlan;
		std::vector<ResourceTransitionPlan> transitionPlans;
		PersistentRenderPacketCacheStats renderPacketCache;
		VisibilityPreparationPlan visibilityPlan;
		IndirectDrawPreparationPlan indirectDrawPlan;
		ShadowRecordingPreparationPlan shadowPlan;
		ShaderMaterialPreparationPlan shaderMaterialPlan;
		UiGeometryPreparationPlan uiPlan;
		std::vector<FrameResourceRetirement> retiredResources;
		std::vector<FrameResourceRetirement> waitingRetirements;
		std::vector<ThreadDiagnosticRecord> diagnostics;
	};

	class TERRAENGINE_API VulkanRendererPreparationCache
	{
	public:
		CachedDescriptorBindingPlan GetDescriptorPlan(
			const RenderFramePublication& publication, std::uint32_t viewId);
		CachedPipelineStateKeyPlan GetPipelineStateKeys(
			const RenderFramePublication& publication);
		CachedUploadPreparationPlan GetUploadPlan(
			const RenderFramePublication& publication);
		PersistentRenderPacketCacheStats RefreshPersistentRenderPackets(
			const RenderFramePublication& publication);
		void EnqueueResourceForRetirement(FrameResourceRetirement retirement);
		std::vector<FrameResourceRetirement> RetireCompletedResources(std::uint64_t completedFrameId);
		std::size_t PendingRetirementCount() const;
		void PushThreadDiagnostic(std::uint32_t workerId, std::string message);
		std::vector<ThreadDiagnosticRecord> DrainThreadDiagnostics();
		VulkanRendererPreparationSummary BuildFramePreparation(
			const RenderFramePublication& publication,
			const std::vector<RenderPassNode>& passes,
			const BackendCapabilityProfile& capabilities,
			bool collectPersistentPacketDiagnostics = true);

	private:
		void PrepareForBackendGeneration(const RenderFramePublication& publication);
		void TrimCachesIfNeeded();

		std::unordered_map<std::uint64_t, DescriptorBindingPlan> descriptorPlanCache;
		std::unordered_map<std::uint64_t, std::vector<PipelineStateKey>> pipelineKeyCache;
		std::unordered_map<std::uint64_t, UploadPreparationPlan> uploadPlanCache;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> persistentRenderPacketSignatures;
		std::deque<UploadPreparationPlan> pendingUploadPlans;
		std::deque<FrameResourceRetirement> pendingRetirements;
		std::vector<ThreadDiagnosticRecord> threadDiagnostics;
		std::uint64_t observedRendererSessionGeneration = 0u;
		std::uint64_t observedBackendResourceGeneration = 0u;
	};

	TERRAENGINE_API VisibilityPreparationPlan BuildVisibilityPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API IndirectDrawPreparationPlan BuildIndirectDrawPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API IndirectDrawBufferCapacityPlan PlanIndirectDrawBufferCapacity(
		std::size_t currentCommandCapacity,
		std::size_t usedCommandCount,
		std::size_t requestedCommandCount);
	TERRAENGINE_API GroupedInstanceStreamPlan BuildGroupedInstanceStreamPlan(
		const DynamicRenderFrameState& dynamicState,
		std::size_t maximumInstanceCapacity,
		std::uint64_t currentRendererGeneration);
	// The renderer already owns the dynamic instance and group arrays. This view
	// overload validates them in place so a large crowd is not copied into a second
	// temporary DynamicRenderFrameState merely to inspect counts and ranges.
	TERRAENGINE_API GroupedInstanceStreamPlan BuildGroupedInstanceStreamPlan(
		std::span<const RenderInstanceRecord> instances,
		std::span<const SharedRenderGroup> groups,
		std::uint64_t frameGeneration,
		std::uint64_t rendererGeneration,
		bool valid,
		std::size_t maximumInstanceCapacity,
		std::uint64_t currentRendererGeneration);
	TERRAENGINE_API ShadowRecordingPreparationPlan BuildShadowRecordingPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API ShaderMaterialPreparationPlan BuildShaderMaterialPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API UiGeometryPreparationPlan BuildUiGeometryPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities);

}
