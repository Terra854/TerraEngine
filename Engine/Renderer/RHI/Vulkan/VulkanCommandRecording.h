// VulkanCommandRecording.h - backend-local translation helpers.
//
// Renderer graph headers stay backend-neutral. This file is the Vulkan side of
// the boundary where neutral resource states can be translated into backend
// decisions without leaking Vulkan names into RenderSystem.

#pragma once

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderWorkPacket.h"
#include "VulkanCommonIncludes.h"

#include <array>
#include <cstdint>
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

	// A single animation generation is recorded once before all view draw work.
	// This value plan deliberately owns neither command buffers nor queue submits;
	// VulkanContext folds it into the frame's existing primary submission.
	struct VulkanAnimationBarrierPlan
	{
		std::uint32_t afterStage = 0U;
		std::uint32_t flags = 0U;
	};

	struct VulkanAnimationCommandPlan
	{
		std::uint32_t planGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::vector<Animation::GpuAnimationDispatchCommand> dispatches;
		std::vector<VulkanAnimationBarrierPlan> barriers;
		bool hasWork = false;
		bool valid = false;
		std::string rejectionReason;
	};

	enum class VulkanAnimationRecordingStatus : std::uint8_t
	{
		Recorded,
		NoWork,
		InvalidPlan,
		MissingPipeline,
		MissingDescriptorSet
	};

	struct VulkanAnimationComputePipelines
	{
		VkPipelineLayout layout = VK_NULL_HANDLE;
		std::array<VkPipeline, Animation::kGpuAnimationStageCount> byStage{};
	};

	struct VulkanAnimationRecordingResult
	{
		VulkanAnimationRecordingStatus status = VulkanAnimationRecordingStatus::NoWork;
		std::uint32_t recordedDispatchCount = 0U;
		std::uint32_t emittedBarrierCount = 0U;
	};

	TERRAENGINE_API VulkanAnimationCommandPlan BuildVulkanAnimationCommandPlan(
		const Animation::GpuAnimationPlan& plan,
		std::uint32_t rendererGeneration);
	TERRAENGINE_API VulkanAnimationRecordingResult RecordVulkanAnimationCommands(
		VkCommandBuffer commandBuffer,
		const VulkanAnimationCommandPlan& plan,
		const VulkanAnimationComputePipelines& pipelines,
		VkDescriptorSet descriptorSet);

}
