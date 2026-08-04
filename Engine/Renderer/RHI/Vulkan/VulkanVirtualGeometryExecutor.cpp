// VulkanVirtualGeometryExecutor.cpp - bounded dispatch recording and barriers.

#include "VulkanVirtualGeometryExecutor.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <algorithm>
#include <array>
#include <limits>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		// The visibility-only layout exposes one overlapping push-constant range to
		// compute raster and fragment compositing. Updates through that layout must
		// therefore name both consumers; preparation layouts remain compute-only.
		constexpr VkShaderStageFlags kVirtualGeometryPushConstantStages =
			VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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

		void RecordProductionDecodeReadbackBarrier(VkCommandBuffer commandBuffer)
		{
			// The decode shader writes both bytes and a compact result row. The
			// protected owner maps the readback buffer only after this boundary and
			// its frame fence, never while a worker is still recording commands.
			VkMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
				0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
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

	VulkanVirtualGeometryVisibilityClearPlan
		BuildVulkanVirtualGeometryVisibilityClearPlan(
			const VulkanVirtualGeometryVisibilityPlan& plan)
	{
		VulkanVirtualGeometryVisibilityClearPlan clearPlan;
		if(!plan.ready || plan.zeroWork || plan.pixelCount == 0u
			|| plan.materialBinCount == 0u)
		{
			return clearPlan;
		}
		clearPlan.depthBytes = static_cast<VkDeviceSize>(plan.pixelCount)
			* sizeof(std::uint32_t);
		clearPlan.visibilityRecordBytes =
			static_cast<VkDeviceSize>(plan.pixelCount) * 4u
			* sizeof(std::uint32_t);
		clearPlan.resolvedVisibilityBytes =
			clearPlan.visibilityRecordBytes;
		clearPlan.shadingBinBytes =
			static_cast<VkDeviceSize>(plan.materialBinCount)
			* sizeof(std::uint32_t);
		clearPlan.valid = true;
		return clearPlan;
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

	VulkanDirectMeshletBatchPlan BuildVulkanDirectMeshletBatchPlan(
		const std::vector<DirectMeshletBatch>& batches, bool meshTaskPathAvailable,
		std::uint32_t meshletsPerTaskGroup)
	{
		VulkanDirectMeshletBatchPlan plan{};
		if(!meshTaskPathAvailable || meshletsPerTaskGroup == 0u)
		{
			plan.rejectionReason = "The Vulkan mesh-task path is unavailable for direct meshlet batches.";
			return plan;
		}
		for(const DirectMeshletBatch& batch : batches)
		{
			if(batch.geometryIdentity == 0u || batch.geometryGeneration == 0u
				|| batch.meshletCount == 0u || batch.instanceIdentities.empty())
			{
				plan.draws.clear();
				plan.rejectionReason = "A direct meshlet batch lacks a stable geometry identity, generation, meshlet range, or instance set.";
				return plan;
			}
			if(batch.meshletCount > std::numeric_limits<std::uint32_t>::max()
				- (meshletsPerTaskGroup - 1u))
			{
				plan.draws.clear();
				plan.rejectionReason = "A direct meshlet batch overflows task-group rounding.";
				return plan;
			}
			VulkanDirectMeshletDrawRecord draw{};
			draw.geometryIdentity = batch.geometryIdentity;
			draw.geometryGeneration = batch.geometryGeneration;
			draw.materialIdentity = batch.materialIdentity;
			draw.taskGroupCount = (batch.meshletCount + meshletsPerTaskGroup - 1u) / meshletsPerTaskGroup;
			draw.instanceCount = static_cast<std::uint32_t>(batch.instanceIdentities.size());
			plan.draws.push_back(draw);
		}
		// Every draw here is direct by construction. These counters make a route
		// audit explicit: recording this plan cannot create a VG executor or page
		// feedback request, even when the same frame has other VG draws.
		plan.suppressedVirtualGeometryExecutors = static_cast<std::uint32_t>(plan.draws.size());
		plan.suppressedPageRequests = static_cast<std::uint32_t>(plan.draws.size());
		plan.valid = true;
		return plan;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanDirectMeshletBatches(
		VkCommandBuffer commandBuffer, const VulkanDirectMeshletBatchPlan& plan,
		VkBuffer existingIndirectBuffer, VkDeviceSize firstIndirectByteOffset)
	{
		VulkanVirtualGeometryExecutorRecordingResult result{};
		if(!plan.valid || commandBuffer == VK_NULL_HANDLE || existingIndirectBuffer == VK_NULL_HANDLE)
		{
			result.failureReason = plan.rejectionReason.empty()
				? "Direct meshlet batches require a valid plan, command buffer, and existing indirect mesh-task buffer."
				: plan.rejectionReason;
			return result;
		}
		for(std::size_t index = 0u; index < plan.draws.size(); ++index)
		{
			// VulkanContext owns and fills this ordinary frame indirect buffer. Reusing
			// it keeps direct route submission on the established mesh-task path and
			// avoids allocating any virtual-geometry executor resource.
			const VkDeviceSize offset = firstIndirectByteOffset
				+ static_cast<VkDeviceSize>(index) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
			vkCmdDrawMeshTasksIndirectEXT(commandBuffer, existingIndirectBuffer, offset, 1u,
				sizeof(VkDrawMeshTasksIndirectCommandEXT));
			++result.dispatchCount;
		}
		result.recorded = true;
		return result;
	}

	VulkanVirtualGeometryProductionDecodePlan BuildVulkanVirtualGeometryProductionDecodePlan(
		VulkanVirtualGeometryProductionDecodeRequest request, std::uint64_t generation,
		std::uint64_t expectedDecodedHash,
		const VulkanVirtualGeometryProductionDecodeResources& resources,
		const VulkanVirtualGeometryProductionDecodePipelines& pipelines)
	{
		constexpr std::uint32_t kExpandedProductionProfile = 1u;
		constexpr std::uint32_t kMaximumDecodeBytes = 262144u;
		VulkanVirtualGeometryProductionDecodePlan plan{};
		plan.request = request;
		plan.generation = generation;
		plan.expectedDecodedHash = expectedDecodedHash;
		plan.supportedProfile = request.profileId == kExpandedProductionProfile;
		if(!plan.supportedProfile)
		{
			plan.unavailableReason = "Only ExpandedProduction has a Vulkan decode kernel; meshoptimizer profiles remain CPU fallback only.";
			return plan;
		}
		const bool invalidRange = generation == 0u || expectedDecodedHash == 0u
			|| request.codecVersion == 0u || request.requestIdentity == 0u
			|| request.encodedByteCount != request.decodedByteCount
			|| request.decodedByteCount == 0u || request.decodedByteCount > kMaximumDecodeBytes
			|| (request.decodedByteCount & 3u) != 0u
			|| request.encodedByteOffset > resources.encodedCapacityBytes
			|| request.encodedByteCount > resources.encodedCapacityBytes - request.encodedByteOffset
			|| request.decodedByteOffset > resources.decodedCapacityBytes
			|| request.decodedByteCount > resources.decodedCapacityBytes - request.decodedByteOffset
			|| request.decodedByteCount > request.outputCapacityBytes;
		if(invalidRange)
		{
			plan.unavailableReason = "ExpandedProduction decode request violates the shared bounded ABI.";
			return plan;
		}
		plan.resourcesReady = resources.resourcesOwnedByFrame
			&& resources.requestBuffer != VK_NULL_HANDLE && resources.encodedBuffer != VK_NULL_HANDLE
			&& resources.decodedBuffer != VK_NULL_HANDLE && resources.resultReadbackBuffer != VK_NULL_HANDLE
			&& resources.descriptorSet != VK_NULL_HANDLE;
		plan.pipelineReady = pipelines.layout != VK_NULL_HANDLE
			&& pipelines.expandedProductionDecode != VK_NULL_HANDLE;
		if(!plan.resourcesReady)
			plan.unavailableReason = "ExpandedProduction decode has no frame-owned descriptor/readback resources.";
		else if(!plan.pipelineReady)
			plan.unavailableReason = "ExpandedProduction decode pipeline has not been created.";
		else
			plan.valid = true;
		return plan;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryProductionDecode(
		VkCommandBuffer commandBuffer, const VulkanVirtualGeometryProductionDecodePlan& plan,
		const VulkanVirtualGeometryProductionDecodeResources& resources,
		const VulkanVirtualGeometryProductionDecodePipelines& pipelines)
	{
		VulkanVirtualGeometryExecutorRecordingResult result{};
		if(!plan.valid || commandBuffer == VK_NULL_HANDLE)
		{
			result.failureReason = plan.unavailableReason.empty()
				? "ExpandedProduction decode plan is not ready for protected Vulkan recording."
				: plan.unavailableReason;
			return result;
		}
		const std::array<std::uint32_t, 4u> constants{
			// Each protected record call submits one request row, so its descriptor
			// slice begins at zero. requestIdentity stays an opaque parity token.
			0u, resources.encodedCapacityBytes,
			resources.decodedCapacityBytes, 0u };
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.layout,
			0u, 1u, &resources.descriptorSet, 0u, nullptr);
		vkCmdPushConstants(commandBuffer, pipelines.layout, VK_SHADER_STAGE_COMPUTE_BIT,
			0u, sizeof(constants), constants.data());
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipelines.expandedProductionDecode);
		vkCmdDispatch(commandBuffer, 1u, 1u, 1u);
		RecordProductionDecodeReadbackBarrier(commandBuffer);
		result.recorded = true;
		result.dispatchCount = 1u;
		result.barrierCount = 1u;
		return result;
	}

	VulkanVirtualGeometryProductionDecodeParityReceipt VerifyVulkanVirtualGeometryProductionDecodeParity(
		const VulkanVirtualGeometryProductionDecodePlan& plan,
		const VulkanVirtualGeometryProductionDecodeResult& deviceResult,
		std::span<const std::uint8_t> readbackBytes)
	{
		constexpr std::uint32_t kDecodeComplete = 1u;
		VulkanVirtualGeometryProductionDecodeParityReceipt receipt{};
		receipt.generation = plan.generation;
		if(!plan.valid || deviceResult.requestIdentity != plan.request.requestIdentity
			|| deviceResult.status != kDecodeComplete
			|| deviceResult.decodedByteCount != plan.request.decodedByteCount
			|| readbackBytes.size() != plan.request.decodedByteCount)
		{
			receipt.reason = "Device decode did not complete with the requested identity and byte count; retain the prior CPU-published generation.";
			return receipt;
		}
		if(CalculateVirtualGeometryCompactHash(readbackBytes) != plan.expectedDecodedHash)
		{
			receipt.reason = "Device decode bytes disagree with the CPU reference hash; retain the prior CPU-published generation.";
			return receipt;
		}
		receipt.deviceDecodeAvailable = true;
		receipt.acceptedForPublication = true;
		receipt.reason = "ExpandedProduction Vulkan decode completed and matched the CPU reference hash.";
		return receipt;
	}

	VulkanVirtualGeometryVisibilityPlan BuildVulkanVirtualGeometryVisibilityPlan(
		std::uint32_t requestedPrimitiveCount, std::uint32_t maximumPrimitiveCount,
		std::uint32_t meshletWorkCount,
		std::uint32_t targetWidth, std::uint32_t targetHeight,
		std::uint32_t materialBinCount,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines,
		std::uint64_t frameId, std::uint64_t geometryGeneration)
	{
		VulkanVirtualGeometryVisibilityPlan plan;
		plan.frameId = frameId; plan.geometryGeneration = geometryGeneration;
		plan.meshletWorkCount = meshletWorkCount;
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
			|| plan.meshletWorkCount == 0u
			|| plan.pixelCount == 0u;
		const bool resourcesReady = resources.resourcesOwnedByFrame
			&& resources.inputTriangleBuffer != VK_NULL_HANDLE
			&& resources.setupPacketBuffer != VK_NULL_HANDLE
			&& resources.setupMeshletBuffer != VK_NULL_HANDLE
			&& resources.pagePayloadBuffer != VK_NULL_HANDLE
			&& resources.depthBuffer != VK_NULL_HANDLE && resources.visibilityRecordBuffer != VK_NULL_HANDLE
			&& resources.resolvedVisibilityBuffer != VK_NULL_HANDLE && resources.shadingBinBuffer != VK_NULL_HANDLE
			&& resources.diagnosticBuffer != VK_NULL_HANDLE
			&& resources.bindlessDescriptorSet != VK_NULL_HANDLE
			&& resources.triangleSetupDescriptorSet != VK_NULL_HANDLE
			&& resources.computeRasterDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityResolveDescriptorSet != VK_NULL_HANDLE && resources.shadingBinsDescriptorSet != VK_NULL_HANDLE;
		const bool pipelinesReady = pipelines.layout != VK_NULL_HANDLE
			&& pipelines.triangleSetup != VK_NULL_HANDLE
			&& pipelines.computeRaster != VK_NULL_HANDLE
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
		const std::array<VulkanVirtualGeometryVisibilityStage, 4> stages{
			VulkanVirtualGeometryVisibilityStage::TriangleSetup,
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
		case VulkanVirtualGeometryVisibilityStage::TriangleSetup:
			descriptorSet = resources.triangleSetupDescriptorSet;
			pipeline = pipelines.triangleSetup;
			constants = { plan.meshletWorkCount, plan.boundedPrimitiveCount,
				static_cast<std::uint32_t>(plan.geometryGeneration), 0u, 0u, 0u };
			dispatchItemCount = plan.meshletWorkCount;
			break;
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
		if(stage == VulkanVirtualGeometryVisibilityStage::ComputeRaster
			&& !plan.zeroWork)
		{
			const VulkanVirtualGeometryVisibilityClearPlan clearPlan =
				BuildVulkanVirtualGeometryVisibilityClearPlan(plan);
			if(!clearPlan.valid)
			{
				result.failureReason =
					"Vulkan visibility scratch clear plan is invalid.";
				return result;
			}
			// Compute visibility consumes viewport-sized scratch ranges. Clear them
			// in device command order so the CPU never constructs or copies tens of
			// megabytes of sentinel arrays for each frame.
			vkCmdFillBuffer(commandBuffer, resources.depthBuffer, 0u,
				clearPlan.depthBytes,
				std::numeric_limits<std::uint32_t>::max());
			vkCmdFillBuffer(commandBuffer, resources.visibilityRecordBuffer, 0u,
				clearPlan.visibilityRecordBytes,
				std::numeric_limits<std::uint32_t>::max());
			vkCmdFillBuffer(commandBuffer, resources.resolvedVisibilityBuffer, 0u,
				clearPlan.resolvedVisibilityBytes,
				std::numeric_limits<std::uint32_t>::max());
			vkCmdFillBuffer(commandBuffer, resources.shadingBinBuffer, 0u,
				clearPlan.shadingBinBytes, 0u);

			VkMemoryBarrier clearBarrier{};
			clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			clearBarrier.dstAccessMask =
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0u, 1u, &clearBarrier, 0u, nullptr, 0u, nullptr);
			++result.barrierCount;
		}
		// Set 0 is the shared bindless heap used by the production composite;
		// compute visibility stages use their pass-local storage buffers at set 1.
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipelines.layout, 0u, 1u, &resources.bindlessDescriptorSet, 0u, nullptr);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.layout, 1u, 1u, &descriptorSet, 0u, nullptr);
		if(!plan.zeroWork) {
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			const std::uint32_t phaseCount = stage
				== VulkanVirtualGeometryVisibilityStage::ComputeRaster
				? 3u : 1u;
			for(std::uint32_t phase = 0u; phase < phaseCount; ++phase)
			{
				if(stage
					== VulkanVirtualGeometryVisibilityStage::ComputeRaster)
				{
					constants[3] = phase;
				}
				vkCmdPushConstants(commandBuffer, pipelines.layout,
					kVirtualGeometryPushConstantStages, 0u, pushConstantBytes,
					constants.data());
				vkCmdDispatch(commandBuffer,
					stage == VulkanVirtualGeometryVisibilityStage::TriangleSetup
						? dispatchItemCount
						: (dispatchItemCount + 63u) / 64u,
					1u, 1u);
				++result.dispatchCount;
				if(phase + 1u < phaseCount)
				{
					VkMemoryBarrier phaseBarrier{};
					phaseBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
					phaseBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
					phaseBarrier.dstAccessMask =
						VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
					vkCmdPipelineBarrier(
						commandBuffer,
						VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						0u,
						1u,
						&phaseBarrier,
						0u,
						nullptr,
						0u,
						nullptr);
					++result.barrierCount;
				}
			}
		}
		result.recorded = true;
		return result;
	}
} // namespace TerraEngine::Renderer::Vulkan
