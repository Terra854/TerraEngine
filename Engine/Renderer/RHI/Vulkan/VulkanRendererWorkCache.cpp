#include "VulkanRendererWorkCache.h"

#include <algorithm>
#include <functional>
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

	std::uint64_t HashResourceRecords(const std::vector<RenderFrameResourceRecord>& records)
	{
		std::uint64_t signature = 1469598103934665603ull;
		for (const RenderFrameResourceRecord& record : records)
		{
			HashCombine(signature, HashUuid(record.resourceId));
			HashCombine(signature, record.resident ? 1u : 0u);
			HashCombine(signature, record.usingFallback ? 1u : 0u);
		}
		return signature;
	}

	std::uint64_t HashVirtualGeometryRange(const VirtualGeometryDrawRange& range)
	{
		std::uint64_t signature = 1469598103934665603ull;
		HashCombine(signature, range.selectedLodIndex);
		HashCombine(signature, range.meshletOffset);
		HashCombine(signature, range.meshletCount);
		HashCombine(signature, range.virtualGeometryActive ? 1u : 0u);
		HashCombine(signature, range.failOpen ? 1u : 0u);
		HashCombine(signature, range.hiZSelfOcclusionGuardPassed ? 1u : 0u);
		HashCombine(signature, range.hiZNearPlaneGuardPassed ? 1u : 0u);
		HashCombine(signature, range.hiZThinGeometryGuardPassed ? 1u : 0u);
		HashCombine(signature, static_cast<std::uint64_t>(std::hash<std::string>{}(range.hiZInactiveReason)));
		HashCombine(signature, range.selectedClusterOffset);
		HashCombine(signature, range.selectedClusterCount);
		HashCombine(signature, range.pageNeedOffset);
		HashCombine(signature, range.pageNeedCount);
		return signature;
	}

	template<typename State>
	void HashVirtualGeometryPacketTables(std::uint64_t& signature, const State& state)
	{
		for (const VirtualGeometryClusterPacket& packet : state.clusterPackets)
		{
			HashCombine(signature, packet.packetId);
			HashCombine(signature, packet.viewId);
			HashCombine(signature, static_cast<std::uint64_t>(packet.viewType));
			HashCombine(signature, HashUuid(packet.objectId));
			HashCombine(signature, HashUuid(packet.meshId));
			HashCombine(signature, HashUuid(packet.materialId));
			HashCombine(signature, packet.selectedLod);
			HashCombine(signature, packet.clusterId);
			HashCombine(signature, packet.meshletOffset);
			HashCombine(signature, packet.meshletCount);
			HashCombine(signature, packet.pageId);
			HashCombine(signature, packet.fallbackPageId);
			HashCombine(signature, static_cast<std::uint64_t>(packet.payloadSource));
			HashCombine(signature, static_cast<std::uint64_t>(packet.emittedSource));
			HashCombine(signature, packet.pagePayloadByteOffset);
			HashCombine(signature, packet.pagePayloadByteSize);
			HashCombine(signature, packet.debugIdentity);
		}
		for (const ClusterWorkBatch& batch : state.clusterBatches)
		{
			HashCombine(signature, batch.batchId);
			HashCombine(signature, batch.viewId);
			HashCombine(signature, HashUuid(batch.meshId));
			HashCombine(signature, HashUuid(batch.materialId));
			HashCombine(signature, batch.packetOffset);
			HashCombine(signature, batch.packetCount);
			HashCombine(signature, batch.selectedMeshlets);
			HashCombine(signature, batch.submittedMeshlets);
			HashCombine(signature, static_cast<std::uint64_t>(batch.fallbackMode));
		}
		for (const CompactedVisibilitySet& compaction : state.compactionResults)
		{
			HashCombine(signature, compaction.viewId);
			HashCombine(signature, compaction.candidatePackets);
			HashCombine(signature, compaction.visiblePackets);
			HashCombine(signature, compaction.submittedBatches);
			HashCombine(signature, static_cast<std::uint64_t>(compaction.source));
		}
		for (const GpuVirtualGeometryPacketRecord& packet : state.gpuPacketRecords)
		{
			HashCombine(signature, packet.packetId);
			HashCombine(signature, packet.viewId);
			HashCombine(signature, static_cast<std::uint64_t>(packet.viewType));
			HashCombine(signature, packet.frameId);
			HashCombine(signature, HashUuid(packet.objectId));
			HashCombine(signature, HashUuid(packet.meshId));
			HashCombine(signature, HashUuid(packet.materialId));
			HashCombine(signature, packet.selectedLod);
			HashCombine(signature, packet.clusterId);
			HashCombine(signature, packet.meshletOffset);
			HashCombine(signature, packet.meshletCount);
			HashCombine(signature, packet.pageId);
			HashCombine(signature, packet.fallbackPageId);
			HashCombine(signature, static_cast<std::uint64_t>(packet.payloadSource));
			HashCombine(signature, static_cast<std::uint64_t>(packet.emittedSource));
			HashCombine(signature, packet.pagePayloadByteOffset);
			HashCombine(signature, packet.pagePayloadByteSize);
			HashCombine(signature, packet.debugIdentity);
			HashCombine(signature, packet.flags);
		}
		for (const GpuVirtualGeometryBatchDescriptor& batch : state.gpuBatchDescriptors)
		{
			HashCombine(signature, batch.batchId);
			HashCombine(signature, batch.viewId);
			HashCombine(signature, batch.frameId);
			HashCombine(signature, batch.packetOffset);
			HashCombine(signature, batch.packetCount);
			HashCombine(signature, HashUuid(batch.meshId));
			HashCombine(signature, HashUuid(batch.materialId));
			HashCombine(signature, batch.selectedMeshlets);
			HashCombine(signature, batch.submittedMeshlets);
			HashCombine(signature, static_cast<std::uint64_t>(batch.debugMode));
			HashCombine(signature, static_cast<std::uint64_t>(batch.fallbackMode));
			HashCombine(signature, batch.flags);
		}
		for (const GpuVirtualGeometryPacketFrameSet& frameSet : state.gpuPacketFrameSets)
		{
			HashCombine(signature, frameSet.viewId);
			HashCombine(signature, frameSet.frameId);
			HashCombine(signature, frameSet.packetOffset);
			HashCombine(signature, frameSet.packetCount);
			HashCombine(signature, frameSet.batchOffset);
			HashCombine(signature, frameSet.batchCount);
			HashCombine(signature, frameSet.generatedSubmissionOffset);
			HashCombine(signature, frameSet.generatedSubmissionCount);
			HashCombine(signature, static_cast<std::uint64_t>(frameSet.validity));
		}
		for (const GpuPacketCompactionDispatch& dispatch : state.gpuCompactionDispatches)
		{
			HashCombine(signature, dispatch.dispatchId);
			HashCombine(signature, dispatch.viewId);
			HashCombine(signature, dispatch.frameId);
			HashCombine(signature, dispatch.candidatePacketCount);
			HashCombine(signature, dispatch.candidateBatchCount);
			HashCombine(signature, dispatch.outputCapacity);
			HashCombine(signature, static_cast<std::uint64_t>(dispatch.hiZBaselineState));
			HashCombine(signature, dispatch.failOpenOnInvalid ? 1u : 0u);
		}
		for (const GeneratedVirtualGeometrySubmissionRecord& submission : state.generatedSubmissions)
		{
			HashCombine(signature, submission.recordId);
			HashCombine(signature, submission.viewId);
			HashCombine(signature, submission.frameId);
			HashCombine(signature, submission.batchId);
			HashCombine(signature, submission.packetOffset);
			HashCombine(signature, submission.packetCount);
			HashCombine(signature, submission.indirectTaskCount);
			HashCombine(signature, HashUuid(submission.materialId));
			HashCombine(signature, HashUuid(submission.meshId));
			HashCombine(signature, submission.counterOffset);
			HashCombine(signature, static_cast<std::uint64_t>(submission.source));
			HashCombine(signature, static_cast<std::uint64_t>(submission.validationState));
		}
		for (const VirtualGeometryCounterReadbackRecord& readback : state.gpuCounterReadbacks)
		{
			HashCombine(signature, readback.viewId);
			HashCombine(signature, readback.frameId);
			HashCombine(signature, static_cast<std::uint64_t>(readback.source));
			HashCombine(signature, readback.latencyFrames);
			HashCombine(signature, readback.candidatePackets);
			HashCombine(signature, readback.compactedPackets);
			HashCombine(signature, readback.submittedBatches);
		}
	}

	std::uint64_t HashVirtualGeometryBatchPacketSignature(const RenderFrameSnapshot& snapshot)
	{
		std::uint64_t signature = snapshot.frameId ^ snapshot.resourceRevision;
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			HashVirtualGeometryPacketTables(signature, state);
		}
		for (const VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			HashVirtualGeometryPacketTables(signature, state);
		}
		return signature;
	}

	std::uint64_t HashRenderableRecords(const std::vector<RenderFrameRenderableRecord>& records)
	{
		std::uint64_t signature = 1099511628211ull;
		for (const RenderFrameRenderableRecord& record : records)
		{
			HashCombine(signature, HashUuid(record.objectId));
			HashCombine(signature, HashUuid(record.meshId));
			HashCombine(signature, HashUuid(record.materialId));
			HashCombine(signature, record.taskShaderCullingEnabled ? 1u : 0u);
			HashCombine(signature, HashVirtualGeometryRange(record.virtualGeometry));
		}
		return signature;
	}

	std::uint64_t HashDescriptorSignature(const RenderFrameSnapshot& snapshot, std::uint32_t viewId)
	{
		std::uint64_t signature = snapshot.resourceRevision;
		HashCombine(signature, viewId);
		HashCombine(signature, HashResourceRecords(snapshot.materials));
		HashCombine(signature, HashResourceRecords(snapshot.textures));
		return signature;
	}

	std::uint64_t HashPipelineSignature(const RenderFrameSnapshot& snapshot)
	{
		std::uint64_t signature = snapshot.resourceRevision;
		HashCombine(signature, HashRenderableRecords(snapshot.renderables));
		return signature;
	}

	std::uint64_t HashUploadSignature(const RenderFrameSnapshot& snapshot)
	{
		std::uint64_t signature = snapshot.resourceRevision;
		HashCombine(signature, HashResourceRecords(snapshot.meshes));
		HashCombine(signature, HashResourceRecords(snapshot.textures));
		return signature;
	}

	std::uint64_t HashPersistentPacketSignature(const RenderFrameRenderableRecord& record)
	{
		std::uint64_t signature = HashUuid(record.meshId);
		HashCombine(signature, HashUuid(record.materialId));
		HashCombine(signature, record.taskShaderCullingEnabled ? 1u : 0u);
		HashCombine(signature, HashVirtualGeometryRange(record.virtualGeometry));
		return signature;
	}
}

	CachedDescriptorBindingPlan VulkanRendererPreparationCache::GetDescriptorPlan(const RenderFrameSnapshot& snapshot, std::uint32_t viewId)
	{
		CachedDescriptorBindingPlan result;
		result.signature = HashDescriptorSignature(snapshot, viewId);
		if (const auto existing = descriptorPlanCache.find(result.signature); existing != descriptorPlanCache.end())
		{
			result.plan = existing->second;
			result.cacheHit = true;
			return result;
		}

		result.plan = BuildDescriptorBindingPlan(snapshot, viewId);
		descriptorPlanCache[result.signature] = result.plan;
		TrimCachesIfNeeded();
		return result;
	}

	CachedPipelineStateKeyPlan VulkanRendererPreparationCache::GetPipelineStateKeys(const RenderFrameSnapshot& snapshot)
	{
		CachedPipelineStateKeyPlan result;
		result.signature = HashPipelineSignature(snapshot);
		if (const auto existing = pipelineKeyCache.find(result.signature); existing != pipelineKeyCache.end())
		{
			result.keys = existing->second;
			result.cacheHit = true;
			return result;
		}

		result.keys = BuildPipelineStateKeys(snapshot);
		pipelineKeyCache[result.signature] = result.keys;
		TrimCachesIfNeeded();
		return result;
	}

	CachedUploadPreparationPlan VulkanRendererPreparationCache::GetUploadPlan(const RenderFrameSnapshot& snapshot)
	{
		CachedUploadPreparationPlan result;
		result.signature = HashUploadSignature(snapshot);
		if (const auto existing = uploadPlanCache.find(result.signature); existing != uploadPlanCache.end())
		{
			result.plan = existing->second;
			result.cacheHit = true;
		}
		else
		{
			result.plan = BuildUploadPreparationPlan(snapshot);
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

	PersistentRenderPacketCacheStats VulkanRendererPreparationCache::RefreshPersistentRenderPackets(const RenderFrameSnapshot& snapshot)
	{
		PersistentRenderPacketCacheStats stats;
		stats.packetCount = snapshot.renderables.size();
		std::unordered_set<TerraEngine::UUID> aliveObjects;
		aliveObjects.reserve(snapshot.renderables.size());

		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			aliveObjects.insert(renderable.objectId);
			const std::uint64_t signature = HashPersistentPacketSignature(renderable);
			const auto existing = persistentRenderPacketSignatures.find(renderable.objectId);
			if (existing != persistentRenderPacketSignatures.end() && existing->second == signature)
			{
				++stats.cacheHits;
			}
			else
			{
				++stats.cacheMisses;
				persistentRenderPacketSignatures[renderable.objectId] = signature;
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
		const RenderFrameSnapshot& snapshot,
		const std::vector<RenderPassNode>& passes,
		const BackendCapabilityProfile& capabilities)
	{
		VulkanRendererPreparationSummary summary;
		summary.frameId = snapshot.frameId;
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			CachedDescriptorBindingPlan descriptorPlan = GetDescriptorPlan(snapshot, view.outputId);
			summary.cacheLookupCount += 1u;
			summary.cacheHitCount += descriptorPlan.cacheHit ? 1u : 0u;
			summary.descriptorPlans.push_back(std::move(descriptorPlan));
		}

		summary.pipelineKeys = GetPipelineStateKeys(snapshot);
		summary.cacheLookupCount += 1u;
		summary.cacheHitCount += summary.pipelineKeys.cacheHit ? 1u : 0u;

		summary.uploadPlan = GetUploadPlan(snapshot);
		summary.cacheLookupCount += 1u;
		summary.cacheHitCount += summary.uploadPlan.cacheHit ? 1u : 0u;

		summary.transitionPlans.reserve(passes.size());
		for (const RenderPassNode& pass : passes)
		{
			summary.transitionPlans.push_back(BuildResourceTransitionPlan(snapshot.frameId, pass));
		}

		summary.renderPacketCache = RefreshPersistentRenderPackets(snapshot);
		summary.visibilityPlan = BuildVisibilityPreparationPlan(snapshot, capabilities);
		summary.indirectDrawPlan = BuildIndirectDrawPreparationPlan(snapshot, capabilities);
		summary.shadowPlan = BuildShadowRecordingPreparationPlan(snapshot, capabilities);
		summary.shaderMaterialPlan = BuildShaderMaterialPreparationPlan(snapshot, capabilities);
		summary.uiPlan = BuildUiGeometryPreparationPlan(snapshot, capabilities);
		summary.retiredResources = RetireCompletedResources(snapshot.frameId > 2u ? snapshot.frameId - 2u : 0u);
		summary.waitingRetirements.assign(pendingRetirements.begin(), pendingRetirements.end());
		summary.diagnostics = DrainThreadDiagnostics();
		return summary;
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
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities)
	{
		VisibilityPreparationPlan plan;
		plan.renderableCount = snapshot.renderables.size();
		plan.lightCount = snapshot.lights.size();
		const std::size_t workload = plan.renderableCount + plan.lightCount;
		plan.workerEligible = capabilities.supportsParallelCulling && workload >= kParallelPreparationThreshold;
		plan.chunkCount = plan.workerEligible ? std::min<std::size_t>(std::max<std::uint32_t>(1u, capabilities.maxRecordingWorkers), workload) : 1u;
		return plan;
	}

	IndirectDrawPreparationPlan BuildIndirectDrawPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities)
	{
		IndirectDrawPreparationPlan plan;
		plan.drawCount = snapshot.renderables.size();
		plan.instancePayloadCount = snapshot.renderables.size();
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			plan.virtualGeometryClusterPacketCount += state.clusterPackets.size();
			plan.virtualGeometryBatchCount += state.clusterBatches.size();
			plan.virtualGeometryCompactionResultCount += state.compactionResults.size();
			plan.virtualGeometryGpuPacketRecordCount += state.gpuPacketRecords.size();
			plan.virtualGeometryGpuBatchDescriptorCount += state.gpuBatchDescriptors.size();
			plan.virtualGeometryGpuPacketFrameSetCount += state.gpuPacketFrameSets.size();
			plan.virtualGeometryGpuCompactionDispatchCount += state.gpuCompactionDispatches.size();
			plan.virtualGeometryGeneratedSubmissionCount += state.generatedSubmissions.size();
			plan.virtualGeometryCounterReadbackCount += state.gpuCounterReadbacks.size();
		}
		for (const VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			plan.virtualGeometryClusterPacketCount += state.clusterPackets.size();
			plan.virtualGeometryBatchCount += state.clusterBatches.size();
			plan.virtualGeometryCompactionResultCount += state.compactionResults.size();
			plan.virtualGeometryGpuPacketRecordCount += state.gpuPacketRecords.size();
			plan.virtualGeometryGpuBatchDescriptorCount += state.gpuBatchDescriptors.size();
			plan.virtualGeometryGpuPacketFrameSetCount += state.gpuPacketFrameSets.size();
			plan.virtualGeometryGpuCompactionDispatchCount += state.gpuCompactionDispatches.size();
			plan.virtualGeometryGeneratedSubmissionCount += state.generatedSubmissions.size();
			plan.virtualGeometryCounterReadbackCount += state.gpuCounterReadbacks.size();
		}
		plan.virtualGeometryBatchPacketSignature = HashVirtualGeometryBatchPacketSignature(snapshot);
		plan.gpuDrivenEligible = capabilities.supportsParallelRecording && !snapshot.renderables.empty();
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

	ShadowRecordingPreparationPlan BuildShadowRecordingPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities)
	{
		ShadowRecordingPreparationPlan plan;
		for (const RenderFrameLightRecord& light : snapshot.lights)
		{
			if (!light.castsShadows)
			{
				continue;
			}
			++plan.shadowLightCount;
			plan.secondaryRecordingCount += light.lightType == 1u ? 6u : 1u;
		}
		plan.workerEligible = capabilities.supportsParallelShadowRecording && capabilities.supportsSecondaryRecording && plan.secondaryRecordingCount > 1u;
		return plan;
	}

	ShaderMaterialPreparationPlan BuildShaderMaterialPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities)
	{
		ShaderMaterialPreparationPlan plan;
		plan.materialCount = snapshot.materials.size();
		plan.shaderVariantCount = snapshot.materials.size();
		plan.workerEligible = capabilities.supportsParallelPipelineStatePreparation && plan.materialCount >= kParallelPreparationThreshold;
		return plan;
	}

	UiGeometryPreparationPlan BuildUiGeometryPreparationPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities)
	{
		UiGeometryPreparationPlan plan;
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			if (view.viewType == RenderViewType::Game || view.viewType == RenderViewType::Scene)
			{
				++plan.viewCount;
				plan.estimatedVertexCount += 6u;
			}
		}
		plan.workerEligible = capabilities.supportsParallelUiPreparation && plan.estimatedVertexCount >= kParallelPreparationThreshold;
		return plan;
	}

}
