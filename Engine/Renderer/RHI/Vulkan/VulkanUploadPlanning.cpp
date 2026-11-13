#include "VulkanUploadPlanning.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <unordered_set>

namespace
{
	// Priority sorting is independent of page identity. Validate the full typed
	// owner key so duplicates cannot hide in different pinned-fallback groups.
	struct PersistentPageIdentity {
		TerraEngine::UUID meshId;
		std::uint32_t pageId;
		bool operator==(const PersistentPageIdentity&) const = default;
	};
	struct PersistentPageIdentityHash {
		std::size_t operator()(const PersistentPageIdentity& key) const noexcept {
			return std::hash<TerraEngine::UUID>{}(key.meshId)
				^ (std::hash<std::uint32_t>{}(key.pageId) + std::size_t{0x9e3779b9u});
		}
	};
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
	CpuAnimationUploadLayout BuildCpuAnimationUploadLayout(const CpuAnimationDeformationPlan& plan)
	{
		CpuAnimationUploadLayout result;
		static_cast<void>(BuildCpuAnimationUploadLayout(plan, result));
		return result;
	}
	bool BuildCpuAnimationUploadLayout(const CpuAnimationDeformationPlan& plan, CpuAnimationUploadLayout& result)
	{
		// Reset all observable facts while retaining worker-local storage capacity.
		result.valid = false; result.paletteBytes = result.morphBytes = 0;
		result.byContent.clear(); result.slices.clear(); result.recordSliceIndices.clear();
		if (!plan.IsComplete()) return false;
		try
		{
			result.slices.reserve(plan.records.size()); result.recordSliceIndices.reserve(plan.records.size()); result.byContent.reserve(plan.records.size());
			for (std::size_t index = 0; index < plan.records.size(); ++index)
			{
				const auto& data = *plan.records[index].data;
				const CpuAnimationContentIdentity key{data.contentIdLow, data.contentIdHigh};
				if (const auto found = result.byContent.find(key); found != result.byContent.end())
				{
					result.recordSliceIndices.push_back(found->second); continue;
				}
				const auto palette = data.paletteMatrices.size() * sizeof(Animation::GpuAnimationPaletteRecord);
				const auto morph = data.morphWeights.size() * sizeof(float);
				if (palette > UINT64_MAX - result.paletteBytes || morph > UINT64_MAX - result.morphBytes) return false;
				result.byContent.emplace(key, result.slices.size()); result.recordSliceIndices.push_back(result.slices.size());
				result.slices.push_back({index, result.paletteBytes, result.morphBytes});
				result.paletteBytes += palette; result.morphBytes += morph;
			}
		}
		catch (const std::bad_alloc&) { return false; }
		result.valid = result.paletteBytes != 0; return result.valid;
	}
	CpuAnimationDeformationOffsetLayout BuildCpuAnimationDeformationOffsets(const CpuAnimationDeformationPlan& plan)
	{
		CpuAnimationDeformationOffsetLayout result;
		static_cast<void>(BuildCpuAnimationDeformationOffsets(plan, result));
		return result;
	}
	bool BuildCpuAnimationDeformationOffsets(const CpuAnimationDeformationPlan& plan, CpuAnimationDeformationOffsetLayout& result)
	{
		// Work records reference the exact shared-content slices used by upload,
		// rather than advancing offsets again for repeated instances of one pose.
		result.valid = false; result.byInstanceMesh.clear();
		if (!BuildCpuAnimationUploadLayout(plan, result.uploadScratch)) return false;
		try
		{
			result.byInstanceMesh.reserve(plan.records.size());
			for (std::size_t index = 0; index < plan.records.size(); ++index)
			{
				const auto& slice = result.uploadScratch.slices[result.uploadScratch.recordSliceIndices[index]];
				const CpuAnimationByteOffsets offsets{slice.paletteOffsetBytes, slice.morphOffsetBytes};
				const auto& record = plan.records[index];
				const auto [found, inserted] = result.byInstanceMesh.try_emplace(
					CpuAnimationDeformationOffsetLayout::Identity{record.stableInstanceId, record.meshId}, offsets);
				if (!inserted && (found->second.paletteOffsetBytes != offsets.paletteOffsetBytes
					|| found->second.morphOffsetBytes != offsets.morphOffsetBytes)) { result.byInstanceMesh.clear(); return false; }
			}
		}
		catch (const std::bad_alloc&) { result.byInstanceMesh.clear(); return false; }
		result.valid = true; return true;
	}

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

