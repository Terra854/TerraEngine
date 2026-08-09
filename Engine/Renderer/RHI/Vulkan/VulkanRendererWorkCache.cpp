#include "VulkanRendererWorkCache.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

namespace TerraEngine::Renderer::Vulkan {
namespace {
	constexpr std::size_t kCacheEntryLimit = 64u;
	constexpr std::size_t kParallelPreparationThreshold = 64u;

	void HashCombine(std::uint64_t& seed, std::uint64_t value)
	{
		// FNV-style mixing gives stable, cheap keys for cache invalidation. These
		// signatures are not security hashes; they only decide whether immutable
		// render-preparation records can be reused.
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
	}

	std::uint64_t HashUuid(const TerraEngine::UUID& id)
	{
		return static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(id));
	}

	std::uint64_t HashDescriptorSignature(
		const RenderFramePublication& publication, std::uint32_t viewId)
	{
		std::uint64_t signature = publication.resourcePreparation.generation;
		HashCombine(signature, publication.rendererSessionGeneration);
		HashCombine(signature, publication.backendResourceGeneration);
		HashCombine(signature, viewId);
		for(const RenderResourcePreparationRecord& resource :
			publication.resourcePreparation.records)
		{
			HashCombine(signature, HashUuid(resource.resourceId));
			HashCombine(signature, resource.resourceGeneration);
			HashCombine(signature, resource.resident ? 1u : 0u);
		}
		return signature;
	}

	std::uint64_t HashPipelineSignature(const RenderFramePublication& publication)
	{
		std::uint64_t signature = publication.resourcePreparation.generation;
		HashCombine(signature, publication.rendererSessionGeneration);
		HashCombine(signature, publication.backendResourceGeneration);
		if(publication.persistent != nullptr)
		{
			for(const std::shared_ptr<const RenderPersistentPage>& page :
				publication.persistent->pages)
			{
				if(page == nullptr)
				{
					continue;
				}
				for(const RenderPersistentSlot& slot : page->slots)
				{
					HashCombine(signature, HashUuid(slot.meshId));
					HashCombine(signature, HashUuid(slot.materialId));
					HashCombine(signature, slot.packetTemplateIdentity);
				}
			}
		}
		return signature;
	}

	std::uint64_t HashUploadSignature(const RenderFramePublication& publication)
	{
		std::uint64_t signature = publication.resourcePreparation.generation;
		HashCombine(signature, publication.rendererSessionGeneration);
		HashCombine(signature, publication.backendResourceGeneration);
		for(const RenderResourcePreparationRecord& resource :
			publication.resourcePreparation.records)
		{
			HashCombine(signature, HashUuid(resource.resourceId));
			HashCombine(signature, resource.resourceGeneration);
			HashCombine(signature, resource.resident ? 1u : 0u);
		}
		return signature;
	}

