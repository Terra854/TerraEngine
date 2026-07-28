#include "VulkanUploadPlanning.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace
{
	VirtualGeometryEmittedPageSource ResolveUploadPlanningEmittedSource(VirtualGeometryPagePayloadSource source)
	{
		switch (source)
		{
		case VirtualGeometryPagePayloadSource::ResidentPagePayload:
			return VirtualGeometryEmittedPageSource::HighDetailPage;
		case VirtualGeometryPagePayloadSource::ResidentFallbackPayload:
			return VirtualGeometryEmittedPageSource::FallbackPage;
		case VirtualGeometryPagePayloadSource::EmbeddedMeshPayload:
		case VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback:
			return VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
		case VirtualGeometryPagePayloadSource::Missing:
		case VirtualGeometryPagePayloadSource::Invalid:
			return VirtualGeometryEmittedPageSource::Invalid;
		case VirtualGeometryPagePayloadSource::Unknown:
		default:
			return VirtualGeometryEmittedPageSource::None;
		}
	}

	void HashPagePayloadValue(std::uint64_t& signature, std::uint64_t value)
	{
		// This is a fast change detector rather than a security hash. Packet order
		// is significant because descriptor rows are consumed in submission order.
		signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6u) + (signature >> 2u);
	}
}

namespace TerraEngine::Renderer::Vulkan {

