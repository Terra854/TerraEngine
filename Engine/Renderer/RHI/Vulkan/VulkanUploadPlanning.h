// VulkanUploadPlanning.h - immutable upload preparation records.

#pragma once

#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/VirtualGeometry/Selection/RenderViewSelectionData.h"
#include "Core/Export.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>

namespace TerraEngine::Renderer::Vulkan {
	// Copied upload layout: indices refer to the caller-owned immutable input
	// only while it is consumed; no borrowed data pointer escapes this plan.
	struct CpuAnimationUploadSlice
	{
		std::size_t sourceRecordIndex = 0;
		std::uint64_t paletteOffsetBytes = 0, morphOffsetBytes = 0;
	};
	// Exact content equality controls deduplication; the hash only chooses buckets.
	struct CpuAnimationContentIdentity
	{
		std::uint64_t low = 0, high = 0;
		bool operator==(const CpuAnimationContentIdentity&) const = default;
	};
	struct CpuAnimationContentIdentityHash
	{
		std::size_t operator()(const CpuAnimationContentIdentity& key) const noexcept
		{ return static_cast<std::size_t>(key.low ^ ((key.high << 1u) | (key.high >> 63u))); }
	};
	struct CpuAnimationUploadLayout
	{
		std::unordered_map<CpuAnimationContentIdentity, std::size_t, CpuAnimationContentIdentityHash> byContent;
		std::vector<CpuAnimationUploadSlice> slices;
		std::vector<std::size_t> recordSliceIndices;
		std::uint64_t paletteBytes = 0, morphBytes = 0;
		bool valid = false;
	};
	struct CpuAnimationByteOffsets
	{
		std::uint64_t paletteOffsetBytes = 0, morphOffsetBytes = 0;
	};
	struct CpuAnimationDeformationOffsetLayout
	{
		CpuAnimationUploadLayout uploadScratch;
		// One animator may drive multiple meshes with different skin bindings.
		// Preserve both identities even when content slices themselves deduplicate.
		struct Identity
		{
			std::uint64_t stableInstanceId = 0;
			TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
			bool operator==(const Identity&) const = default;
		};
		struct IdentityHash
		{
			std::size_t operator()(const Identity& value) const noexcept
			{
				const std::size_t mesh = std::hash<TerraEngine::UUID>{}(value.meshId);
				return std::hash<std::uint64_t>{}(value.stableInstanceId)
					^ (mesh + std::size_t{0x9e3779b9u} + (mesh << 6u) + (mesh >> 2u));
			}
		};
		std::unordered_map<Identity, CpuAnimationByteOffsets, IdentityHash> byInstanceMesh;
		bool valid = false;
	};
	// Upload bytes and work offsets consume the same content-slice planner.
	// Inplace callers retain their worker-owned vector and hash capacities.
	TERRAENGINE_API CpuAnimationUploadLayout BuildCpuAnimationUploadLayout(const CpuAnimationDeformationPlan& plan);
	TERRAENGINE_API bool BuildCpuAnimationUploadLayout(const CpuAnimationDeformationPlan& plan, CpuAnimationUploadLayout& scratch);
	TERRAENGINE_API CpuAnimationDeformationOffsetLayout BuildCpuAnimationDeformationOffsets(const CpuAnimationDeformationPlan& plan);
	TERRAENGINE_API bool BuildCpuAnimationDeformationOffsets(const CpuAnimationDeformationPlan& plan, CpuAnimationDeformationOffsetLayout& scratch);

