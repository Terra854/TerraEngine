// VulkanCommandRecording.h - backend-local translation helpers.
//
// Renderer graph headers stay backend-neutral. This file is the Vulkan side of
// the boundary where neutral resource states can be translated into backend
// decisions without leaking Vulkan names into RenderSystem.

#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderWorkPacket.h"

#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	TERRAENGINE_API std::string DescribeBackendLocalState(RenderAccessMode accessMode, RenderStage stage);
	TERRAENGINE_API BackendRecordingBatchPlan BuildVulkanRecordingBatchPlan(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const BackendCapabilityProfile& capabilities,
		RenderBackendWorkKind workKind);

	struct VulkanRendererThreadingStage
	{
		std::string stageId;
		RenderBackendWorkKind workKind = RenderBackendWorkKind::Geometry;
		bool workerEligible = false;
		bool coordinatorOwned = false;
		std::string coordinatorReason;
	};

	struct VulkanRendererThreadingPlan
	{
		std::uint64_t frameId = 0u;
		std::vector<VulkanRendererThreadingStage> stages;
		std::size_t workerEligibleStageCount = 0u;
		std::size_t coordinatorStageCount = 0u;
	};

	TERRAENGINE_API VulkanRendererThreadingPlan BuildVulkanRendererThreadingPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities);

}
