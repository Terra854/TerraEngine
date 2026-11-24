// Lazy immutable fullscreen tone/grade pipeline for one prepared view stage.
#pragma once
#include "VulkanPostProcessTargets.h"
#include <memory>
#include <string>

namespace TerraEngine::Renderer::GraphicsSettings { struct PostProcessColorSettings; }
namespace TerraEngine::Renderer::Vulkan
{
	struct TonePass;
	struct ToneLookupTextures
	{
		VkImageView curve = VK_NULL_HANDLE;
		VkImageView lut = VK_NULL_HANDLE;
		std::uint32_t curveWidth = 0;
		std::uint32_t lutEdge = 0;
	};

	// Call only when the graph requires tone/grading. Source and optional trusted
	// lookups must be shader-readable before recording. The processing pass must
	// have one color attachment, initial COLOR_ATTACHMENT and final SHADER_READ.
	// The caller retains the exact target, compatible pass/framebuffer and lookup
	// assets until every borrowing GPU submission has physically completed.
	std::shared_ptr<TonePass> PrepareTonePass(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& source, PostProcessTargetImage& destination,
		VkRenderPass processingPass, VkFramebuffer destinationFramebuffer,
		ToneLookupTextures lookups, std::string& reason);

	// Copied live controls change push constants only. This owner-thread recording
	// performs no file IO, allocation, asset lookup or live scene access.
	void RecordTonePass(VkCommandBuffer command, TonePass& pass,
		const GraphicsSettings::PostProcessColorSettings& settings);
}
