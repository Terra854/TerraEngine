// VulkanUploadPlanning.h - immutable upload preparation records.

#pragma once

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Core/Export.h"

#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct UploadPreparationPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t resourceRevision = 0u;
		std::vector<std::string> payloads;
		bool queueSubmissionCoordinatorOwned = true;
	};

	TERRAENGINE_API UploadPreparationPlan BuildUploadPreparationPlan(const RenderFrameSnapshot& snapshot);

}
