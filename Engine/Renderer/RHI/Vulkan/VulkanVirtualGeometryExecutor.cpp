// VulkanVirtualGeometryExecutor.cpp - bounded dispatch recording and barriers.

#include "VulkanVirtualGeometryExecutor.h"

#include <algorithm>
#include <array>
#include <limits>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		void RecordVirtualGeometryBarrier(VkCommandBuffer commandBuffer, VulkanVirtualGeometryBarrierKind kind)
		{
			VkMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			VkPipelineStageFlags source = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			VkPipelineStageFlags destination = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			if(kind == VulkanVirtualGeometryBarrierKind::UploadToTraversal)
			{
				source = VK_PIPELINE_STAGE_HOST_BIT;
				barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
			}
			if (kind == VulkanVirtualGeometryBarrierKind::ComputeToDraw)
			{
				destination = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
				barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
			}
			else if (kind == VulkanVirtualGeometryBarrierKind::CounterToReadback)
			{
				destination = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
			}
			vkCmdPipelineBarrier(
				commandBuffer, source, destination, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
		}
	} // namespace

	VulkanVirtualGeometryExecutorPlan BuildVulkanVirtualGeometryExecutorPlan(std::uint32_t requestedPacketCount,
		std::uint32_t maximumPacketCount,
		bool usesIndirectCount,
		const VulkanVirtualGeometryExecutorResources &resources,
		const VulkanVirtualGeometryExecutorPipelines &pipelines,
		std::uint64_t frameId,
		std::uint64_t geometryGeneration,
		std::uint64_t residencyGeneration,
		std::uint64_t commandGeneration)
	{
		VulkanVirtualGeometryExecutorPlan plan;
		plan.frameId = frameId;
		plan.geometryGeneration = geometryGeneration;
		plan.residencyGeneration = residencyGeneration;
		plan.commandGeneration = commandGeneration;
		plan.requestedPacketCount = requestedPacketCount;
		plan.maximumPacketCount = maximumPacketCount;
		plan.usesIndirectCount = usesIndirectCount;
		if (maximumPacketCount == 0u || frameId == 0u || geometryGeneration == 0u || commandGeneration == 0u)
		{
			plan.unavailableReason = "Vulkan virtual geometry requires bounded packets and non-zero frame/generation identity.";
			return plan;
		}
		plan.boundedPacketCount = std::min(requestedPacketCount, maximumPacketCount);
		plan.zeroWork = plan.boundedPacketCount == 0u;
		plan.resourcesReady = resources.resourcesOwnedByFrame && resources.packetBuffer != VK_NULL_HANDLE &&
							  resources.traversalInstanceBuffer != VK_NULL_HANDLE && resources.traversalRequestBuffer != VK_NULL_HANDLE &&
							  resources.traversalCounterBuffer != VK_NULL_HANDLE &&
							  resources.compactedPacketBuffer != VK_NULL_HANDLE && resources.generatedSubmissionBuffer != VK_NULL_HANDLE &&
							  resources.counterReadbackBuffer != VK_NULL_HANDLE && resources.zeroWorkIndirectBuffer != VK_NULL_HANDLE;
		plan.descriptorReady = resources.descriptorSet != VK_NULL_HANDLE;
		// All three pipelines are required because a generated draw is valid only
		// after traversal, compaction, and indirect generation record as one chain.
		plan.pipelinesReady = pipelines.layout != VK_NULL_HANDLE && pipelines.traversal != VK_NULL_HANDLE
			&& pipelines.compaction != VK_NULL_HANDLE && pipelines.indirectGeneration != VK_NULL_HANDLE;
		plan.barriers = { VulkanVirtualGeometryBarrierKind::UploadToTraversal,
			VulkanVirtualGeometryBarrierKind::TraversalToCompaction,
			// Compaction writes the packet range read by indirect generation. Keep
			// this explicit in the immutable plan as well as in the recorder so
			// diagnostics cannot omit a real producer/consumer dependency.
			VulkanVirtualGeometryBarrierKind::CompactionToIndirect,
			VulkanVirtualGeometryBarrierKind::ComputeToDraw,
			VulkanVirtualGeometryBarrierKind::CounterToReadback };
		plan.evidenceId = "vulkan-vg-executor." + std::to_string(frameId) + "." + std::to_string(commandGeneration);
		if (!plan.resourcesReady)
			plan.unavailableReason = "Vulkan virtual-geometry executor has no frame-owned compute/readback buffers.";
		else if (!plan.descriptorReady)
			plan.unavailableReason = "Vulkan virtual-geometry executor has no bound descriptor set.";
		else if (!plan.pipelinesReady)
			plan.unavailableReason = "Vulkan virtual-geometry compaction pipeline is not created.";
		return plan;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryExecutorStage(
		VkCommandBuffer commandBuffer, VulkanVirtualGeometryExecutorStage stage,
		const VulkanVirtualGeometryExecutorPlan &plan,
		const VulkanVirtualGeometryExecutorResources &resources,
		const VulkanVirtualGeometryExecutorPipelines &pipelines)
	{
		VulkanVirtualGeometryExecutorRecordingResult result{};
		result.zeroWork = plan.zeroWork;
		if(commandBuffer == VK_NULL_HANDLE || !plan.resourcesReady || !plan.descriptorReady || !plan.pipelinesReady) {
			result.failureReason = plan.unavailableReason.empty() ? "Vulkan virtual-geometry executor command prerequisites are unavailable." : plan.unavailableReason;
			return result;
		}
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.layout, 0u, 1u, &resources.descriptorSet, 0u, nullptr);
		if(stage == VulkanVirtualGeometryExecutorStage::Traversal) {
			RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::UploadToTraversal);
			const VulkanVirtualGeometryTraversalPushConstants constants{ plan.boundedPacketCount, plan.maximumPacketCount,
				static_cast<std::uint32_t>(plan.commandGeneration), 1u };
			vkCmdPushConstants(commandBuffer, pipelines.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(constants), &constants);
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.traversal);
			if(!plan.zeroWork) vkCmdDispatch(commandBuffer, (plan.boundedPacketCount + 63u) / 64u, 1u, 1u);
			RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::TraversalToCompaction);
			result.recorded = true; result.dispatchCount = plan.zeroWork ? 0u : 1u; result.barrierCount = 2u; return result;
		}
		if(stage == VulkanVirtualGeometryExecutorStage::Compaction) {
			const VulkanVirtualGeometryCompactionPushConstants constants{ plan.boundedPacketCount, plan.maximumPacketCount, 0u, 1u };
			vkCmdPushConstants(commandBuffer, pipelines.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(constants), &constants);
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.compaction);
			if(!plan.zeroWork) vkCmdDispatch(commandBuffer, (plan.boundedPacketCount + 63u) / 64u, 1u, 1u);
			RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::CompactionToIndirect);
			result.recorded = true; result.dispatchCount = plan.zeroWork ? 0u : 1u; result.barrierCount = 1u; return result;
		}
		// Emitted-source identities are enum values. Bits 1, 2, and 3 admit
		// high-detail, fallback, and conservative embedded coverage while
		// explicitly rejecting None, NoSubmittedWork, and Invalid records.
		const VulkanVirtualGeometryIndirectPushConstants constants{
			plan.boundedPacketCount,
			plan.maximumPacketCount,
			16u,
			(1u << 1u) | (1u << 2u) | (1u << 3u) };
		vkCmdPushConstants(commandBuffer, pipelines.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(constants), &constants);
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.indirectGeneration);
		if(plan.zeroWork) vkCmdDispatchIndirect(commandBuffer, resources.zeroWorkIndirectBuffer, 0u);
		else vkCmdDispatch(commandBuffer, (plan.boundedPacketCount + 63u) / 64u, 1u, 1u);
		// Generated uint4 records become indirect mesh-task commands before the
		// main view consumes them; counters remain available for later readback.
		RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::ComputeToDraw);
		RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::CounterToReadback);
		result.recorded = true; result.dispatchCount = 1u; result.barrierCount = 2u; return result;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryExecutor(VkCommandBuffer commandBuffer,
		const VulkanVirtualGeometryExecutorPlan &plan,
		const VulkanVirtualGeometryExecutorResources &resources,
		const VulkanVirtualGeometryExecutorPipelines &pipelines)
	{
		VulkanVirtualGeometryExecutorRecordingResult result;
		result.zeroWork = plan.zeroWork;
		if (commandBuffer == VK_NULL_HANDLE || !plan.resourcesReady || !plan.descriptorReady || !plan.pipelinesReady)
		{
			result.failureReason =
				plan.unavailableReason.empty() ? "Vulkan virtual-geometry executor command prerequisites are unavailable." : plan.unavailableReason;
			return result;
		}
		if (plan.zeroWork)
		{
			// A zero-count frame still owns the counter/readback barrier, but no
			// compute dispatch or graphics submission is recorded for it.
			RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::CounterToReadback);
			result.recorded = true;
			result.barrierCount = 1u;
			return result;
		}
		// CPU-mapped uploads and counter clears precede this command. Make those
		// host writes visible to the shader before it reads or atomically updates
		// any packet/counter storage.
		RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::UploadToTraversal);
		++result.barrierCount;
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.layout, 0u, 1u, &resources.descriptorSet, 0u, nullptr);
		const std::uint32_t groups = (plan.boundedPacketCount + 63u) / 64u;
		const VulkanVirtualGeometryCompactionPushConstants constants{
			plan.boundedPacketCount,
			plan.maximumPacketCount,
			0u,
			1u };
		vkCmdPushConstants(commandBuffer, pipelines.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(constants), &constants);
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.compaction);
		vkCmdDispatch(commandBuffer, groups, 1u, 1u);
		++result.dispatchCount;
		// This legacy all-at-once helper records compaction only. Callers must use
		// the stage recorder before enabling generated indirect consumption.
		RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::TraversalToCompaction);
		++result.barrierCount;
		RecordVirtualGeometryBarrier(commandBuffer, VulkanVirtualGeometryBarrierKind::CounterToReadback);
		++result.barrierCount;
		result.recorded = true;
		return result;
	}

	VulkanVirtualGeometryFailureReceipt BuildVulkanVirtualGeometryFailureReceipt(
		const VulkanVirtualGeometryExecutorPlan &plan, VirtualGeometryBackendStage stage)
	{
		return { stage,
			plan.frameId,
			plan.commandGeneration,
			plan.evidenceId,
			plan.unavailableReason.empty() ? "Vulkan virtual-geometry executor did not submit a command." : plan.unavailableReason };
	}

	VulkanVirtualGeometryVisibilityPlan BuildVulkanVirtualGeometryVisibilityPlan(
		std::uint32_t requestedPrimitiveCount, std::uint32_t maximumPrimitiveCount,
		std::uint32_t targetWidth, std::uint32_t targetHeight,
		std::uint32_t materialBinCount,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines,
		std::uint64_t frameId, std::uint64_t geometryGeneration)
	{
		VulkanVirtualGeometryVisibilityPlan plan;
		plan.frameId = frameId; plan.geometryGeneration = geometryGeneration;
		plan.targetWidth = targetWidth;
		plan.targetHeight = targetHeight;
		plan.materialBinCount = materialBinCount;
		const std::uint64_t widePixelCount =
			static_cast<std::uint64_t>(targetWidth)
			* static_cast<std::uint64_t>(targetHeight);
		if(widePixelCount <= std::numeric_limits<std::uint32_t>::max())
		{
			plan.pixelCount = static_cast<std::uint32_t>(widePixelCount);
		}
		if(maximumPrimitiveCount == 0u || frameId == 0u
			|| geometryGeneration == 0u || plan.pixelCount == 0u
			|| materialBinCount == 0u)
		{
			plan.unavailableReason = "Vulkan visibility requires bounded primitives, pixels, material bins, and non-zero frame/generation identity.";
			return plan;
		}
		plan.boundedPrimitiveCount = std::min(requestedPrimitiveCount, maximumPrimitiveCount);
		plan.zeroWork = plan.boundedPrimitiveCount == 0u
			|| plan.pixelCount == 0u;
		const bool resourcesReady = resources.resourcesOwnedByFrame && resources.inputTriangleBuffer != VK_NULL_HANDLE
			&& resources.depthBuffer != VK_NULL_HANDLE && resources.visibilityRecordBuffer != VK_NULL_HANDLE
			&& resources.resolvedVisibilityBuffer != VK_NULL_HANDLE && resources.shadingBinBuffer != VK_NULL_HANDLE
			&& resources.diagnosticBuffer != VK_NULL_HANDLE && resources.computeRasterDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityResolveDescriptorSet != VK_NULL_HANDLE && resources.shadingBinsDescriptorSet != VK_NULL_HANDLE;
		const bool pipelinesReady = pipelines.layout != VK_NULL_HANDLE && pipelines.computeRaster != VK_NULL_HANDLE
			&& pipelines.visibilityResolve != VK_NULL_HANDLE && pipelines.shadingBins != VK_NULL_HANDLE;
		plan.ready = resourcesReady && pipelinesReady;
		plan.evidenceId = "vulkan-vg-visibility." + std::to_string(frameId) + "." + std::to_string(geometryGeneration);
		if(!resourcesReady) plan.unavailableReason = "Vulkan visibility frame buffers or pass descriptors are unavailable.";
		else if(!pipelinesReady) plan.unavailableReason = "Vulkan visibility compute pipelines are unavailable.";
		return plan;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryVisibility(
		VkCommandBuffer commandBuffer, const VulkanVirtualGeometryVisibilityPlan& plan,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines)
	{
		VulkanVirtualGeometryExecutorRecordingResult result{}; result.zeroWork = plan.zeroWork;
		if(commandBuffer == VK_NULL_HANDLE || !plan.ready)
		{
			result.failureReason = plan.unavailableReason.empty() ? "Vulkan visibility prerequisites are unavailable." : plan.unavailableReason;
			return result;
		}
		const auto RecordStage = [&](VulkanVirtualGeometryVisibilityStage stage) {
			return RecordVulkanVirtualGeometryVisibilityStage(commandBuffer, stage, plan, resources, pipelines);
		};
		const std::array<VulkanVirtualGeometryVisibilityStage, 3> stages{
			VulkanVirtualGeometryVisibilityStage::ComputeRaster,
			VulkanVirtualGeometryVisibilityStage::VisibilityResolve,
			VulkanVirtualGeometryVisibilityStage::ShadingBins };
		for(std::size_t stageIndex = 0u; stageIndex < stages.size(); ++stageIndex) {
			const VulkanVirtualGeometryExecutorRecordingResult stageResult = RecordStage(stages[stageIndex]);
			if(!stageResult.recorded) return stageResult;
			result.dispatchCount += stageResult.dispatchCount;
			if(!plan.zeroWork && stageIndex + 1u < stages.size()) {
				VkMemoryBarrier barrier{};
				barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
				barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
				vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
				++result.barrierCount;
			}
		}
		result.recorded = true;
		return result;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryVisibilityStage(
		VkCommandBuffer commandBuffer, VulkanVirtualGeometryVisibilityStage stage,
		const VulkanVirtualGeometryVisibilityPlan& plan,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines)
	{
		VulkanVirtualGeometryExecutorRecordingResult result{};
		result.zeroWork = plan.zeroWork;
		if(commandBuffer == VK_NULL_HANDLE || !plan.ready) {
			result.failureReason = plan.unavailableReason.empty() ? "Vulkan visibility prerequisites are unavailable." : plan.unavailableReason;
			return result;
		}
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		std::array<std::uint32_t, 6> constants{};
		std::uint32_t pushConstantBytes = 16u;
		std::uint32_t dispatchItemCount = plan.pixelCount;
		switch(stage) {
		case VulkanVirtualGeometryVisibilityStage::ComputeRaster:
			descriptorSet = resources.computeRasterDescriptorSet;
			pipeline = pipelines.computeRaster;
			constants = { plan.boundedPrimitiveCount, plan.pixelCount,
				static_cast<std::uint32_t>(plan.geometryGeneration), 1u,
				plan.targetWidth, plan.targetHeight };
			pushConstantBytes = 24u;
			dispatchItemCount = plan.boundedPrimitiveCount;
			break;
		case VulkanVirtualGeometryVisibilityStage::VisibilityResolve:
			descriptorSet = resources.visibilityResolveDescriptorSet;
			pipeline = pipelines.visibilityResolve;
			// Resolve rejects stale records by requiring the same generation on both sides.
			constants = { plan.pixelCount, plan.pixelCount,
				static_cast<std::uint32_t>(plan.geometryGeneration),
				static_cast<std::uint32_t>(plan.geometryGeneration),
				plan.boundedPrimitiveCount, 0u };
			pushConstantBytes = 24u;
			break;
		case VulkanVirtualGeometryVisibilityStage::ShadingBins:
			descriptorSet = resources.shadingBinsDescriptorSet;
			pipeline = pipelines.shadingBins;
			constants = { plan.pixelCount, plan.pixelCount,
				plan.materialBinCount, 0u, 0u, 0u };
			break;
		}
		if(descriptorSet == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE) {
			result.failureReason = "Vulkan visibility pass descriptor set or pipeline is unavailable.";
			return result;
		}
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.layout, 0u, 1u, &descriptorSet, 0u, nullptr);
		if(!plan.zeroWork) {
			vkCmdPushConstants(commandBuffer, pipelines.layout,
				VK_SHADER_STAGE_COMPUTE_BIT, 0u, pushConstantBytes,
				constants.data());
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdDispatch(commandBuffer,
				(dispatchItemCount + 63u) / 64u, 1u, 1u);
			result.dispatchCount = 1u;
		}
		result.recorded = true;
		return result;
	}
} // namespace TerraEngine::Renderer::Vulkan
