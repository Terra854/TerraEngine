// VulkanDescriptorPlanning.h - immutable descriptor preparation records.

#pragma once

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Core/Export.h"

#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct DescriptorBindingPlan
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		std::vector<std::string> records;
		bool coordinatorOwnedAllocator = true;
		std::string fallbackReason;
	};

	TERRAENGINE_API DescriptorBindingPlan BuildDescriptorBindingPlan(const RenderFrameSnapshot& snapshot, std::uint32_t viewId);

}
