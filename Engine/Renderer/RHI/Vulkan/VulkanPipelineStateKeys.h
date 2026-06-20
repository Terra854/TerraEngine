// VulkanPipelineStateKeys.h - deterministic state keys for worker prep.

#pragma once

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct PipelineStateKey
	{
		std::uint64_t sortKey = 0u;
		std::string label;
	};

	TERRAENGINE_API std::vector<PipelineStateKey> BuildPipelineStateKeys(const RenderFrameSnapshot& snapshot);

}
