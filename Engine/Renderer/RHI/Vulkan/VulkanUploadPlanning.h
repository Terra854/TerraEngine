// VulkanUploadPlanning.h - immutable upload preparation records.

#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Core/Export.h"

#include <cstdint>
#include <limits>
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

	TERRAENGINE_API UploadPreparationPlan BuildUploadPreparationPlan(const RenderFrameSnapshot& snapshot, std::size_t payloadBudget = 64u);
	TERRAENGINE_API VirtualGeometryPageUploadPublication PlanVirtualGeometryPageUploadPublication(const GeometryPageRuntime& page);
	TERRAENGINE_API VirtualGeometryPageRetirementPlan PlanVirtualGeometryPageRetirement(const GeometryPageRuntime& page, std::uint64_t frameId, std::uint64_t safeFrameLag = 2u);
	TERRAENGINE_API std::vector<VirtualGeometryPagePayloadUploadRecord> BuildVirtualGeometryPagePayloadUploadPlan(const RenderFrameSnapshot& snapshot, RenderViewType viewType = RenderViewType::Game, std::uint32_t viewId = std::numeric_limits<std::uint32_t>::max());

}
