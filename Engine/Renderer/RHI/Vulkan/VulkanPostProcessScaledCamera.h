// Native internal-resolution camera raster owner.
#pragma once
#include "VulkanPostProcessTargets.h"
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct ScaledCameraTargets
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkExtent2D extent{};
		bool initialized = false; // Set only by the queue-accepted output boundary.
		PostProcessTargetImage color;
		PostProcessTargetImage identity;
		PostProcessTargetImage depth;

		ScaledCameraTargets() = default;
		ScaledCameraTargets(const ScaledCameraTargets&) = delete;
		ScaledCameraTargets& operator=(const ScaledCameraTargets&) = delete;
		~ScaledCameraTargets();
	};

	// Preparation requires the actual single-sample offscreen pass with typed
	// identity. No VRS attachment, color upscaling or capture is done here.
	std::shared_ptr<ScaledCameraTargets> PrepareScaledCameraTargets(VkPhysicalDevice physicalDevice,
		VkDevice device, VmaAllocator allocator, VkRenderPass renderPass, VkFormat colorFormat,
		VkFormat depthFormat, VkExtent2D internalExtent, std::string& reason);
	void RecordScaledCameraInitialization(VkCommandBuffer command, const ScaledCameraTargets& targets) noexcept;
}
