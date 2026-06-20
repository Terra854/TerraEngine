#include "VulkanTransitionPlanning.h"

namespace TerraEngine::Renderer::Vulkan {

	ResourceTransitionPlan BuildResourceTransitionPlan(std::uint64_t frameId, const RenderPassNode& pass)
	{
		ResourceTransitionPlan plan;
		plan.frameId = frameId;
		plan.passId = pass.passId;
		plan.transitions = pass.outputs;
		if (plan.transitions.empty())
		{
			plan.diagnostics.push_back("pass has no writable output transitions");
		}
		return plan;
	}

}
