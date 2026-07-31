// RenderSubmissionPlan.h - validated frame hand-off to the submission owner.

#pragma once

#include "RenderGraph.h"
#include "RenderWorkPacket.h"
#include "Renderer/Deformation/GpuDeformationPlan.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	struct RenderSubmissionPlan
	{
		std::uint64_t frameId = 0u;
		std::size_t requestedViewCount = 0u;
		std::vector<RenderPassNode> orderedPasses;
		std::vector<RenderWorkPacket> recordedWork;
		AnimationBackendWorkPlan animationWork;
		// Renderer deformation is separate from animation evaluation because its
		// work set is the union of resident page demands from every render view.
		std::shared_ptr<const GpuDeformationPlan> deformationWork;
		// Worker preparation can attach one immutable device-authored geometry
		// envelope. The backend consumes this plan at its protected command
		// recording boundary; RenderSystem never hands it mutable scene storage.
		std::shared_ptr<const VirtualGeometryGpuPlan> virtualGeometryWork;
		std::vector<RenderResourceState> resourceTransitions;
		std::vector<RenderQueueReadiness> queueReadiness;
		std::vector<std::string> fallbacksUsed;
		std::vector<std::string> degradedWork;
		std::vector<std::string> fatalFailures;
		std::vector<RenderGraphValidationDiagnostic> validationDiagnostics;
		bool frameOverlapAllowed = false;
		std::vector<std::string> frameOverlapBlockedReasons;
		bool valid = true;
	};

	TERRAENGINE_API RenderSubmissionPlan BuildRenderSubmissionPlan(
		const RenderFrameSnapshot& snapshot,
		const std::vector<RenderPassNode>& passes,
		const RenderWorkPacketPlan& packetPlan,
		std::uint64_t currentFrameId,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> animationPlan = {},
		std::shared_ptr<const VirtualGeometryGpuPlan> virtualGeometryPlan = {});

	TERRAENGINE_API bool ValidateRenderSubmissionPlan(RenderSubmissionPlan& plan, std::uint64_t currentFrameId);

}
