#include "VulkanRendererWorkCache.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_set>

namespace TerraEngine::Renderer::Vulkan {
	ShadowGroupedStreamPlan BuildShadowGroupedStreamPlan(const ShadowBatchPlan& batch,
		std::span<const ShadowBatchMember> members, std::span<const RenderInstanceRecord> expected,
		std::span<const RenderInstanceRecord> stream, const ShadowGroupedStreamOwner& owner,
		std::size_t capacityBytes)
	{
		ShadowGroupedStreamPlan plan;
		plan.view = batch.view; plan.owner = owner;
		// Derive fallback from actual selected inputs, never a malformed planner
		// list. An invalid optimized range must not silently drop original draws.
		if(members.size() > std::numeric_limits<std::uint32_t>::max()) return plan;
		for(std::size_t index = 0; index < members.size(); ++index)
			plan.allSingletonIndices.push_back(static_cast<std::uint32_t>(index));
		if(!batch.coverageComplete || batch.failure != ShadowBatchFailure::None
			|| batch.allSingletonIndices != plan.allSingletonIndices || expected.size() != members.size()
			|| !owner.rendererSessionGeneration || !owner.backendGeneration || !owner.allocationId
			|| !owner.allocationGeneration || !owner.frameSerial || !owner.commandSerial
			|| owner.frameSlot >= 3 || owner.snapshotSlot >= 8
			|| batch.view.rendererSessionGeneration != owner.rendererSessionGeneration
			|| batch.view.lightId == EMPTY_UUID || !batch.view.allocationGeneration
			|| !batch.view.extent[0] || !batch.view.extent[1]
			|| (batch.view.viewType != RenderViewType::Scene && batch.view.viewType != RenderViewType::Game)
			|| stream.size() > std::numeric_limits<std::size_t>::max() / sizeof(RenderInstanceRecord)
			|| stream.size() * sizeof(RenderInstanceRecord) > capacityBytes) return plan;
		std::vector<bool> covered(members.size(), false);
		std::set<std::tuple<UUID, std::uint64_t, std::uint64_t>> contributions;
		for(const auto& member : members)
		{
			if(member.objectId == EMPTY_UUID || !member.lifetimeGeneration || !member.meshletCount
				|| member.firstMeshlet > std::numeric_limits<std::uint64_t>::max() - member.meshletCount
				|| member.firstCluster > std::numeric_limits<std::uint64_t>::max() - member.clusterCount
				|| member.payloadOffset > std::numeric_limits<std::uint64_t>::max() - member.payloadSize
				|| !contributions.emplace(member.objectId, member.lifetimeGeneration, member.segmentId).second) return plan;
		}
		std::size_t nextInstance = batch.groups.empty() ? 0 : batch.groups.front().firstInstance;
		for(const auto& group : batch.groups)
		{
			const std::size_t first = group.firstInstance, count = group.instanceCount;
			if(count < 2 || count != group.memberIndices.size() || first != nextInstance
				|| first > stream.size() || count > stream.size() - first
				|| group.representativeIndex >= members.size()
				|| std::find(group.memberIndices.begin(), group.memberIndices.end(), group.representativeIndex) == group.memberIndices.end()) return plan;
			// Exact descriptor/push identities and dispatch count are part of the
			// planner contract, not assumptions implied by a default shader name.
			if(!group.key.pushRangeCount || group.key.pushRangeCount > group.key.pushRanges.size()
				|| group.taskGroupsX != ((std::uint64_t(group.key.dispatchMeshletCount) + 31) / 32) * count) return plan;
			for(std::size_t set = 0; set < group.key.descriptors.size(); ++set)
				if(!group.key.descriptors[set] || !group.key.descriptorGenerations[set]) return plan;
			for(std::size_t range = 0; range < group.key.pushRangeCount; ++range)
				if(!group.key.pushRanges[range][0] || !group.key.pushRanges[range][2]
					|| group.key.pushRanges[range][1] > std::numeric_limits<std::uint32_t>::max() - group.key.pushRanges[range][2]) return plan;
			for(std::size_t index = 0; index < count; ++index)
			{
				const auto memberIndex = group.memberIndices[index];
				if(memberIndex >= members.size() || covered[memberIndex]) return plan;
				const auto& member = members[memberIndex];
				if(member.key != group.key || !member.knownDefaultTaskAbi || !member.supportedRoute
					|| !member.deformationBridgeComplete || member.conservativeDeformation
					|| member.objectId == EMPTY_UUID || !member.lifetimeGeneration
					|| !member.meshletCount || member.meshletCount != group.key.dispatchMeshletCount
					|| group.key.meshId == EMPTY_UUID || group.key.materialId == EMPTY_UUID || group.key.shaderId == EMPTY_UUID
					|| !group.key.pipeline || !group.key.layout || !group.key.pipelineGeneration || !group.key.layoutGeneration
					|| !group.key.meshGeneration || !group.key.materialGeneration || !group.key.shaderGeneration || !group.key.residencyGeneration
					|| expected[memberIndex].materialAndTexture.x != group.key.materialSelector
					|| expected[memberIndex].materialAndTexture.y != group.key.textureSelector
					|| expected[memberIndex].geometrySelectors.x != group.key.selectedLod
					|| expected[memberIndex].geometrySelectors.z != member.firstMeshlet
					|| expected[memberIndex].geometrySelectors.w != member.meshletCount
					|| std::memcmp(&stream[first + index], &expected[memberIndex], sizeof(RenderInstanceRecord)) != 0) return plan;
				covered[memberIndex] = true;
			}
			nextInstance = first + count;
		}
		for(const auto index : batch.singletonIndices)
		{
			if(index >= members.size() || covered[index]) return plan;
			covered[index] = true;
		}
		if(std::find(covered.begin(), covered.end(), false) != covered.end()) return plan;
		// No GPU state is changed here. Publish the immutable ranges only after
		// every selected contribution and byte range has passed validation.
		plan.groups = batch.groups; plan.singletonIndices = batch.singletonIndices;
		plan.instanceBytes = stream.size() * sizeof(RenderInstanceRecord);
		plan.valid = true;
		return plan;
	}
	ShadowGroupedExecutionSelection SelectShadowGroupedExecution(
		const ShadowGroupedStreamPlan& plan, const ShadowGroupedUploadReceipt& receipt)
	{
		ShadowGroupedExecutionSelection selection;
		selection.singletonIndices = plan.allSingletonIndices;
		// Upload success is separate from planned compatibility. Fence, actual
		// byte transfer and descriptor publication must all belong to this owner.
		if(plan.valid && receipt.view == plan.view && receipt.owner == plan.owner
			&& receipt.frameSlotSafe && receipt.ownerCurrent && receipt.mapSucceeded
			&& receipt.writeSucceeded && receipt.flushSucceeded && receipt.descriptorPublished)
		{
			selection.groups = plan.groups; selection.singletonIndices = plan.singletonIndices;
			selection.grouped = true;
		}
		return selection;
	}
	ShadowGroupedRecordedCounts CountShadowGroupedRecording(
		const ShadowGroupedStreamPlan& plan, const ShadowGroupedExecutionSelection& selection,
		const ShadowGroupedRecordingReceipt& receipt)
	{
		ShadowGroupedRecordedCounts counts;
		if(!receipt.recorded || receipt.owner != plan.owner || receipt.view != plan.view
			|| !plan.owner.rendererSessionGeneration || !plan.owner.backendGeneration
			|| (selection.grouped && (!plan.owner.allocationId || !plan.owner.allocationGeneration))
			|| !plan.owner.frameSerial || !plan.owner.commandSerial
			|| plan.owner.frameSlot >= 3 || plan.owner.snapshotSlot >= 8
			|| plan.view.rendererSessionGeneration != plan.owner.rendererSessionGeneration
			|| plan.view.lightId == EMPTY_UUID) return counts;
		if(selection.grouped)
		{
			if(!plan.valid || selection.groups.size() != plan.groups.size()
				|| selection.singletonIndices != plan.singletonIndices) return counts;
			for(std::size_t index = 0; index < plan.groups.size(); ++index)
			{
				const auto& actual = selection.groups[index]; const auto& sealed = plan.groups[index];
				if(actual.key != sealed.key || actual.representativeIndex != sealed.representativeIndex
					|| actual.firstInstance != sealed.firstInstance || actual.instanceCount != sealed.instanceCount
					|| actual.taskGroupsX != sealed.taskGroupsX || actual.memberIndices != sealed.memberIndices) return counts;
			}
		}
		else if(!selection.groups.empty() || selection.singletonIndices != plan.allSingletonIndices) return counts;
		// Original singletons do not read the grouped allocation. They can have
		// honest recording counts even when creating that allocation failed.
		std::vector<bool> recordedGroups(selection.groups.size(), false);
		for(const auto index : receipt.recordedGroupIndices)
		{
			if(index >= recordedGroups.size() || recordedGroups[index]) return counts;
			recordedGroups[index] = true;
		}
		if(std::find(recordedGroups.begin(), recordedGroups.end(), false) != recordedGroups.end()) return counts;
		auto singles = receipt.recordedSingletonIndices;
		auto required = selection.singletonIndices;
		std::sort(singles.begin(), singles.end()); std::sort(required.begin(), required.end());
		if(singles != required) return counts;
		counts.groups = recordedGroups.size(); counts.singletons = singles.size();
		counts.draws = counts.groups + counts.singletons; counts.segments = counts.singletons;
		for(const auto& group : selection.groups) counts.segments += group.instanceCount;
		counts.valid = true;
		return counts;
	}
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