	std::uint64_t HashPersistentPacketSignature(const RenderFramePublication& publication,
		const RenderPersistentSlot& slot)
	{
		std::uint64_t signature = HashUuid(slot.meshId);
		HashCombine(signature, publication.rendererSessionGeneration);
		HashCombine(signature, publication.backendResourceGeneration);
		HashCombine(signature, HashUuid(slot.materialId));
		HashCombine(signature, slot.packetTemplateIdentity);
		HashCombine(signature, slot.routeTemplate);
		return signature;
	}
}

	CachedDescriptorBindingPlan VulkanRendererPreparationCache::GetDescriptorPlan(
		const RenderFramePublication& publication, std::uint32_t viewId)
	{
		CachedDescriptorBindingPlan result;
		result.signature = HashDescriptorSignature(publication, viewId);
		if (const auto existing = descriptorPlanCache.find(result.signature); existing != descriptorPlanCache.end())
		{
			result.plan = existing->second;
			result.cacheHit = true;
			return result;
		}

		result.plan = BuildDescriptorBindingPlan(publication, viewId);
		descriptorPlanCache[result.signature] = result.plan;
		TrimCachesIfNeeded();
		return result;
	}

	CachedPipelineStateKeyPlan VulkanRendererPreparationCache::GetPipelineStateKeys(
		const RenderFramePublication& publication)
	{
		CachedPipelineStateKeyPlan result;
		result.signature = HashPipelineSignature(publication);
		if (const auto existing = pipelineKeyCache.find(result.signature); existing != pipelineKeyCache.end())
		{
			result.keys = existing->second;
			result.cacheHit = true;
			return result;
		}

		result.keys = BuildPipelineStateKeys(publication);
		pipelineKeyCache[result.signature] = result.keys;
		TrimCachesIfNeeded();
		return result;
	}

	CachedUploadPreparationPlan VulkanRendererPreparationCache::GetUploadPlan(
		const RenderFramePublication& publication)
	{
		CachedUploadPreparationPlan result;
		result.signature = HashUploadSignature(publication);
		if (const auto existing = uploadPlanCache.find(result.signature); existing != uploadPlanCache.end())
		{
			result.plan = existing->second;
			result.cacheHit = true;
		}
		else
		{
			result.plan = BuildUploadPreparationPlan(publication);
			uploadPlanCache[result.signature] = result.plan;
			TrimCachesIfNeeded();
		}

		if (!result.plan.payloads.empty())
		{
			// The queue stores immutable upload packets only. A backend worker can
			// later record staging copies from these packets while queue submit
			// remains coordinator-owned.
			pendingUploadPlans.push_back(result.plan);
			result.queuedForUploadWorker = true;
		}
		return result;
	}

	PersistentRenderPacketCacheStats VulkanRendererPreparationCache::RefreshPersistentRenderPackets(
		const RenderFramePublication& publication)
	{
		PersistentRenderPacketCacheStats stats;
		std::unordered_set<TerraEngine::UUID> aliveObjects;
		if(publication.persistent == nullptr)
		{
			return stats;
		}
		for(const std::shared_ptr<const RenderPersistentPage>& page :
			publication.persistent->pages)
		{
			if(page == nullptr)
			{
				continue;
			}
			for(const RenderPersistentSlot& slot : page->slots)
			{
				if(!slot.IsComplete())
				{
					continue;
				}
				++stats.packetCount;
				aliveObjects.insert(slot.objectId);
				const std::uint64_t signature = HashPersistentPacketSignature(publication, slot);
				const auto existing = persistentRenderPacketSignatures.find(slot.objectId);
				if(existing != persistentRenderPacketSignatures.end()
					&& existing->second == signature)
				{
					++stats.cacheHits;
				}
				else
				{
					++stats.cacheMisses;
					persistentRenderPacketSignatures[slot.objectId] = signature;
				}
			}
		}

		for (auto iterator = persistentRenderPacketSignatures.begin(); iterator != persistentRenderPacketSignatures.end();)
		{
			iterator = aliveObjects.contains(iterator->first) ? std::next(iterator) : persistentRenderPacketSignatures.erase(iterator);
		}
		return stats;
	}

	void VulkanRendererPreparationCache::EnqueueResourceForRetirement(FrameResourceRetirement retirement)
	{
		pendingRetirements.push_back(std::move(retirement));
	}

	std::vector<FrameResourceRetirement> VulkanRendererPreparationCache::RetireCompletedResources(std::uint64_t completedFrameId)
	{
		std::vector<FrameResourceRetirement> retired;
		while (!pendingRetirements.empty() && pendingRetirements.front().retireAfterFrameId <= completedFrameId)
		{
			if (!pendingRetirements.front().cancelled)
			{
				retired.push_back(std::move(pendingRetirements.front()));
			}
			pendingRetirements.pop_front();
		}
		return retired;
	}

	std::size_t VulkanRendererPreparationCache::PendingRetirementCount() const
	{
		return pendingRetirements.size();
	}

	void VulkanRendererPreparationCache::PushThreadDiagnostic(std::uint32_t workerId, std::string message)
	{
		threadDiagnostics.push_back({ workerId, std::move(message) });
	}

	std::vector<ThreadDiagnosticRecord> VulkanRendererPreparationCache::DrainThreadDiagnostics()
	{
		std::vector<ThreadDiagnosticRecord> records = std::move(threadDiagnostics);
		threadDiagnostics.clear();
		return records;
	}

	VulkanRendererPreparationSummary VulkanRendererPreparationCache::BuildFramePreparation(
		const RenderFramePublication& publication,
		const std::vector<RenderPassNode>& passes,
		const BackendCapabilityProfile& capabilities,
		bool collectPersistentPacketDiagnostics)
	{
		PrepareForBackendGeneration(publication);
		VulkanRendererPreparationSummary summary;
		summary.frameId = publication.frameId;
		for(const RenderViewSelection& view : publication.views)
		{
			CachedDescriptorBindingPlan descriptorPlan = GetDescriptorPlan(
				publication, view.outputId);
			summary.cacheLookupCount += 1u;
			summary.cacheHitCount += descriptorPlan.cacheHit ? 1u : 0u;
			summary.descriptorPlans.push_back(std::move(descriptorPlan));
		}

		summary.pipelineKeys = GetPipelineStateKeys(publication);
		summary.cacheLookupCount += 1u;
		summary.cacheHitCount += summary.pipelineKeys.cacheHit ? 1u : 0u;

		summary.uploadPlan = GetUploadPlan(publication);
		summary.cacheLookupCount += 1u;
		summary.cacheHitCount += summary.uploadPlan.cacheHit ? 1u : 0u;

		summary.transitionPlans.reserve(passes.size());
		for (const RenderPassNode& pass : passes)
		{
			summary.transitionPlans.push_back(BuildResourceTransitionPlan(
				publication.frameId, pass));
		}

		if(collectPersistentPacketDiagnostics)
		{
			summary.renderPacketCache = RefreshPersistentRenderPackets(publication);
		}
		else
		{
			// Packet-cache scans exist only to report hit/miss diagnostics. Preserve
			// the useful population count while keeping the compact production path
			// free of an otherwise redundant O(instance-count) hash/map pass.
			if(publication.persistent != nullptr)
			{
				for(const std::shared_ptr<const RenderPersistentPage>& page :
					publication.persistent->pages)
				{
					summary.renderPacketCache.packetCount += page != nullptr
						? page->slots.size() : 0u;
				}
			}
		}
		for(std::size_t viewIndex = 0u; viewIndex < publication.views.size(); ++viewIndex)
		{
			const RenderViewSelection& view = publication.views[viewIndex];
			summary.visibilityPlan.renderableCount += view.VisibleOrdinals().size();
			summary.indirectDrawPlan.drawCount += view.VisibleOrdinals().size();
			summary.indirectDrawPlan.instancePayloadCount += view.VisibleOrdinals().size();
			if(viewIndex >= publication.presentationInputs.size())
			{
				continue;
			}
			const RenderViewPresentationInput& input = publication.presentationInputs[viewIndex];
			summary.visibilityPlan.lightCount += input.lightCandidates.size();
			summary.shadowPlan.shadowLightCount += input.shadowDrawLists.size();
			summary.uiPlan.viewCount += 1u;
			summary.indirectDrawPlan.virtualGeometryGpuPacketRecordCount +=
				input.VirtualGeometryPackets().size();
			summary.indirectDrawPlan.virtualGeometryGpuBatchDescriptorCount +=
				input.VirtualGeometryBatches().size();
			summary.indirectDrawPlan.virtualGeometryGpuPacketFrameSetCount +=
				input.VirtualGeometryFrameSets().size();
			summary.indirectDrawPlan.virtualGeometryGpuCompactionDispatchCount +=
				input.VirtualGeometryCompactionDispatches().size();
			summary.indirectDrawPlan.virtualGeometryGeneratedSubmissionCount +=
				input.VirtualGeometryGeneratedSubmissions().size();
			summary.indirectDrawPlan.virtualGeometryCounterReadbackCount +=
				input.VirtualGeometryCounterReadbacks().size();
		}
		const std::size_t workload = summary.visibilityPlan.renderableCount
			+ summary.visibilityPlan.lightCount;
		summary.visibilityPlan.workerEligible = capabilities.supportsParallelCulling
			&& workload >= kParallelPreparationThreshold;
		summary.visibilityPlan.chunkCount = summary.visibilityPlan.workerEligible
			? std::min<std::size_t>(std::max<std::uint32_t>(1u,
				capabilities.maxRecordingWorkers), workload)
			: 1u;
		summary.indirectDrawPlan.gpuDrivenEligible =
			capabilities.supportsParallelRecording
			&& summary.indirectDrawPlan.drawCount != 0u;
		summary.indirectDrawPlan.gpuPacketBuffersEligible =
			capabilities.supportsVirtualGeometryGpuPacketBuffers
			&& summary.indirectDrawPlan.virtualGeometryGpuPacketRecordCount != 0u;
		summary.indirectDrawPlan.gpuCompactionEligible =
			capabilities.supportsVirtualGeometryGpuCompaction
			&& summary.indirectDrawPlan.gpuPacketBuffersEligible;
		summary.indirectDrawPlan.generatedSubmissionEligible =
			capabilities.supportsVirtualGeometryGeneratedSubmissions
			&& summary.indirectDrawPlan.gpuCompactionEligible;
		summary.indirectDrawPlan.counterReadbackEligible =
			capabilities.supportsVirtualGeometryCounterReadback
			&& summary.indirectDrawPlan.virtualGeometryGpuPacketRecordCount != 0u;
		summary.shadowPlan.workerEligible = capabilities.supportsParallelRecording
			&& summary.shadowPlan.shadowLightCount != 0u;
		summary.shadowPlan.secondaryRecordingCount = summary.shadowPlan.workerEligible
			? summary.shadowPlan.shadowLightCount : 0u;
		summary.shaderMaterialPlan.materialCount = summary.pipelineKeys.keys.size();
		summary.shaderMaterialPlan.shaderVariantCount = summary.pipelineKeys.keys.size();
		summary.shaderMaterialPlan.workerEligible = capabilities.supportsParallelRecording
			&& summary.shaderMaterialPlan.materialCount != 0u;
		summary.uiPlan.workerEligible = capabilities.supportsParallelRecording
			&& summary.uiPlan.viewCount != 0u;
		summary.retiredResources = RetireCompletedResources(
			publication.frameId > 2u ? publication.frameId - 2u : 0u);
		summary.waitingRetirements.assign(pendingRetirements.begin(), pendingRetirements.end());
		summary.diagnostics = DrainThreadDiagnostics();
		return summary;
	}

	void VulkanRendererPreparationCache::PrepareForBackendGeneration(
		const RenderFramePublication& publication)
	{
		if (observedRendererSessionGeneration == publication.rendererSessionGeneration
			&& observedBackendResourceGeneration == publication.backendResourceGeneration) {
			return;
		}
		// These plans contain only values, but pipeline keys and upload/retirement
		// decisions are meaningful only for one backend object generation. Drop all
		// retained plans before preparing against a newly recovered backend.
		descriptorPlanCache.clear();
		pipelineKeyCache.clear();
		uploadPlanCache.clear();
		persistentRenderPacketSignatures.clear();
		pendingUploadPlans.clear();
		pendingRetirements.clear();
		observedRendererSessionGeneration = publication.rendererSessionGeneration;
		observedBackendResourceGeneration = publication.backendResourceGeneration;
	}

	void VulkanRendererPreparationCache::TrimCachesIfNeeded()
	{
		// This is intentionally simple because the frame path owns the cache on
		// the coordinator thread. If it grows past the cap, old unordered entries
		// are discarded; the next frame can rebuild them from immutable snapshots.
		while (descriptorPlanCache.size() > kCacheEntryLimit)
		{
			descriptorPlanCache.erase(descriptorPlanCache.begin());
		}
		while (pipelineKeyCache.size() > kCacheEntryLimit)
		{
			pipelineKeyCache.erase(pipelineKeyCache.begin());
		}
		while (uploadPlanCache.size() > kCacheEntryLimit)
		{
			uploadPlanCache.erase(uploadPlanCache.begin());
		}
		while (pendingUploadPlans.size() > kCacheEntryLimit)
		{
			pendingUploadPlans.pop_front();
		}
	}

	VisibilityPreparationPlan BuildVisibilityPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities)
	{
		VisibilityPreparationPlan plan;
		// Each selected ordinal resolves through the sealed topology/dynamic roots.
		// The publication deliberately owns no copied renderable table, so this count
		// preserves the per-view workload without reopening the worker snapshot.
		for (const RenderViewSelection& view : publication.views)
		{
			plan.renderableCount += view.VisibleOrdinals().size();
		}
		for (const RenderViewPresentationInput& input : publication.presentationInputs)
		{
			plan.lightCount += input.lightCandidates.size();
		}
		const std::size_t workload = plan.renderableCount + plan.lightCount;
		plan.workerEligible = capabilities.supportsParallelCulling && workload >= kParallelPreparationThreshold;
		plan.chunkCount = plan.workerEligible ? std::min<std::size_t>(std::max<std::uint32_t>(1u, capabilities.maxRecordingWorkers), workload) : 1u;
		return plan;
	}

	IndirectDrawPreparationPlan BuildIndirectDrawPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities)
	{
		IndirectDrawPreparationPlan plan;
		std::uint64_t signature = publication.frameId ^ publication.resourcePreparation.generation;
		for (const RenderViewSelection& view : publication.views)
		{
			plan.drawCount += view.VisibleOrdinals().size();
			plan.instancePayloadCount += view.VisibleOrdinals().size();
			HashCombine(signature, view.generation);
			for (const RenderSlotGenerationKey& key : view.VisibleOrdinals())
			{
				HashCombine(signature, key.ordinal);
				HashCombine(signature, key.lifetimeGeneration);
			}
		}
		for (const RenderViewPresentationInput& input : publication.presentationInputs)
		{
			const auto& packets = input.VirtualGeometryPackets();
			const auto& batches = input.VirtualGeometryBatches();
			plan.virtualGeometryClusterPacketCount += packets.size();
			plan.virtualGeometryBatchCount += batches.size();
			plan.virtualGeometryGpuPacketRecordCount += packets.size();
			plan.virtualGeometryGpuBatchDescriptorCount += batches.size();
			plan.virtualGeometryGpuPacketFrameSetCount += input.VirtualGeometryFrameSets().size();
			plan.virtualGeometryGpuCompactionDispatchCount += input.VirtualGeometryCompactionDispatches().size();
			plan.virtualGeometryGeneratedSubmissionCount += input.VirtualGeometryGeneratedSubmissions().size();
			plan.virtualGeometryCounterReadbackCount += input.VirtualGeometryCounterReadbacks().size();
			// Packet evidence is intentionally part of the compact publication. Hash
			// the immutable records rather than trusting a producer-provided summary
			// token so a changed debug identity or buffer binding invalidates Vulkan
			// preparation deterministically.
			for (const GpuVirtualGeometryPacketRecord& packet : packets)
			{
				HashCombine(signature, packet.packetId);
				HashCombine(signature, packet.debugIdentity);
				HashCombine(signature, packet.flags);
				HashCombine(signature, packet.payloadBufferIndex);
				HashCombine(signature, packet.payloadDescriptorIndex);
			}
			for (const GpuVirtualGeometryBatchDescriptor& batch : batches)
			{
				HashCombine(signature, batch.batchId);
				HashCombine(signature, static_cast<std::uint64_t>(batch.debugMode));
				HashCombine(signature, batch.flags);
			}
			HashCombine(signature, input.virtualGeometryPacketEvidenceSignature);
			HashCombine(signature, input.virtualGeometryVisibilitySetupSignature);
		}
		plan.virtualGeometryBatchPacketSignature = signature;
		plan.gpuDrivenEligible = capabilities.supportsParallelRecording && plan.drawCount != 0u;
		plan.gpuPacketBuffersEligible = capabilities.supportsVirtualGeometryGpuPacketBuffers
			&& plan.virtualGeometryClusterPacketCount > 0u;
		plan.gpuCompactionEligible = capabilities.supportsVirtualGeometryGpuCompaction
			&& plan.gpuPacketBuffersEligible;
		plan.generatedSubmissionEligible = capabilities.supportsVirtualGeometryGeneratedSubmissions
			&& plan.gpuCompactionEligible;
		plan.counterReadbackEligible = capabilities.supportsVirtualGeometryCounterReadback
			&& (plan.virtualGeometryGpuPacketRecordCount > 0u || plan.virtualGeometryClusterPacketCount > 0u);
		return plan;
	}

	IndirectDrawBufferCapacityPlan PlanIndirectDrawBufferCapacity(
		std::size_t currentCommandCapacity,
		std::size_t usedCommandCount,
		std::size_t requestedCommandCount)
	{
		IndirectDrawBufferCapacityPlan plan{};
		// An overflowing command count would wrap push-constant offsets and make
		// two views address the same indirect record. Reject it before choosing a
		// new allocation so the caller cannot publish a partial command stream.
		if (usedCommandCount > std::numeric_limits<std::size_t>::max()
			- requestedCommandCount)
		{
			return plan;
		}

		plan.commandOffset = usedCommandCount;
		plan.requiredCommandCount = usedCommandCount + requestedCommandCount;
		plan.targetCommandCapacity = currentCommandCapacity;
		if (plan.targetCommandCapacity < plan.requiredCommandCount)
		{
			// Geometric growth amortizes large crowds while retaining the exact
			// requested floor. This is an initial allocation policy, not a draw cap.
			plan.targetCommandCapacity = std::max<std::size_t>(
				currentCommandCapacity, 1024u);
			while (plan.targetCommandCapacity < plan.requiredCommandCount)
			{
				if (plan.targetCommandCapacity
					> std::numeric_limits<std::size_t>::max() / 2u)
				{
					plan.targetCommandCapacity = plan.requiredCommandCount;
					break;
				}
				plan.targetCommandCapacity *= 2u;
			}
			plan.requiresGrowth = true;
		}

		plan.valid = true;
		return plan;
	}

	GroupedInstanceStreamPlan BuildGroupedInstanceStreamPlan(
		const DynamicRenderFrameState& dynamicState,
		std::size_t maximumInstanceCapacity,
		std::uint64_t currentRendererGeneration)
	{
		return BuildGroupedInstanceStreamPlan(
			dynamicState.instances,
			dynamicState.groups,
			dynamicState.frameGeneration,
			dynamicState.rendererGeneration,
			dynamicState.valid,
			maximumInstanceCapacity,
			currentRendererGeneration);
	}

	GroupedInstanceStreamPlan BuildGroupedInstanceStreamPlan(
		std::span<const RenderInstanceRecord> instances,
		std::span<const SharedRenderGroup> groups,
		std::uint64_t frameGeneration,
		std::uint64_t rendererGeneration,
		bool valid,
		std::size_t maximumInstanceCapacity,
		std::uint64_t currentRendererGeneration)
	{
		GroupedInstanceStreamPlan plan{};
		plan.frameGeneration = frameGeneration;
		plan.rendererGeneration = rendererGeneration;
		if (!valid || frameGeneration == 0u
			|| rendererGeneration == 0u
			|| rendererGeneration != currentRendererGeneration)
		{
			plan.rejectionReason = RenderGroupRejectionReason::StaleGeneration;
			plan.requiresCompleteFallback = true;
			return plan;
		}
		if (instances.size() > maximumInstanceCapacity)
		{
			plan.rejectionReason = RenderGroupRejectionReason::InstanceStreamAllocation;
			plan.requiresCompleteFallback = true;
			return plan;
		}

		for (const SharedRenderGroup& group : groups)
		{
			const std::size_t first = group.firstInstance;
			const std::size_t count = group.instanceCount;
			if (!group.executable || !group.key.IsComplete()
				|| group.key.rendererGeneration != currentRendererGeneration
				|| count < 2u || first > instances.size()
				|| count > instances.size() - first)
			{
				plan.rejectionReason = !group.key.IsComplete()
					? RenderGroupRejectionReason::IncompleteKey
					: RenderGroupRejectionReason::InstanceRange;
				plan.requiresCompleteFallback = true;
				return plan;
			}
			plan.topologyGeneration = group.topologyGeneration;
			plan.groups.push_back(group);
			plan.avoidedSubmissionCount += count - 1u;
		}
		if (plan.groups.empty())
		{
			plan.rejectionReason = RenderGroupRejectionReason::Singleton;
			plan.requiresCompleteFallback = true;
			return plan;
		}
		plan.instanceCount = instances.size();
		plan.instanceBytes = plan.instanceCount * sizeof(RenderInstanceRecord);
		plan.valid = true;
		return plan;
	}

	ShadowRecordingPreparationPlan BuildShadowRecordingPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities)
	{
		ShadowRecordingPreparationPlan plan;
		for (const RenderViewPresentationInput& input : publication.presentationInputs)
		{
			for (const ShadowDrawList& shadow : input.shadowDrawLists)
			{
				// One shadow list is already split by target face/cascade. Its item
				// count is the sealed secondary-recording workload Vulkan must own.
				++plan.shadowLightCount;
				plan.secondaryRecordingCount += std::max<std::size_t>(1u, shadow.stableOrdinals.size());
			}
		}
		plan.workerEligible = capabilities.supportsParallelShadowRecording && capabilities.supportsSecondaryRecording && plan.secondaryRecordingCount > 1u;
		return plan;
	}

	ShaderMaterialPreparationPlan BuildShaderMaterialPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities)
	{
		ShaderMaterialPreparationPlan plan;
		std::unordered_set<TerraEngine::UUID> materials;
		if (publication.persistent != nullptr)
		{
			for (const std::shared_ptr<const RenderPersistentPage>& page : publication.persistent->pages)
			{
				if (page == nullptr)
				{
					continue;
				}
				for (const RenderPersistentSlot& slot : page->slots)
				{
					if (slot.IsComplete())
					{
						materials.insert(slot.materialId);
					}
				}
			}
		}
		plan.materialCount = materials.size();
		plan.shaderVariantCount = plan.materialCount;
		plan.workerEligible = capabilities.supportsParallelPipelineStatePreparation && plan.materialCount >= kParallelPreparationThreshold;
		return plan;
	}

	UiGeometryPreparationPlan BuildUiGeometryPreparationPlan(
		const RenderFramePublication& publication,
		const BackendCapabilityProfile& capabilities)
	{
		UiGeometryPreparationPlan plan;
		for (const RenderViewPresentationInput& input : publication.presentationInputs)
		{
			if (input.output.diagnosticsView == RenderViewType::Game
				|| input.output.diagnosticsView == RenderViewType::Scene)
			{
				++plan.viewCount;
				plan.estimatedVertexCount += 6u;
			}
		}
		plan.workerEligible = capabilities.supportsParallelUiPreparation && plan.estimatedVertexCount >= kParallelPreparationThreshold;
		return plan;
	}

}
