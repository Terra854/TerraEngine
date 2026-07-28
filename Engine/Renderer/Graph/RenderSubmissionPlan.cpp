#include "RenderSubmissionPlan.h"

#include <utility>

namespace TerraEngine::Renderer {

	namespace {
		void AddSubmissionDiagnostic(RenderSubmissionPlan& plan, std::string code, std::string message, std::string action)
		{
			plan.valid = false;
			RenderGraphValidationDiagnostic diagnostic;
			diagnostic.severity = "error";
			diagnostic.code = std::move(code);
			diagnostic.message = std::move(message);
			diagnostic.recommendedAction = std::move(action);
			plan.validationDiagnostics.push_back(std::move(diagnostic));
		}
	}

	RenderSubmissionPlan BuildRenderSubmissionPlan(
		const RenderFrameSnapshot& snapshot,
		const std::vector<RenderPassNode>& passes,
		const RenderWorkPacketPlan& packetPlan,
		std::uint64_t currentFrameId,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> animationPlan)
	{
		RenderSubmissionPlan plan;
		plan.frameId = snapshot.frameId;
		plan.requestedViewCount = snapshot.viewRequests.size();
		plan.orderedPasses = passes;
		plan.recordedWork = packetPlan.packets;
		plan.animationWork = BuildAnimationBackendWorkPlan(snapshot, std::move(animationPlan));
		const DeformationBackendWorkPlan deformation =
			BuildDeformationBackendWorkPlan(snapshot);
		plan.deformationWork = deformation.immutablePlan;
		plan.fallbacksUsed = packetPlan.serializedReasons;

		for (const RenderPassNode& pass : passes)
		{
			for (const RenderResourceState& output : pass.outputs)
			{
				plan.resourceTransitions.push_back(output);
			}
		}
		plan.queueReadiness = {
			{ RenderQueueCategory::Transfer, RenderQueueMode::Serial, false, "Single-queue fallback remains valid until backend reports queue overlap." },
			{ RenderQueueCategory::Compute, RenderQueueMode::Unavailable, false, "No compute overlap requested by this frame." },
			{ RenderQueueCategory::Graphics, RenderQueueMode::Serial, false, {} },
			{ RenderQueueCategory::Presentation, RenderQueueMode::Serial, false, "Presentation remains coordinator-owned." }
		};

		ValidateRenderSubmissionPlan(plan, currentFrameId);
		return plan;
	}

	bool ValidateRenderSubmissionPlan(RenderSubmissionPlan& plan, std::uint64_t currentFrameId)
	{
		plan.validationDiagnostics.clear();
		plan.valid = true;
		if (plan.frameId != currentFrameId)
		{
			AddSubmissionDiagnostic(plan, "stale-frame", "Submission plan frame id does not match the current frame.", "Discard stale work and rebuild from the latest snapshot.");
		}
		if (plan.requestedViewCount > 0u && plan.orderedPasses.empty())
		{
			AddSubmissionDiagnostic(plan, "missing-passes", "Submission plan contains no render passes.", "Build a render graph before final submission.");
		}
		// Hand-authored and legacy submission plans do not declare an animation
		// envelope. Treat the all-default record as "no animation work" while still
		// validating every envelope produced by BuildAnimationBackendWorkPlan,
		// including rejected envelopes whose diagnostic is the only populated field.
		const bool hasDeclaredAnimationWork = plan.animationWork.frameId != 0U
			|| !plan.animationWork.ranges.empty()
			|| plan.animationWork.gpuAnimatorCount != 0U
			|| plan.animationWork.cpuOrFallbackAnimatorCount != 0U
			|| plan.animationWork.immutablePlan != nullptr
			|| plan.animationWork.cpuDeformationPlan != nullptr
			|| !plan.animationWork.valid
			|| !plan.animationWork.rejectionReason.empty();
		if(hasDeclaredAnimationWork
			&& (plan.animationWork.frameId != plan.frameId || !plan.animationWork.valid))
		{
			AddSubmissionDiagnostic(plan, "invalid-animation-work",
				plan.animationWork.rejectionReason.empty()
					? "Animation work was prepared for a different frame."
					: plan.animationWork.rejectionReason,
				"Discard the complete animation envelope and use one declared CPU or bind-pose fallback generation.");
		}
		if(plan.animationWork.cpuDeformationPlan != nullptr
			&& (!plan.animationWork.cpuDeformationPlan->IsComplete()
				|| plan.animationWork.cpuDeformationPlan->frameId != plan.frameId))
		{
			AddSubmissionDiagnostic(plan, "invalid-cpu-animation-deformation",
				"CPU animation deformation data is incomplete or belongs to another frame.",
				"Discard the CPU palette upload and render the authored bind pose.");
		}
		if(plan.deformationWork != nullptr
			&& (!plan.deformationWork->IsComplete()
				|| plan.deformationWork->frameId != plan.frameId))
		{
			AddSubmissionDiagnostic(plan, "invalid-animated-geometry-deformation",
				"Animated geometry deformation work is incomplete or belongs to another frame.",
				"Discard the candidate deformation publication and keep compatible fallback coverage.");
		}
		if (!plan.frameOverlapAllowed && plan.frameOverlapBlockedReasons.empty())
		{
			plan.frameOverlapBlockedReasons.push_back("Frame overlap remains disabled until scene mutation and resource lifetime boundaries are proven deterministic.");
		}
		for (const RenderWorkPacket& packet : plan.recordedWork)
		{
			if (packet.state == RenderPacketState::Failed)
			{
				if (packet.failurePolicy == RenderFailurePolicy::Fatal)
				{
					plan.fatalFailures.push_back(packet.packetId);
					AddSubmissionDiagnostic(plan, "failed-packet", "Required render work packet failed.", "Use a validated fallback or block submission.");
				}
				else
				{
					plan.degradedWork.push_back(packet.packetId);
				}
			}
			if (packet.state == RenderPacketState::Cancelled)
			{
				if (packet.failurePolicy == RenderFailurePolicy::Fatal)
				{
					AddSubmissionDiagnostic(plan, "cancelled-packet", "Required render work packet was cancelled.", "Rebuild the frame or serialize the pass.");
				}
				else
				{
					plan.degradedWork.push_back(packet.packetId);
				}
			}
			if (packet.state == RenderPacketState::Stale)
			{
				AddSubmissionDiagnostic(plan, "stale-packet", "Render work packet completed after its owner changed.", "Discard stale results before publishing.");
			}
			for (const RenderResourceState& output : packet.outputs)
			{
				if (output.accessMode == RenderAccessMode::Present && packet.executionPolicy == RenderPacketExecutionPolicy::Worker)
				{
					AddSubmissionDiagnostic(plan, "worker-present", "Worker packet attempted to own presentation state.", "Keep presentation resources coordinator-owned.");
				}
			}
		}
		for (const RenderQueueReadiness& readiness : plan.queueReadiness)
		{
			if (readiness.mode == RenderQueueMode::Parallel && readiness.handoffRequired && readiness.fallbackReason.empty())
			{
				AddSubmissionDiagnostic(plan, "missing-queue-handoff-reason", "Parallel queue readiness requires a documented ownership handoff.", "Record the queue handoff or use serial fallback.");
			}
		}
		return plan.valid;
	}

}
