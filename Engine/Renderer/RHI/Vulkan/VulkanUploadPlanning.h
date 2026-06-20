// VulkanUploadPlanning.h - immutable upload preparation records.

#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Core/Export.h"

#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct UploadPreparationPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t resourceRevision = 0u;
		std::vector<std::string> payloads;
		RenderWorkBudget budget;
		RenderCancellationToken cancellationToken;
		std::vector<std::string> delayedPayloads;
		bool queueSubmissionCoordinatorOwned = true;
		bool overBudget = false;
	};

	TERRAENGINE_API UploadPreparationPlan BuildUploadPreparationPlan(const RenderFrameSnapshot& snapshot, std::size_t payloadBudget = 64u);

}
