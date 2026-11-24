// Immutable three-stage SMAA 1x resources for one prepared native view.
#pragma once
#include "Renderer/RHI/Vulkan/VulkanPostProcessTargets.h"
#include <memory>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct SmaaPass;
	// Protected preparation creates a complete private candidate, including its
	// intermediate attachments. Failure publishes nothing and retains no GPU work.
	// Source/destination are single-sample 2D mip-zero target images. They and
	// the borrowed resolve pass/framebuffer must belong to the
	// retained target owner. The resolve pass is single-color, sample1, initial
	// COLOR_ATTACHMENT_OPTIMAL and final SHADER_READ_ONLY_OPTIMAL, with
	// DONT_CARE load / STORE output. Opaque pass/framebuffer compatibility is
	// a caller-owned creation contract and cannot be introspected by this API.
	std::shared_ptr<SmaaPass> PrepareSmaaPass(VkPhysicalDevice physicalDevice,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& source,
		PostProcessTargetImage& destination, VkRenderPass processingPass,
		VkFramebuffer destinationFramebuffer, std::string& reason);
	// Source must already be display-linear Rec.709 in SHADER_READ_ONLY_OPTIMAL.
	// Record on the protected graphics queue outside any active render pass.
	// Caller retains this owner and its target until actual GPU borrowers retire.
	// Recording performs no file IO, allocations, capability queries or mutations
	// to descriptors. Alpha comes directly from the identical-format source texel.
	void RecordSmaaPass(VkCommandBuffer command, SmaaPass& pass);
}
