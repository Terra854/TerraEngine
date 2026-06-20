#include "VulkanPipelineStateKeys.h"

#include <algorithm>

namespace TerraEngine::Renderer::Vulkan {

	std::vector<PipelineStateKey> BuildPipelineStateKeys(const RenderFrameSnapshot& snapshot)
	{
		std::vector<PipelineStateKey> keys;
		keys.reserve(snapshot.renderables.size());
		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			PipelineStateKey key;
			key.sortKey = std::hash<TerraEngine::UUID>{}(renderable.materialId) ^ (std::hash<TerraEngine::UUID>{}(renderable.meshId) << 1u);
			key.label = TerraEngine::ToHexString(renderable.materialId);
			keys.push_back(std::move(key));
		}
		std::stable_sort(keys.begin(), keys.end(), [](const PipelineStateKey& left, const PipelineStateKey& right) {
			return left.sortKey < right.sortKey;
		});
		return keys;
	}

}
