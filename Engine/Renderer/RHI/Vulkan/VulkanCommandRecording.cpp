#include "VulkanCommandRecording.h"

namespace TerraEngine::Renderer::Vulkan {

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
		addStage("per-thread-diagnostics-buffering", RenderBackendWorkKind::Stats, capabilities.supportsPacketLocalDiagnostics, false);
		addStage("final-submit-present-retire", RenderBackendWorkKind::Validation, true, true, "Vulkan queue submit, present, swapchain resize, and resource retirement stay coordinator-owned.");
		return plan;
	}

}
