// VulkanPostProcessBloom.h - Per-view HDR bloom preparation and recording.
#pragma once

#include "VulkanPostProcessTargets.h"
#include <memory>
#include <string>

namespace TerraEngine::Renderer::GraphicsSettings { struct PostProcessColorSettings; }

namespace TerraEngine::Renderer::Vulkan
{
	struct BloomPass;
	// Call only for an enabled nonzero bloom candidate. The returned owner keeps
	// private mip resources; its target bundle retains all borrowed attachments.
	std::shared_ptr<BloomPass> PrepareBloomPass(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& exposedSource, PostProcessTargetImage& bloomOutput, VkRenderPass processingPass,
		VkFramebuffer bloomDestination, std::string& reason);
	// Lens consumers can request the actual HDR mip chain without compositing
	// bloom into the camera image. They retain the real producer owner.
	std::shared_ptr<BloomPass> PrepareBloomRadiance(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& exposedSource, std::string& reason);
	PostProcessTargetImage ReadBloomRadianceTarget(const BloomPass& pass) noexcept;
	// The caller has already made the exposed source compute-visible and keeps
	// the owner through actual submission completion. Output is the supplied
	// distinct attachment, allowing bloom-only graphs to allocate one HDR target.
	void RecordBloomPass(VkCommandBuffer command, BloomPass& pass,
		const GraphicsSettings::PostProcessColorSettings& settings);
}
