#include "VulkanCommandRecording.h"
#include "VulkanDescriptorPlanning.h"
#include "Profiling/ProfilerScope.h"

#include <limits>

namespace TerraEngine::Renderer::Vulkan {
	namespace {
		// Vulkan guarantees at least 65,535 compute workgroups in each dispatch
		// dimension. Chunking at that portable limit keeps the value plan independent
		// of a particular adapter while collapsing ordinary crowds to one dispatch.
		constexpr std::uint32_t kMaxDeformationWorkRecordsPerDispatch = 65535u;

		[[nodiscard]] VkAccessFlags AccessMaskForAnimationBarrier(std::uint32_t flags)
		{
			// All animation stages write storage buffers. The exact producer bit is
			// retained in the value plan for diagnostics, while Vulkan receives the
			// conservative storage-write to storage-read/write visibility boundary.
			return flags == 0U ? 0U : VK_ACCESS_SHADER_WRITE_BIT;
		}

		void RecordAnimationBarrier(VkCommandBuffer commandBuffer, std::uint32_t flags)
		{
			if(flags == 0U) return;
			const bool publishesToGraphics = (flags
				& static_cast<std::uint32_t>(Animation::GpuAnimationBarrierFlag::PaletteToRender)) != 0U;
			VkMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			barrier.srcAccessMask = AccessMaskForAnimationBarrier(flags);
			// Intermediate boundaries feed another compute stage, while the final
			// palette boundary feeds task/mesh/vertex shaders in every graphics view.
			// ALL_GRAPHICS deliberately covers conventional and mesh-shader paths so
			// a later draw cannot observe a cleared or previous-frame palette.
			barrier.dstAccessMask = publishesToGraphics
				? VK_ACCESS_SHADER_READ_BIT
				: VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				publishesToGraphics
					? VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
					: VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0U, 1U, &barrier, 0U, nullptr, 0U, nullptr);
		}

