// Immutable owner for one native-extent HDR-linear FXAA stage.
#pragma once
#include "VulkanPostProcessTargets.h"
#include <memory>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct FxaaPass;
	// Call only when FXAA is selected. No image allocation is performed here;
	// the graph owns distinct source/output images, pass and framebuffer. The
	// supplied render pass has one color attachment, initial COLOR_ATTACHMENT
	// and final SHADER_READ. Source content is HDR-linear Rec.709 after the chosen color stage.
	std::shared_ptr<FxaaPass> PrepareFxaaPass(VkPhysicalDevice physicalDevice,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& source,
		PostProcessTargetImage& destination, VkRenderPass processingPass,
		VkFramebuffer destinationFramebuffer, std::string& reason);

	// Owner-thread command recording only: no allocation, asset lookup or live
	// scene access. Caller supplies a fragment-readable source and retains the
	// owner and every borrowed image until actual submission/readback completion.
	void RecordFxaaPass(VkCommandBuffer command, FxaaPass& pass);
}
