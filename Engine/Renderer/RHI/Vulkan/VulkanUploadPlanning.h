// VulkanUploadPlanning.h - immutable upload preparation records.

#pragma once

#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/VirtualGeometry/Selection/RenderViewSelectionData.h"
#include "Core/Export.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

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
		std::uint64_t poolByteOffset = 0u;
		std::uint64_t allocationId = 0u;
		bool reuseExisting = false;
	};

	struct PersistentPhysicalPageUploadPlan
	{
		std::vector<PersistentPhysicalPageUpload> uploads;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t committedBytes = 0u;
		std::uint32_t reusedPages = 0u;
		bool valid = false;
		std::string rejectionReason;
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
	// Produces one checked persistent pool allocation per exact page generation.
	// Existing allocations are reused and therefore consume no upload budget.
	TERRAENGINE_API PersistentPhysicalPageUploadPlan
		BuildPersistentPhysicalPageUploadPlan(
			std::vector<PersistentPhysicalPageUploadInput> inputs,
			std::uint64_t poolCapacityBytes,
			std::uint64_t uploadBudgetBytes,
			std::uint64_t allocationGeneration);
	TERRAENGINE_API VirtualGeometryUploadIntentPlan BuildVirtualGeometryUploadIntentPlan(
		std::vector<VirtualGeometryPageUploadIntent> intents,
		std::vector<std::uint64_t> retirementAllocations);

}
