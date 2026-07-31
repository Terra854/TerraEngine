// VulkanCommandRecording.h - backend-local translation helpers.
//
// Renderer graph headers stay backend-neutral. This file is the Vulkan side of
// the boundary where neutral resource states can be translated into backend
// decisions without leaking Vulkan names into RenderSystem.

#pragma once

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderWorkPacket.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "VulkanCommonIncludes.h"
#include "VulkanVirtualGeometryExecutor.h"

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

	// This value plan bridges immutable renderer preparation to Vulkan's existing
	// executor. It owns no VkBuffer or descriptor lifetime: VulkanContext remains
	// the protected owner of those objects and invokes the recorder below once.
	struct VulkanVirtualGeometryCommandPlan
	{
		VulkanVirtualGeometryExecutorPlan executorPlan;
		std::vector<VulkanVirtualGeometryExecutorStage> stages;
		bool protectedRecordingRequired = true;
		bool valid = false;
		std::string rejectionReason;
	};

	TERRAENGINE_API VulkanVirtualGeometryCommandPlan BuildVulkanVirtualGeometryCommandPlan(
		const VirtualGeometryGpuPlan& immutablePlan,
		const VulkanVirtualGeometryExecutorResources& resources,
		const VulkanVirtualGeometryExecutorPipelines& pipelines);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryCommands(
		VkCommandBuffer commandBuffer,
		const VulkanVirtualGeometryCommandPlan& commandPlan,
		const VulkanVirtualGeometryExecutorResources& resources,
		const VulkanVirtualGeometryExecutorPipelines& pipelines);

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

	enum class VulkanDeformationBarrierKind : std::uint8_t
	{
		AnimationOrUploadToDeform,
		DeformToBounds,
		BoundsToRefinement,
		RefinementToGraphics
	};

	struct VulkanDeformationDispatch
	{
		std::uint32_t workRecordIndex = 0u;
		std::uint32_t groupCountX = 0u;
	};

	struct VulkanDeformationCommandPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::vector<VulkanDeformationDispatch> deformationDispatches;
		std::vector<VulkanDeformationBarrierKind> barriers;
		bool hasWork = false;
		bool valid = false;
		std::string rejectionReason;
	};

	enum class VulkanDeformationPipelineStage : std::uint8_t
	{
		Deform,
		Bounds,
		Refinement,
		Count
	};

	struct VulkanDeformationComputePipelines
	{
		VkPipelineLayout layout = VK_NULL_HANDLE;
		std::array<VkPipeline,
			static_cast<std::size_t>(
				VulkanDeformationPipelineStage::Count)> byStage{};
	};

	struct VulkanDeformationRecordingResult
	{
		bool recorded = false;
		std::uint32_t deformationDispatchCount = 0u;
		std::uint32_t boundsDispatchCount = 0u;
		std::uint32_t refinementDispatchCount = 0u;
		std::uint32_t barrierCount = 0u;
	};

	TERRAENGINE_API VulkanDeformationCommandPlan BuildVulkanDeformationCommandPlan(
		const GpuDeformationPlan& plan);
	TERRAENGINE_API VulkanDeformationRecordingResult
		RecordVulkanDeformationCommands(
			VkCommandBuffer commandBuffer,
			const VulkanDeformationCommandPlan& commandPlan,
			const GpuDeformationPlan& immutablePlan,
			const VulkanDeformationComputePipelines& pipelines,
			VkDescriptorSet descriptorSet);

}
