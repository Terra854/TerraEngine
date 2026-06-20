// RenderSubmissionPlan.h - validated frame hand-off to the submission owner.

#pragma once

#include "RenderGraph.h"
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
		std::uint64_t currentFrameId);

	TERRAENGINE_API bool ValidateRenderSubmissionPlan(RenderSubmissionPlan& plan, std::uint64_t currentFrameId);

}