	GroupedInstanceDirtyPlan BuildGroupedInstanceDirtyPlan(
		std::span<const RenderInstanceRecord> previous,
		std::span<const RenderInstanceRecord> current,
		bool allocationReplaced)
	{
		GroupedInstanceDirtyPlan plan;
		plan.completeReplacement = allocationReplaced
			|| previous.size() != current.size();
		if(current.empty())
		{
			return plan;
		}
		if(plan.completeReplacement)
		{
			plan.ranges.push_back({ 0u, current.size() });
			plan.dirtyRecordCount = current.size();
			return plan;
		}

		std::size_t rangeStart = current.size();
		for(std::size_t index = 0u; index < current.size(); ++index)
		{
			// RenderInstanceRecord is a fixed initialized shader ABI record. A byte
			// comparison therefore catches every GPU-visible lane without inventing
			// field-specific invalidation rules that could silently go stale.
			const bool changed = std::memcmp(&previous[index], &current[index],
				sizeof(RenderInstanceRecord)) != 0;
			if(changed && rangeStart == current.size())
			{
				rangeStart = index;
			}
			if(!changed && rangeStart != current.size())
			{
				plan.ranges.push_back({ rangeStart, index - rangeStart });
				plan.dirtyRecordCount += index - rangeStart;
				rangeStart = current.size();
			}
		}
		if(rangeStart != current.size())
		{
			plan.ranges.push_back({ rangeStart, current.size() - rangeStart });
			plan.dirtyRecordCount += current.size() - rangeStart;
		}
		return plan;
	}

	std::uint64_t BuildSecondaryCommandReuseSignature(
		const SecondaryCommandReuseIdentity& identity)
	{
		std::uint64_t signature = 1469598103934665603ull;
		HashCombine(signature, identity.rendererSessionGeneration);
		HashCombine(signature, identity.backendResourceGeneration);
		HashCombine(signature, identity.renderPassIdentity);
		HashCombine(signature, identity.framebufferIdentity);
		HashCombine(signature, identity.commandSignature);
		HashCombine(signature, identity.width);
		HashCombine(signature, identity.height);
		return signature;
	}

	bool CanReuseSecondaryCommands(std::uint64_t previousSignature,
		std::uint64_t currentSignature, bool frameSlotFenceComplete)
	{
		return frameSlotFenceComplete
			&& previousSignature != 0u
			&& previousSignature == currentSignature;
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