	PersistentPhysicalPageUploadPlan BuildPersistentPhysicalPageUploadPlan(
		std::vector<PersistentPhysicalPageUploadInput>, std::uint64_t,
		std::uint64_t, std::uint64_t)
	{
		// Keep the legacy symbol for existing binaries, but require source callers
		// to migrate: an ephemeral generation/page hash cannot establish ownership.
		PersistentPhysicalPageUploadPlan plan;
		plan.rejectionReason = "Legacy stateless overload is unsupported; pass persistent pool-owner allocator state and commit the stateful candidate at the protected publication boundary.";
		return plan;
	}
	PersistentPhysicalPageUploadPlan BuildPersistentPhysicalPageUploadPlan(
		std::vector<PersistentPhysicalPageUploadInput> inputs, std::uint64_t poolCapacityBytes,
		std::uint64_t uploadBudgetBytes, std::uint64_t allocationGeneration,
		const PersistentPhysicalPageAllocationState& observedAllocator,
		std::span<const std::uint64_t> retirementAllocations, std::uint64_t completedFence)
	{
		PersistentPhysicalPageUploadPlan plan;
		const auto reject = [&](const char* reason)
		{
			plan.valid = false; plan.uploads.clear(); plan.candidateAllocator.owners.clear();
			plan.requestedBytes = plan.committedBytes = plan.reusedPages = 0;
			plan.rejectionReason = reason; return std::move(plan);
		};
		if (!allocationGeneration || !observedAllocator.revision || observedAllocator.revision == UINT64_MAX)
			return reject("Persistent allocator generation/revision is missing or exhausted.");
		try
		{
			// Preparation never mutates the observed owner. The protected commit
			// compares its revision before adopting this complete candidate.
			for (const auto& [id, owner] : observedAllocator.owners)
				if (!id || (observedAllocator.nextAllocationId && id >= observedAllocator.nextAllocationId)
					|| owner.meshId == TerraEngine::EMPTY_UUID || owner.pageId == UINT32_MAX
					|| !owner.contentGeneration || !owner.residencyGeneration || !owner.byteSize || !owner.allocationGeneration)
					return reject("Persistent allocator owner/counter evidence is invalid.");
			plan.observedAllocatorRevision = observedAllocator.revision;
			plan.candidateAllocator = observedAllocator;
			plan.candidateAllocator.revision = observedAllocator.revision + 1;
			for (auto id : retirementAllocations)
			{
				const auto owner = plan.candidateAllocator.owners.find(id);
				if (owner == plan.candidateAllocator.owners.end() || owner->second.lastUseFence > completedFence)
					return reject("Retirement has no recorded owner or its last use is still pending.");
				plan.candidateAllocator.owners.erase(owner);
			}
			std::sort(inputs.begin(), inputs.end(), [](const auto& left, const auto& right)
			{
				if (left.pinnedFallback != right.pinnedFallback) return left.pinnedFallback;
				if (left.meshId != right.meshId) return std::lexicographical_compare(left.meshId.begin(), left.meshId.end(), right.meshId.begin(), right.meshId.end());
				if (left.pageId != right.pageId) return left.pageId < right.pageId;
				if (left.contentGeneration != right.contentGeneration) return left.contentGeneration < right.contentGeneration;
				return left.residencyGeneration < right.residencyGeneration;
			});
			std::uint64_t cursor = 0, uploadedBytes = 0;
			std::unordered_set<PersistentPageIdentity, PersistentPageIdentityHash> seenPages;
			seenPages.reserve(inputs.size());
			for (const auto& input : inputs)
			{
				if (input.meshId == TerraEngine::EMPTY_UUID || input.pageId == UINT32_MAX
					|| !input.contentGeneration || !input.residencyGeneration || !input.byteSize)
					return reject("Persistent page upload input is incomplete.");
				if (!seenPages.emplace(PersistentPageIdentity{input.meshId, input.pageId}).second)
					return reject("Persistent page upload inputs repeat a page identity.");
				if (!input.eligibleForNewWork && !input.pinnedFallback) continue;
				PersistentPhysicalPageUpload upload; upload.input = input;
				upload.reuseExisting = input.existingAllocationId != 0;
				if (upload.reuseExisting)
				{
					const auto found = plan.candidateAllocator.owners.find(input.existingAllocationId);
					if (found == plan.candidateAllocator.owners.end()) return reject("Reused allocation has no recorded live owner.");
					auto& owner = found->second;
					if (owner.meshId != input.meshId || owner.pageId != input.pageId || owner.contentGeneration != input.contentGeneration
						|| owner.residencyGeneration != input.residencyGeneration || owner.byteSize != input.byteSize)
						return reject("Reused allocation identity does not match its recorded owner.");
					owner.lastUseFence = std::max(owner.lastUseFence, input.completionFence);
					upload.allocationId = input.existingAllocationId; ++plan.reusedPages;
				}
				else
				{
					if (!plan.candidateAllocator.nextAllocationId) return reject("Persistent allocation ID space is exhausted.");
					if (cursor > UINT64_MAX - 255u) return reject("Persistent page alignment overflowed.");
					cursor = (cursor + 255u) & ~std::uint64_t{255u};
					if (input.byteSize > UINT64_MAX - cursor || input.byteSize > UINT64_MAX - uploadedBytes)
						return reject("Persistent page byte interval overflowed.");
					upload.poolByteOffset = cursor; cursor += input.byteSize; uploadedBytes += input.byteSize;
					if ((poolCapacityBytes && cursor > poolCapacityBytes) || (uploadBudgetBytes && uploadedBytes > uploadBudgetBytes))
						return reject("Persistent page upload exceeds its pool or frame budget.");
					upload.allocationId = plan.candidateAllocator.nextAllocationId++;
					plan.candidateAllocator.owners.emplace(upload.allocationId, PersistentPhysicalPageAllocationOwner{
						input.meshId, input.pageId, input.contentGeneration, input.residencyGeneration,
						input.byteSize, allocationGeneration, input.completionFence});
				}
				plan.uploads.push_back(upload);
			}
			plan.requestedBytes = plan.committedBytes = cursor; plan.valid = true;
		}
		catch (const std::bad_alloc&) { return reject("Persistent allocation candidate storage is unavailable."); }
		return plan;
	}
	bool CommitPersistentPhysicalPageUploadPlan(PersistentPhysicalPageUploadPlan& plan, PersistentPhysicalPageAllocationState& allocator)
	{
		// The caller already owns the pool's protected publication boundary. A
		// stale parallel candidate cannot partially publish IDs or retire owners.
		if (!plan.valid || plan.observedAllocatorRevision != allocator.revision
			|| allocator.revision == UINT64_MAX || plan.candidateAllocator.revision != allocator.revision + 1) return false;
		allocator.owners.swap(plan.candidateAllocator.owners);
		std::swap(allocator.nextAllocationId, plan.candidateAllocator.nextAllocationId);
		std::swap(allocator.revision, plan.candidateAllocator.revision);
		plan.valid = false; return true;
	}