	struct UploadPreparationPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t resourceRevision = 0u;
		std::vector<std::string> payloads;
		RenderWorkBudget budget;
		RenderCancellationToken cancellationToken;
		std::vector<std::string> delayedPayloads;
		bool queueSubmissionCoordinatorOwned = true;
		bool overBudget = false;
	};

	struct VirtualGeometryPageUploadPublication
	{
		std::uint32_t pageId = 0u;
		GeometryPageState publishedState = GeometryPageState::NotResident;
		bool accepted = false;
		std::string reason;
	};

	struct VirtualGeometryPageRetirementPlan
	{
		std::uint32_t pageId = 0u;
		std::uint64_t retireAfterFrameId = 0u;
		bool canRetire = false;
		std::string reason;
	};

	struct VirtualGeometryPagePayloadUploadRecord
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t viewId = 0u;
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackPageId = 0xffffffffu;
		VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
		VirtualGeometryEmittedPageSource emittedSource = VirtualGeometryEmittedPageSource::None;
		std::uint64_t payloadByteOffset = 0u;
		std::uint64_t payloadByteSize = 0u;
		std::uint32_t payloadBufferIndex = 0xffffffffu;
		std::uint32_t payloadDescriptorIndex = 0xffffffffu;
		std::uint64_t allocationId = 0u;
		bool pinnedFallback = false;
		bool accepted = false;
		std::string reason;
	};

	struct PersistentPhysicalPageUploadInput
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0xffffffffu;
		std::uint64_t contentGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t byteSize = 0u;
		std::uint64_t existingAllocationId = 0u;
		std::uint64_t completionFence = 0u;
		bool pinnedFallback = false;
		bool eligibleForNewWork = true;
	};

	struct PersistentPhysicalPageUpload
	{
		PersistentPhysicalPageUploadInput input;
		// Batch-local packed upload/staging offset, not a durable physical-pool
		// address. Durable identity and retirement belong to allocationId/owner.
		std::uint64_t poolByteOffset = 0u;
		std::uint64_t allocationId = 0u;
		bool reuseExisting = false;
	};

	// One protected pool/session owns this state. IDs never rewind, even after
	// completed retirement removes an owner. Full-width generations are evidence,
	// not bit-packed identities, so distinct meshes/pages cannot alias by truncation.
	struct PersistentPhysicalPageAllocationOwner
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0xffffffffu;
		std::uint64_t contentGeneration = 0, residencyGeneration = 0, byteSize = 0;
		std::uint64_t allocationGeneration = 0, lastUseFence = 0;
		bool operator==(const PersistentPhysicalPageAllocationOwner&) const = default;
	};
	struct PersistentPhysicalPageAllocationState
	{
		std::uint64_t revision = 1, nextAllocationId = 1;
		std::map<std::uint64_t, PersistentPhysicalPageAllocationOwner> owners;
	};
	struct PersistentPhysicalPageUploadPlan
	{
		std::vector<PersistentPhysicalPageUpload> uploads;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t committedBytes = 0u;
		std::uint32_t reusedPages = 0u;
		bool valid = false;
		std::string rejectionReason;
		std::uint64_t observedAllocatorRevision = 0;
		PersistentPhysicalPageAllocationState candidateAllocator;
	};

	// A value-only intent crosses from resource streaming to Vulkan's protected
	// submission boundary.  It describes work, never performs an upload itself.
	struct VirtualGeometryPageUploadIntent
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0xffffffffu;
		std::uint64_t contentGeneration = 0u;
		std::uint64_t byteSize = 0u;
		std::uint64_t sourceHash = 0u;
		std::uint64_t existingAllocationId = 0u;
		bool pinnedFallback = false;
	};

	struct VirtualGeometryUploadIntentPlan
	{
		std::vector<VirtualGeometryPageUploadIntent> uploadIntents;
		std::vector<std::uint64_t> descriptorRefreshAllocations;
		std::vector<std::uint64_t> retirementAllocations;
		bool valid = false;
		std::string rejectionReason;
	};

	// Upload preparation reads the sealed compact resource table. Worker snapshot
	// ownership ends before Vulkan receives this immutable publication.
	TERRAENGINE_API UploadPreparationPlan BuildUploadPreparationPlan(
		const RenderFramePublication& publication, std::size_t payloadBudget = 64u);
	TERRAENGINE_API VirtualGeometryPageUploadPublication PlanVirtualGeometryPageUploadPublication(const GeometryPageRuntime& page);
	TERRAENGINE_API VirtualGeometryPageRetirementPlan PlanVirtualGeometryPageRetirement(const GeometryPageRuntime& page, std::uint64_t frameId, std::uint64_t safeFrameLag = 2u);
	TERRAENGINE_API std::vector<VirtualGeometryPagePayloadUploadRecord> BuildVirtualGeometryPagePayloadUploadPlan(std::span<const VirtualGeometryPagePayloadSelection> selections, RenderViewType viewType = RenderViewType::Game, std::uint32_t viewId = std::numeric_limits<std::uint32_t>::max());
	// Page-local stream bytes depend on packet geometry and page bindings, but
	// not on the advancing frame id. This signature lets a frame slot retain its
	// already uploaded buffer until one of those shader-visible inputs changes.
	TERRAENGINE_API std::uint64_t BuildVirtualGeometryPagePayloadReuseSignature(const std::vector<GpuVirtualGeometryPacketRecord>& packets);
	// Reuse is safe only when the prior upload completed into a live buffer and
	// its exact packet signature still matches the current render snapshot.
	TERRAENGINE_API bool CanReuseVirtualGeometryPagePayload(std::uint64_t cachedSignature, std::uint64_t currentSignature, bool cacheValid, bool bufferAvailable);
	// ABI-only compatibility entry: without persistent owner state this overload
	// always returns an invalid plan. Migrate to the stateful overload below,
	// retaining one allocator per actual pool and committing at its protected
	// boundary; a fresh allocator per call cannot preserve allocation identity.
	[[deprecated("Always rejects: pass persistent pool-owner allocator state to the stateful overload and commit the candidate at the protected publication boundary.")]]
	TERRAENGINE_API PersistentPhysicalPageUploadPlan
		BuildPersistentPhysicalPageUploadPlan(
			std::vector<PersistentPhysicalPageUploadInput> inputs,
			std::uint64_t poolCapacityBytes,
			std::uint64_t uploadBudgetBytes,
			std::uint64_t allocationGeneration);
	// Worker preparation reads an immutable state snapshot. Only the pool owner
	// commits at its existing protected publication boundary; no new flush occurs.
	// completionFence inputs identify submitted last use; completedFence is the
	// owner's proven completed fence. Every live owner/fence update goes through
	// revision-advancing commit, and reuse retains the maximum prior use fence.
	TERRAENGINE_API PersistentPhysicalPageUploadPlan BuildPersistentPhysicalPageUploadPlan(
		std::vector<PersistentPhysicalPageUploadInput> inputs, std::uint64_t poolCapacityBytes,
		std::uint64_t uploadBudgetBytes, std::uint64_t allocationGeneration,
		const PersistentPhysicalPageAllocationState& observedAllocator,
		std::span<const std::uint64_t> retirementAllocations = {}, std::uint64_t completedFence = 0);
	// Requires exclusive pool-owner access. Rejects a stale parallel plan without
	// changing live state; success adopts candidate storage using noexcept swaps.
	TERRAENGINE_API bool CommitPersistentPhysicalPageUploadPlan(
		PersistentPhysicalPageUploadPlan& plan, PersistentPhysicalPageAllocationState& allocator);
	TERRAENGINE_API VirtualGeometryUploadIntentPlan BuildVirtualGeometryUploadIntentPlan(
		std::vector<VirtualGeometryPageUploadIntent> intents,
		std::vector<std::uint64_t> retirementAllocations);

}