		void RecordDeformationBarrier(
			VkCommandBuffer commandBuffer,
			VulkanDeformationBarrierKind kind)
		{
			VkMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			VkPipelineStageFlags sourceStage =
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			VkPipelineStageFlags destinationStage =
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.dstAccessMask =
				VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_SHADER_WRITE_BIT;
			if(kind
				== VulkanDeformationBarrierKind::
					AnimationOrUploadToDeform)
			{
				sourceStage =
					VK_PIPELINE_STAGE_TRANSFER_BIT
					| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
					| VK_PIPELINE_STAGE_HOST_BIT;
				barrier.srcAccessMask =
					VK_ACCESS_TRANSFER_WRITE_BIT
					| VK_ACCESS_SHADER_WRITE_BIT
					| VK_ACCESS_HOST_WRITE_BIT;
			}
			else if(kind
				== VulkanDeformationBarrierKind::
					RefinementToGraphics)
			{
				destinationStage =
					VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			}
			vkCmdPipelineBarrier(
				commandBuffer,
				sourceStage,
				destinationStage,
				0u,
				1u,
				&barrier,
				0u,
				nullptr,
				0u,
				nullptr);
		}
	}

	std::string DescribeBackendLocalState(RenderAccessMode accessMode, RenderStage stage)
	{
		// Keep this as text for now so tests can validate the portability
		// boundary without linking against Vulkan objects or headers.
		std::string access;
		switch (accessMode)
		{
		case RenderAccessMode::Read:
			access = "read";
			break;
		case RenderAccessMode::Write:
			access = "write";
			break;
		case RenderAccessMode::ReadWrite:
			access = "read-write";
			break;
		case RenderAccessMode::Present:
			access = "present";
			break;
		case RenderAccessMode::Upload:
			access = "upload";
			break;
		case RenderAccessMode::Resolve:
			access = "resolve";
			break;
		default:
			access = "unknown";
			break;
		}

		return access + " stage " + std::to_string(static_cast<int>(stage));
	}

	BackendRecordingBatchPlan BuildVulkanRecordingBatchPlan(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const BackendCapabilityProfile& capabilities,
		RenderBackendWorkKind workKind)
	{
		// Vulkan can translate worker-eligible renderer packets into secondary
		// recording batches, but the result intentionally remains expressed as
		// backend-neutral batch metadata. Actual command-buffer allocation and
		// queue submission stay owned by VulkanContext at the coordinator edge.
		return BuildBackendRecordingBatchPlan(snapshot, packetPlan, capabilities, workKind);
	}

	VulkanRendererThreadingPlan BuildVulkanRendererThreadingPlan(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities)
	{
		VulkanRendererThreadingPlan plan;
		plan.frameId = snapshot.frameId;
		auto addStage = [&](std::string stageId, RenderBackendWorkKind workKind, bool supported, bool coordinatorOwned, std::string coordinatorReason = {}) {
			VulkanRendererThreadingStage stage;
			stage.stageId = std::move(stageId);
			stage.workKind = workKind;
			stage.workerEligible = supported && !coordinatorOwned;
			stage.coordinatorOwned = coordinatorOwned || !supported;
			stage.coordinatorReason = stage.coordinatorOwned
				? (coordinatorReason.empty() ? "Backend capability or Vulkan ownership boundary keeps this stage serialized." : std::move(coordinatorReason))
				: "";
			plan.workerEligibleStageCount += stage.workerEligible ? 1u : 0u;
			plan.coordinatorStageCount += stage.coordinatorOwned ? 1u : 0u;
			plan.stages.push_back(std::move(stage));
		};

		// Virtual geometry uses the same split as the rest of the renderer:
		// worker-safe preparation builds immutable LOD choices, selected cluster
		// ranges, cluster packet batches, page-need packets, descriptor/update
		// intents, and upload plans.
		// Actual Vulkan page upload, command-buffer recording ownership, resource
		// retirement, and resident-page publication remain protected coordinator
		// work so workers never mutate live Vulkan objects.
		addStage("prepared-draw-setup", RenderBackendWorkKind::Geometry, capabilities.supportsParallelRecording, false);
		addStage("shadow-draw-list-build", RenderBackendWorkKind::Shadow, capabilities.supportsParallelShadowRecording, false);
		addStage("shadow-secondary-recording", RenderBackendWorkKind::Shadow, capabilities.supportsParallelShadowRecording && capabilities.supportsSecondaryRecording, false);
		addStage("per-view-recording", RenderBackendWorkKind::Geometry, capabilities.supportsParallelPerViewRecording, false);
		addStage("pipeline-state-key-generation", RenderBackendWorkKind::Geometry, capabilities.supportsParallelPipelineStatePreparation, false);
		addStage("descriptor-write-planning", RenderBackendWorkKind::Descriptor, capabilities.supportsParallelDescriptorPreparation, false);
		addStage("upload-staging-planning", RenderBackendWorkKind::Upload, capabilities.supportsParallelUploadPlanning, false);
		addStage("resource-transition-planning", RenderBackendWorkKind::Transition, capabilities.supportsParallelTransitionPlanning, false);
		addStage("renderable-sort-merge", RenderBackendWorkKind::Geometry, capabilities.supportsParallelRecording, false);
		addStage("frustum-culling", RenderBackendWorkKind::Culling, capabilities.supportsParallelCulling, false);
		addStage("occlusion-hiz-preparation", RenderBackendWorkKind::Culling, capabilities.supportsParallelCulling, false);
		addStage("light-cluster-preparation", RenderBackendWorkKind::Culling, capabilities.supportsParallelLightPreparation, false);
		addStage("ui-geometry-preparation", RenderBackendWorkKind::UI, capabilities.supportsParallelUiPreparation, false);
		addStage("thread-local-transient-allocation", RenderBackendWorkKind::Geometry, capabilities.supportsRenderTaskGroups, false);
		addStage("secondary-command-cache-reuse", RenderBackendWorkKind::Validation, capabilities.supportsSecondaryRecording, false);
		addStage("material-descriptor-cache-reuse", RenderBackendWorkKind::Descriptor, capabilities.supportsParallelDescriptorPreparation, false);
		addStage("mesh-draw-packet-cache-reuse", RenderBackendWorkKind::Geometry, capabilities.supportsParallelRecording, false);
		addStage("batched-resource-upload-packets", RenderBackendWorkKind::Upload, capabilities.supportsParallelUploadPlanning, false);
		// Animation lowering and descriptor/resource planning operate on the
		// immutable GpuAnimationPlan. Vulkan command recording and completion
		// stamping remain frame-bound protected work, never per-instance submits.
		addStage("animation-plan-upload-preparation", RenderBackendWorkKind::Upload, capabilities.supportsParallelUploadPlanning, false);
		addStage("animation-descriptor-plan-preparation", RenderBackendWorkKind::Descriptor, capabilities.supportsParallelDescriptorPreparation, false);
		addStage("animation-batched-compute-recording", RenderBackendWorkKind::Geometry, false, true,
			"Animation compute binds one frame descriptor set and records into the existing primary submission.");
		addStage("animation-generation-completion", RenderBackendWorkKind::Validation, false, true,
			"Animation completion is published only after the frame fence; pose data is never read back.");
		addStage("per-thread-diagnostics-buffering", RenderBackendWorkKind::Stats, capabilities.supportsPacketLocalDiagnostics, false);
		addStage("final-submit-present-retire", RenderBackendWorkKind::Validation, true, true, "Vulkan queue submit, present, swapchain resize, and resource retirement stay coordinator-owned.");
		return plan;
	}

	VulkanVirtualGeometryCommandPlan BuildVulkanVirtualGeometryCommandPlan(
		const VirtualGeometryGpuPlan& immutablePlan,
		const VulkanVirtualGeometryExecutorResources& resources,
		const VulkanVirtualGeometryExecutorPipelines& pipelines)
	{
		VulkanVirtualGeometryCommandPlan commandPlan;
		if(!immutablePlan.IsComplete())
		{
			commandPlan.rejectionReason = immutablePlan.rejectionReason.empty()
				? "Virtual-geometry command recording requires a complete immutable GPU plan."
				: immutablePlan.rejectionReason;
			return commandPlan;
		}

		commandPlan.executorPlan = BuildVulkanVirtualGeometryExecutorPlan(
			immutablePlan.traversal.instanceCount,
			immutablePlan.capacity,
			immutablePlan.generatedWork.usesIndirectCount,
			resources,
			pipelines,
			immutablePlan.frameId,
			immutablePlan.generation,
			immutablePlan.generation,
			immutablePlan.generation);
		if(!commandPlan.executorPlan.unavailableReason.empty())
		{
			commandPlan.rejectionReason = commandPlan.executorPlan.unavailableReason;
			return commandPlan;
		}

		// The ordering is not optional: each stage publishes storage writes for
		// the next one, and the final stage creates the indirect draw boundary.
		commandPlan.stages = {
			VulkanVirtualGeometryExecutorStage::Traversal,
			VulkanVirtualGeometryExecutorStage::Compaction,
			VulkanVirtualGeometryExecutorStage::Indirect };
		commandPlan.valid = true;
		return commandPlan;
	}

	VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryCommands(
		VkCommandBuffer commandBuffer,
		const VulkanVirtualGeometryCommandPlan& commandPlan,
		const VulkanVirtualGeometryExecutorResources& resources,
		const VulkanVirtualGeometryExecutorPipelines& pipelines)
	{
		VulkanVirtualGeometryExecutorRecordingResult result{};
		result.zeroWork = commandPlan.executorPlan.zeroWork;
		if(!commandPlan.valid)
		{
			result.failureReason = commandPlan.rejectionReason.empty()
				? "Virtual-geometry Vulkan command plan is invalid."
				: commandPlan.rejectionReason;
			return result;
		}

		for(const VulkanVirtualGeometryExecutorStage stage : commandPlan.stages)
		{
			const VulkanVirtualGeometryExecutorRecordingResult stageResult =
				RecordVulkanVirtualGeometryExecutorStage(
					commandBuffer, stage, commandPlan.executorPlan, resources, pipelines);
			if(!stageResult.recorded)
			{
				return stageResult;
			}
			result.dispatchCount += stageResult.dispatchCount;
			result.barrierCount += stageResult.barrierCount;
		}
		result.recorded = true;
		return result;
	}

	VulkanAnimationCommandPlan BuildVulkanAnimationCommandPlan(
		const Animation::GpuAnimationPlan& plan,
		std::uint32_t rendererGeneration)
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Build Animation Command Plan",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		VulkanAnimationCommandPlan output;
		output.planGeneration = plan.generation;
		output.rendererGeneration = rendererGeneration;
		output.hasWork = !plan.dispatchCommands.empty();
		if(!output.hasWork) {
			// Do not create a command, barrier, descriptor bind, or submission when
			// no animator selected GPU visual evaluation this frame.
			output.valid = true;
			return output;
		}
		if(plan.generation == 0U || rendererGeneration == 0U) {
			output.rejectionReason = "Animation and renderer generations must both be non-zero.";
			return output;
		}

		output.dispatches.reserve(plan.dispatchCommands.size());
		std::uint32_t previousStage = 0U;
		bool hasPreviousStage = false;
		for(const Animation::GpuAnimationDispatchCommand& command : plan.dispatchCommands) {
			const std::uint32_t stage = command.pushConstants.stage;
			if(!Animation::IsGpuAnimationStageValueValid(stage)
				|| command.pushConstants.planGeneration != plan.generation
				|| command.groupCountX == 0U || command.groupCountX > Animation::kGpuAnimationMaximumGroupsX) {
				output.dispatches.clear();
				output.rejectionReason = "Animation dispatch exceeds the frozen stage, generation, or workgroup bounds.";
				return output;
			}
			if(hasPreviousStage && stage < previousStage) {
				output.dispatches.clear();
				output.rejectionReason = "Animation dispatches must remain stage-sorted for explicit barriers.";
				return output;
			}
			output.dispatches.push_back(command);
			previousStage = stage;
			hasPreviousStage = true;
		}
		for(const Animation::GpuAnimationStageSpan& span : plan.stageSpans) {
			if(span.commandCount == 0U || !Animation::IsGpuAnimationStageValueValid(span.stage)) continue;
			if(span.barrierFlags != 0U) output.barriers.push_back({ span.stage, span.barrierFlags });
		}
		output.valid = true;
		return output;
	}

	VulkanAnimationRecordingResult RecordVulkanAnimationCommands(
		VkCommandBuffer commandBuffer,
		const VulkanAnimationCommandPlan& plan,
		const VulkanAnimationComputePipelines& pipelines,
		VkDescriptorSet descriptorSet)
	{
		VulkanAnimationRecordingResult result;
		if(!plan.hasWork) return result;
		if(!plan.valid || commandBuffer == VK_NULL_HANDLE) {
			result.status = VulkanAnimationRecordingStatus::InvalidPlan;
			return result;
		}
		if(descriptorSet == VK_NULL_HANDLE || pipelines.layout == VK_NULL_HANDLE) {
			result.status = VulkanAnimationRecordingStatus::MissingDescriptorSet;
			return result;
		}

		// One descriptor set is bound for the complete generation. There is no
		// per-instance descriptor allocation or rebinding in this hot path.
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipelines.layout, Animation::kGpuAnimationDescriptorSet, 1U, &descriptorSet, 0U, nullptr);

		std::size_t barrierIndex = 0U;
		std::uint32_t activeStage = std::numeric_limits<std::uint32_t>::max();
		for(const Animation::GpuAnimationDispatchCommand& dispatch : plan.dispatches) {
			const std::uint32_t stage = dispatch.pushConstants.stage;
			if(stage != activeStage) {
				while(barrierIndex < plan.barriers.size() && plan.barriers[barrierIndex].afterStage < stage) {
					RecordAnimationBarrier(commandBuffer, plan.barriers[barrierIndex++].flags);
					++result.emittedBarrierCount;
				}
				if(pipelines.byStage[stage] == VK_NULL_HANDLE) {
					result.status = VulkanAnimationRecordingStatus::MissingPipeline;
					return result;
				}
				vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.byStage[stage]);
				activeStage = stage;
			}
			vkCmdPushConstants(commandBuffer, pipelines.layout, VK_SHADER_STAGE_COMPUTE_BIT,
				0U, sizeof(Animation::GpuAnimationPushConstants), &dispatch.pushConstants);
			vkCmdDispatch(commandBuffer, dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
			++result.recordedDispatchCount;
		}
		while(barrierIndex < plan.barriers.size()) {
			RecordAnimationBarrier(commandBuffer, plan.barriers[barrierIndex++].flags);
			++result.emittedBarrierCount;
		}
		result.status = VulkanAnimationRecordingStatus::Recorded;
		return result;
	}

	VulkanDeformationCommandPlan BuildVulkanDeformationCommandPlan(
		const GpuDeformationPlan& plan)
	{
		VulkanDeformationCommandPlan output;
		output.frameId = plan.frameId;
		output.rendererGeneration = plan.rendererGeneration;
		output.hasWork = !plan.work.empty();
		if(!output.hasWork)
		{
			output.valid = plan.valid;
			if(!plan.valid)
			{
				output.rejectionReason = plan.rejectionReason;
			}
			return output;
		}
		if(!plan.IsComplete())
		{
			output.rejectionReason = "Deformation commands require a complete immutable plan.";
			return output;
		}
		if(plan.work.size() > std::numeric_limits<std::uint32_t>::max())
		{
			output.rejectionReason =
				"Deformation work exceeds the portable 32-bit shader index range.";
			return output;
		}
		const std::uint32_t totalWorkRecordCount =
			static_cast<std::uint32_t>(plan.work.size());
		output.deformationDispatches.reserve(
			(totalWorkRecordCount + kMaxDeformationWorkRecordsPerDispatch - 1u)
				/ kMaxDeformationWorkRecordsPerDispatch);
		for(std::uint32_t first = 0u; first < totalWorkRecordCount;)
		{
			const std::uint32_t count = std::min(
				kMaxDeformationWorkRecordsPerDispatch,
				totalWorkRecordCount - first);
			std::uint32_t maximumGroupCountX = 0u;
			for(std::uint32_t offset = 0u; offset < count; ++offset)
			{
				const std::uint32_t vertexCount = plan.work[first + offset].vertexCount;
				const std::uint32_t groups = (vertexCount + 63u) / 64u;
				if(groups == 0u || groups > kMaxDeformationWorkRecordsPerDispatch)
				{
					output.deformationDispatches.clear();
					output.rejectionReason =
						"Deformation dispatch exceeds the portable compute-grid range.";
					return output;
				}
				maximumGroupCountX = std::max(maximumGroupCountX, groups);
			}
			output.deformationDispatches.push_back({
				first, count, maximumGroupCountX });
			first += count;
		}
		// The complete frame union is synchronized once, regardless of view count.
		output.barriers = {
			VulkanDeformationBarrierKind::AnimationOrUploadToDeform,
			VulkanDeformationBarrierKind::DeformToBounds,
			VulkanDeformationBarrierKind::BoundsToRefinement,
			VulkanDeformationBarrierKind::RefinementToGraphics
		};
		output.valid = true;
		return output;
	}

	VulkanDeformationRecordingResult
		RecordVulkanDeformationCommands(
			VkCommandBuffer commandBuffer,
			const VulkanDeformationCommandPlan& commandPlan,
			const GpuDeformationPlan& immutablePlan,
			const VulkanDeformationComputePipelines& pipelines,
			VkDescriptorSet descriptorSet)
	{
		VulkanDeformationRecordingResult result;
		if(!commandPlan.hasWork)
		{
			return result;
		}
		const auto Pipeline = [&pipelines](
			VulkanDeformationPipelineStage stage)
		{
			return pipelines.byStage[
				static_cast<std::size_t>(stage)];
		};
		if(!commandPlan.valid
			|| !immutablePlan.IsComplete()
			|| commandBuffer == VK_NULL_HANDLE
			|| descriptorSet == VK_NULL_HANDLE
			|| pipelines.layout == VK_NULL_HANDLE
			|| Pipeline(VulkanDeformationPipelineStage::Deform)
				== VK_NULL_HANDLE
			|| Pipeline(VulkanDeformationPipelineStage::Bounds)
				== VK_NULL_HANDLE
			|| Pipeline(VulkanDeformationPipelineStage::Refinement)
				== VK_NULL_HANDLE)
		{
			return result;
		}

		struct PushConstants
		{
			std::uint32_t workRecordIndex = 0u;
			std::uint32_t workRecordCount = 0u;
			std::uint32_t abiVersion =
				kAnimatedGeometryAbiVersion;
			std::uint32_t identity = 0u;
		};
		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_COMPUTE,
			pipelines.layout,
			kVulkanDeformationDescriptorSet,
			1u,
			&descriptorSet,
			0u,
			nullptr);

		RecordDeformationBarrier(
			commandBuffer,
			VulkanDeformationBarrierKind::
				AnimationOrUploadToDeform);
		++result.barrierCount;
		vkCmdBindPipeline(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_COMPUTE,
			Pipeline(VulkanDeformationPipelineStage::Deform));
		for(const VulkanDeformationDispatch& dispatch :
			commandPlan.deformationDispatches)
		{
			PushConstants push{
				dispatch.firstWorkRecordIndex,
				static_cast<std::uint32_t>(
					immutablePlan.work.size()),
				kAnimatedGeometryAbiVersion,
				0u };
			vkCmdPushConstants(
				commandBuffer,
				pipelines.layout,
				VK_SHADER_STAGE_COMPUTE_BIT,
				0u,
				sizeof(push),
				&push);
			vkCmdDispatch(
				commandBuffer,
				dispatch.groupCountX,
				dispatch.workRecordCount,
				1u);
			++result.deformationDispatchCount;
		}

		RecordDeformationBarrier(
			commandBuffer,
			VulkanDeformationBarrierKind::DeformToBounds);
		++result.barrierCount;
		vkCmdBindPipeline(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_COMPUTE,
			Pipeline(VulkanDeformationPipelineStage::Bounds));
		for(const VulkanDeformationDispatch& dispatch :
			commandPlan.deformationDispatches)
		{
			PushConstants push{
				dispatch.firstWorkRecordIndex,
				static_cast<std::uint32_t>(
					immutablePlan.work.size()),
				kAnimatedGeometryAbiVersion,
				0u };
			vkCmdPushConstants(
				commandBuffer,
				pipelines.layout,
				VK_SHADER_STAGE_COMPUTE_BIT,
				0u,
				sizeof(push),
				&push);
			vkCmdDispatch(
				commandBuffer,
				dispatch.workRecordCount,
				1u,
				1u);
			++result.boundsDispatchCount;
		}

		RecordDeformationBarrier(
			commandBuffer,
			VulkanDeformationBarrierKind::
				BoundsToRefinement);
		++result.barrierCount;
		if(immutablePlan.totalClusterBoundsCount != 0u)
		{
			vkCmdBindPipeline(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_COMPUTE,
				Pipeline(
					VulkanDeformationPipelineStage::Refinement));
			PushConstants push{
				0u,
				immutablePlan.totalClusterBoundsCount,
				kAnimatedGeometryAbiVersion,
				0u };
			vkCmdPushConstants(
				commandBuffer,
				pipelines.layout,
				VK_SHADER_STAGE_COMPUTE_BIT,
				0u,
				sizeof(push),
				&push);
			vkCmdDispatch(
				commandBuffer,
				(immutablePlan.totalClusterBoundsCount + 63u)
					/ 64u,
				1u,
				1u);
			result.refinementDispatchCount = 1u;
		}
		RecordDeformationBarrier(
			commandBuffer,
			VulkanDeformationBarrierKind::
				RefinementToGraphics);
		++result.barrierCount;
		result.recorded =
			result.deformationDispatchCount
				== commandPlan.deformationDispatches.size()
			&& result.boundsDispatchCount
				== commandPlan.deformationDispatches.size();
		return result;
	}

}