	VirtualGeometryUploadIntentPlan BuildVirtualGeometryUploadIntentPlan(
		std::vector<VirtualGeometryPageUploadIntent> intents,
		std::vector<std::uint64_t> retirementAllocations)
	{
		VirtualGeometryUploadIntentPlan plan;
		std::sort(intents.begin(), intents.end(), [](const auto& left, const auto& right)
		{
			if(left.pinnedFallback != right.pinnedFallback) return left.pinnedFallback;
			if(left.meshId != right.meshId)
			{
				return std::lexicographical_compare(left.meshId.begin(), left.meshId.end(), right.meshId.begin(), right.meshId.end());
			}
			return left.pageId < right.pageId;
		});
		for(const VirtualGeometryPageUploadIntent& intent : intents)
		{
			if(intent.meshId == TerraEngine::EMPTY_UUID || intent.pageId == 0xffffffffu
				|| intent.contentGeneration == 0u || intent.byteSize == 0u || intent.sourceHash == 0u)
			{
				plan.rejectionReason = "Virtual geometry upload intent is incomplete.";
				return plan;
			}
			if(!plan.uploadIntents.empty()
				&& plan.uploadIntents.back().meshId == intent.meshId
				&& plan.uploadIntents.back().pageId == intent.pageId)
			{
				plan.rejectionReason = "Virtual geometry upload intents repeat a page identity.";
				return plan;
			}
			plan.uploadIntents.push_back(intent);
			if(intent.existingAllocationId != 0u)
			{
				plan.descriptorRefreshAllocations.push_back(intent.existingAllocationId);
			}
		}
		std::sort(retirementAllocations.begin(), retirementAllocations.end());
		retirementAllocations.erase(std::unique(retirementAllocations.begin(), retirementAllocations.end()), retirementAllocations.end());
		if(std::find(retirementAllocations.begin(), retirementAllocations.end(), 0u) != retirementAllocations.end())
		{
			plan.rejectionReason = "Virtual geometry retirement intent has no allocation.";
			return plan;
		}
		plan.retirementAllocations = std::move(retirementAllocations);
		plan.valid = true;
		return plan;
	}

	UploadPreparationPlan BuildUploadPreparationPlan(
		const RenderFramePublication& publication, std::size_t payloadBudget)
	{
		UploadPreparationPlan plan;
		plan.frameId = publication.frameId;
		plan.resourceRevision = publication.resourcePreparation.generation;
		plan.cancellationToken = { publication.frameId,
			publication.resourcePreparation.generation, publication.frameId, "upload-plan" };
		for(const RenderResourcePreparationRecord& resource :
			publication.resourcePreparation.records)
		{
			if(!resource.resident)
			{
				plan.payloads.push_back("resource:" + TerraEngine::ToHexString(
					resource.resourceId));
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
		std::span<const VirtualGeometryPagePayloadSelection> selections,
		RenderViewType viewType,
		std::uint32_t viewId)
	{
		std::vector<VirtualGeometryPagePayloadUploadRecord> records;
		// A caller passes only selections that already chose a virtual-geometry
		// route. This value-only boundary avoids reopening a snapshot just to infer
		// whether the backend should upload a page payload.
		for (const VirtualGeometryPagePayloadSelection& state : selections)
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

			for (const VirtualGeometryPagePayloadRecord& payload : state.residentPayloads)
			{
				appendRecord(payload);
			}
			if (!state.residentPayloads.empty())
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
