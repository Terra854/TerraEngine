// VulkanTransitionPlanning.h - backend-neutral transition planning for Vulkan.

#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Core/Export.h"

#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct ResourceTransitionPlan
	{
		std::uint64_t frameId = 0u;
		std::string passId;
		std::vector<RenderResourceState> transitions;
		std::vector<std::string> diagnostics;
	};

	TERRAENGINE_API ResourceTransitionPlan BuildResourceTransitionPlan(std::uint64_t frameId, const RenderPassNode& pass);

}