	std::uint64_t BuildVirtualGeometryPagePayloadReuseSignature(const std::vector<GpuVirtualGeometryPacketRecord>& packets)
	{
		std::uint64_t signature = 1469598103934665603ull;
		HashPagePayloadValue(signature, packets.size());
		for (const GpuVirtualGeometryPacketRecord& packet : packets)
		{
			// Include every field copied into the descriptor table or used to
			// extract meshlet bytes. Frame ownership, transforms, and culling
			// bounds deliberately stay out because they do not change this buffer.
			HashPagePayloadValue(signature, packet.packetId);
			HashPagePayloadValue(signature, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.meshId)));
			HashPagePayloadValue(signature, packet.selectedLod);
			HashPagePayloadValue(signature, packet.clusterId);
			HashPagePayloadValue(signature, packet.meshletOffset);
			HashPagePayloadValue(signature, packet.meshletCount);
			HashPagePayloadValue(signature, packet.pageId);
			HashPagePayloadValue(signature, packet.fallbackPageId);
			HashPagePayloadValue(signature, static_cast<std::uint64_t>(packet.payloadSource));
			HashPagePayloadValue(signature, static_cast<std::uint64_t>(packet.emittedSource));
			HashPagePayloadValue(signature, packet.pagePayloadByteOffset);
			HashPagePayloadValue(signature, packet.pagePayloadByteSize);
			HashPagePayloadValue(signature, packet.payloadBufferIndex);
			HashPagePayloadValue(signature, packet.payloadDescriptorIndex);
			HashPagePayloadValue(signature, packet.flags);
		}
		return signature;
	}

	bool CanReuseVirtualGeometryPagePayload(
		std::uint64_t cachedSignature,
		std::uint64_t currentSignature,
		bool cacheValid,
		bool bufferAvailable)
	{
		return cacheValid && bufferAvailable && cachedSignature == currentSignature;
	}

	PersistentPhysicalPageUploadPlan
	BuildPersistentPhysicalPageUploadPlan(
		std::vector<PersistentPhysicalPageUploadInput> inputs,
		std::uint64_t poolCapacityBytes,
		std::uint64_t uploadBudgetBytes,
		std::uint64_t allocationGeneration)
	{
		PersistentPhysicalPageUploadPlan plan;
		if(allocationGeneration == 0u)
		{
			plan.rejectionReason =
				"Persistent page allocation generation is missing.";
			return plan;
		}
		std::sort(inputs.begin(), inputs.end(),
			[](const PersistentPhysicalPageUploadInput& left,
				const PersistentPhysicalPageUploadInput& right)
			{
				if(left.pinnedFallback != right.pinnedFallback)
				{
					return left.pinnedFallback;
				}
				if(left.meshId != right.meshId)
				{
					return std::lexicographical_compare(
						left.meshId.begin(), left.meshId.end(),
						right.meshId.begin(), right.meshId.end());
				}
				if(left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.contentGeneration
					< right.contentGeneration;
			});
		std::uint64_t cursor = 0u;
		std::uint64_t uploadedBytes = 0u;
		for(const PersistentPhysicalPageUploadInput& input : inputs)
		{
			if(input.meshId == TerraEngine::EMPTY_UUID
				|| input.pageId == 0xffffffffu
				|| input.contentGeneration == 0u
				|| input.residencyGeneration == 0u
				|| input.byteSize == 0u)
			{
				plan.uploads.clear();
				plan.rejectionReason =
					"Persistent page upload input is incomplete.";
				return plan;
			}
			if(!input.eligibleForNewWork && !input.pinnedFallback)
			{
				// Eviction first removes eligibility. Such a page may remain
				// alive for old fences but cannot enter this generation's plan.
				continue;
			}
			PersistentPhysicalPageUpload upload;
			upload.input = input;
			upload.reuseExisting =
				input.existingAllocationId != 0u;
			upload.allocationId =
				upload.reuseExisting
					? input.existingAllocationId
					: (allocationGeneration << 32u)
						^ static_cast<std::uint64_t>(
							input.pageId + 1u);
			if(upload.reuseExisting)
			{
				++plan.reusedPages;
				plan.uploads.push_back(upload);
				continue;
			}
			if(cursor > std::numeric_limits<std::uint64_t>::max()
					- 255u)
			{
				plan.uploads.clear();
				plan.rejectionReason =
					"Persistent page pool alignment overflowed.";
				return plan;
			}
			cursor = (cursor + 255u) & ~std::uint64_t{255u};
			if(input.byteSize
					> std::numeric_limits<std::uint64_t>::max()
						- cursor
				|| input.byteSize
					> std::numeric_limits<std::uint64_t>::max()
						- uploadedBytes)
			{
				plan.uploads.clear();
				plan.rejectionReason =
					"Persistent page pool byte range overflowed.";
				return plan;
			}
			upload.poolByteOffset = cursor;
			cursor += input.byteSize;
			uploadedBytes += input.byteSize;
			plan.requestedBytes = cursor;
			if((poolCapacityBytes != 0u
					&& cursor > poolCapacityBytes)
				|| (uploadBudgetBytes != 0u
					&& uploadedBytes > uploadBudgetBytes))
			{
				plan.uploads.clear();
				plan.rejectionReason =
					"Persistent page upload exceeds the configured "
					"pool or per-frame upload budget.";
				return plan;
			}
			plan.uploads.push_back(upload);
		}
		plan.committedBytes = cursor;
		plan.valid = true;
		return plan;
	}

	UploadPreparationPlan BuildUploadPreparationPlan(const RenderFrameSnapshot& snapshot, std::size_t payloadBudget)
	{
		UploadPreparationPlan plan;
		plan.frameId = snapshot.frameId;
		plan.resourceRevision = snapshot.resourceRevision;
		plan.cancellationToken = { snapshot.frameId, snapshot.resourceRevision, snapshot.frameId, "upload-plan" };
		for (const RenderFrameResourceRecord& mesh : snapshot.meshes)
		{
			if (!mesh.resident)
			{
				plan.payloads.push_back("mesh:" + TerraEngine::ToHexString(mesh.resourceId));
			}
		}
		for (const RenderFrameResourceRecord& texture : snapshot.textures)
		{
			if (!texture.resident)
			{
				plan.payloads.push_back("texture:" + TerraEngine::ToHexString(texture.resourceId));
			}
		}
		plan.budget = { RenderWorkBudgetCategory::Upload, payloadBudget, plan.payloads.size(), RenderBudgetOverflowPolicy::Delay, "upload-payloads" };
		plan.overBudget = IsRenderWorkOverBudget(plan.budget);
		while (payloadBudget > 0u && plan.payloads.size() > payloadBudget)
		{
			plan.delayedPayloads.push_back(plan.payloads.back());
			plan.payloads.pop_back();
		}
		return plan;
	}

	VirtualGeometryPageUploadPublication PlanVirtualGeometryPageUploadPublication(const GeometryPageRuntime& page)
	{
		VirtualGeometryPageUploadPublication publication;
		publication.pageId = page.pageId;
		if (page.state == GeometryPageState::Resident
			|| page.state == GeometryPageState::Pinned
			|| page.state == GeometryPageState::PendingRetirement)
		{
			publication.accepted = true;
			publication.publishedState = page.state;
			publication.reason = "virtual geometry page is already published";
			return publication;
		}
		if (page.state != GeometryPageState::PendingUpload)
		{
			publication.reason = "virtual geometry page is not waiting for protected GPU upload";
			return publication;
		}
		if (page.byteSize == 0u)
		{
			publication.reason = "virtual geometry page upload has no byte payload";
			return publication;
		}

		// The actual Vulkan buffer creation and descriptor publication stay in
		// the coordinator-owned backend path. This pure hook lets workers and
		// tests agree on the state that may be published once upload succeeds.
		publication.accepted = true;
		publication.publishedState = (page.fallbackPage || page.pinnedFallback)
			? GeometryPageState::Pinned
			: GeometryPageState::Resident;
		publication.reason = publication.publishedState == GeometryPageState::Pinned
			? "virtual geometry fallback page uploaded and pinned"
			: "virtual geometry page uploaded";
		return publication;
	}

	VirtualGeometryPageRetirementPlan PlanVirtualGeometryPageRetirement(const GeometryPageRuntime& page, std::uint64_t frameId, std::uint64_t safeFrameLag)
	{
		VirtualGeometryPageRetirementPlan plan;
		plan.pageId = page.pageId;
		if (page.fallbackPage
			|| page.pinnedFallback
			|| page.protectedFallback
			|| page.state == GeometryPageState::Pinned)
		{
			plan.reason = "virtual geometry fallback page is protected from retirement";
			return plan;
		}
		if (page.state != GeometryPageState::Evicting)
		{
			plan.reason = "virtual geometry page is not marked evicting";
			return plan;
		}

		// Page retirement waits for completed frames so in-flight command
		// buffers never read buffers after the backend releases them.
		plan.canRetire = true;
		plan.retireAfterFrameId = frameId + safeFrameLag;
		plan.reason = "virtual geometry page queued for deferred retirement";
		return plan;
	}

	std::vector<VirtualGeometryPagePayloadUploadRecord> BuildVirtualGeometryPagePayloadUploadPlan(
		const RenderFrameSnapshot& snapshot,
		RenderViewType viewType,
		std::uint32_t viewId)
	{
		std::vector<VirtualGeometryPagePayloadUploadRecord> records;
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			if (state.viewType != viewType)
			{
				continue;
			}
			if (viewId != std::numeric_limits<std::uint32_t>::max() && state.outputId != viewId)
			{
				continue;
			}

			auto appendRecord = [&](const VirtualGeometryPagePayloadRecord& payload) {
				VirtualGeometryPagePayloadUploadRecord record;
				record.meshId = payload.meshId;
				record.viewId = state.outputId;
				record.viewType = state.viewType;
				record.pageId = payload.pageId;
				record.fallbackPageId = payload.fallbackPageId;
				record.payloadSource = payload.source;
				record.emittedSource = ResolveUploadPlanningEmittedSource(record.payloadSource);
				record.payloadByteOffset = payload.payloadByteOffset;
				record.payloadByteSize = payload.payloadByteSize;
				record.payloadBufferIndex = payload.payloadBufferIndex;
				record.payloadDescriptorIndex = payload.payloadDescriptorIndex;
				record.allocationId = payload.allocationId;
				record.pinnedFallback = payload.pinnedFallback;
				const bool hasPhysicalBinding = payload.payloadBufferIndex != std::numeric_limits<std::uint32_t>::max()
					&& payload.payloadDescriptorIndex != std::numeric_limits<std::uint32_t>::max();
				switch (payload.source)
				{
				case VirtualGeometryPagePayloadSource::ResidentPagePayload:
					record.accepted = payload.resident && payload.valid && payload.payloadByteSize > 0u && hasPhysicalBinding;
					record.reason = record.accepted
						? "resident high-detail virtual geometry page payload is ready for upload publication"
						: "resident high-detail page payload is missing byte metadata or a physical descriptor binding";
					break;
				case VirtualGeometryPagePayloadSource::ResidentFallbackPayload:
					record.accepted = payload.resident && payload.valid && payload.payloadByteSize > 0u && hasPhysicalBinding;
					record.reason = record.accepted
						? "resident fallback virtual geometry page payload is ready and protected"
						: "resident fallback page payload is missing byte metadata or a physical descriptor binding";
					break;
				case VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback:
					record.accepted = payload.valid;
					record.reason = "conservative embedded fallback is labeled for diagnostics instead of reported as streamed page geometry";
					break;
				default:
					record.reason = "virtual geometry page payload source is not publishable";
					break;
				}
				records.push_back(std::move(record));
			};

			for (const VirtualGeometryPagePayloadRecord& payload : state.residentPagePayloads)
			{
				appendRecord(payload);
			}
			if (!state.residentPagePayloads.empty())
			{
				continue;
			}

			// Older tests and conservative migration paths may only populate
			// packet records. Convert those packet labels into the same upload
			// contract so backend planning never has to infer source identity
			// from draw commands or live mesh state.
			for (const VirtualGeometryClusterPacket& packet : state.clusterPackets)
			{
				VirtualGeometryPagePayloadRecord payload;
				payload.meshId = packet.meshId;
				payload.pageId = packet.pageId;
				payload.fallbackPageId = packet.fallbackPageId;
				payload.source = packet.payloadSource;
				payload.selectedLod = packet.selectedLod;
				payload.meshletOffset = packet.meshletOffset;
				payload.meshletCount = packet.meshletCount;
				payload.payloadByteOffset = packet.pagePayloadByteOffset;
				payload.payloadByteSize = packet.pagePayloadByteSize;
				payload.payloadBufferIndex = packet.payloadBufferIndex;
				payload.payloadDescriptorIndex = packet.payloadDescriptorIndex;
				payload.owningMeshFallbackReason = packet.fallbackReason;
				payload.resident = packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentPagePayload
					|| packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				payload.pinnedFallback = packet.fallbackPacket || packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				payload.valid = packet.payloadSource != VirtualGeometryPagePayloadSource::Unknown
					&& packet.payloadSource != VirtualGeometryPagePayloadSource::Missing
					&& packet.payloadSource != VirtualGeometryPagePayloadSource::Invalid;
				appendRecord(payload);
			}
		}
		return records;
	}

}
