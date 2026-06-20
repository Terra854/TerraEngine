// VulkanRendererWorkCache.h - cached renderer preparation work for Vulkan.
//
// This layer stores only immutable plan data and stable identifiers. It does
// not own VkDevice, VkDescriptorSet, VkCommandBuffer, or fence handles, so the
// same scheduling ideas can be mirrored by a future DX12 backend without
// inheriting Vulkan object lifetime rules.

#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanPipelineStateKeys.h"
#include "Renderer/RHI/Vulkan/VulkanTransitionPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"
#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <deque>
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
		bool gpuDrivenEligible = false;
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
		std::vector<ThreadDiagnosticRecord> diagnostics;
	};

	class TERRAENGINE_API VulkanRendererPreparationCache
	{
	public:
		CachedDescriptorBindingPlan GetDescriptorPlan(const RenderFrameSnapshot& snapshot, std::uint32_t viewId);
		CachedPipelineStateKeyPlan GetPipelineStateKeys(const RenderFrameSnapshot& snapshot);
		CachedUploadPreparationPlan GetUploadPlan(const RenderFrameSnapshot& snapshot);
		PersistentRenderPacketCacheStats RefreshPersistentRenderPackets(const RenderFrameSnapshot& snapshot);
		void EnqueueResourceForRetirement(FrameResourceRetirement retirement);
		std::vector<FrameResourceRetirement> RetireCompletedResources(std::uint64_t completedFrameId);
		void PushThreadDiagnostic(std::uint32_t workerId, std::string message);
		std::vector<ThreadDiagnosticRecord> DrainThreadDiagnostics();
		VulkanRendererPreparationSummary BuildFramePreparation(
			const RenderFrameSnapshot& snapshot,
			const std::vector<RenderPassNode>& passes,
			const BackendCapabilityProfile& capabilities);

	private:
		void TrimCachesIfNeeded();

		std::unordered_map<std::uint64_t, DescriptorBindingPlan> descriptorPlanCache;
		std::unordered_map<std::uint64_t, std::vector<PipelineStateKey>> pipelineKeyCache;
		std::unordered_map<std::uint64_t, UploadPreparationPlan> uploadPlanCache;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> persistentRenderPacketSignatures;
		std::deque<UploadPreparationPlan> pendingUploadPlans;
		std::deque<FrameResourceRetirement> pendingRetirements;
		std::vector<ThreadDiagnosticRecord> threadDiagnostics;
	};

	TERRAENGINE_API VisibilityPreparationPlan BuildVisibilityPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API IndirectDrawPreparationPlan BuildIndirectDrawPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API ShadowRecordingPreparationPlan BuildShadowRecordingPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API ShaderMaterialPreparationPlan BuildShaderMaterialPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API UiGeometryPreparationPlan BuildUiGeometryPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities);

}
