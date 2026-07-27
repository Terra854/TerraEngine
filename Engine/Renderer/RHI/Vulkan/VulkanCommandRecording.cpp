#include "VulkanCommandRecording.h"

#include <limits>

namespace TerraEngine::Renderer::Vulkan {
	namespace {
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

	VulkanAnimationCommandPlan BuildVulkanAnimationCommandPlan(
		const Animation::GpuAnimationPlan& plan,
		std::uint32_t rendererGeneration)
	{
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

}
