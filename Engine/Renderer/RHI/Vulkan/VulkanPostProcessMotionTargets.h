// Per-view, depth-tested motion outputs for the indexed reconstruction path.
#pragma once

#include "VulkanPostProcessTargets.h"
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct MotionRasterTargets
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkExtent2D extent{};
		// The caller retains the accepted opaque depth owner through completion.
		VkImageView sourceDepth = VK_NULL_HANDLE;
		PostProcessTargetImage totalMotion;
		PostProcessTargetImage cameraMotion;
		bool usesCameraMotion = false;

		MotionRasterTargets() = default;
		MotionRasterTargets(const MotionRasterTargets&) = delete;
		MotionRasterTargets& operator=(const MotionRasterTargets&) = delete;
		~MotionRasterTargets();
	};

	// Borrow one exact single-sample opaque depth surface. The producer must
	// use the same current deformation, projection, viewport and depth compare
	// convention as the opaque draw. Preparation submits no GPU work.
	std::shared_ptr<MotionRasterTargets> PrepareMotionRasterTargets(
		VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
		VkImageView depthView, VkFormat depthFormat, VkExtent2D extent,
		bool cameraMotion, std::string& reason);

	// Begin the pass after the rendering owner has finished opaque depth.
	// The cleared pixels are explicitly invalid until covered by a valid draw.
	void BeginMotionRasterPass(VkCommandBuffer command, const MotionRasterTargets& targets) noexcept;
}
