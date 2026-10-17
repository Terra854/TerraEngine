// VulkanVRS.cpp — Vulkan Variable Rate Shading Implementation
//
// Implements the management of shading rate images and their associated GPU resources in Vulkan.
// It handles the creation, destruction, and resizing of VRS attachments based on hardware capabilities and frame requirements.
// This module provides the backend support for optimizing fragment shading frequency during rendering.

#include "VulkanVRS.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

void VulkanVRS::Create(VkDevice device, VkPhysicalDevice physicalDevice, VmaAllocator allocator, uint32_t screenWidth, uint32_t screenHeight) {
	VkPhysicalDeviceFragmentShadingRatePropertiesKHR vrsProps{};
	vrsProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
	
	VkPhysicalDeviceProperties2 props{};
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = &vrsProps;
	vkGetPhysicalDeviceProperties2(physicalDevice, &props);

	texelSize = vrsProps.minFragmentShadingRateAttachmentTexelSize;

	if (texelSize.width == 0 || texelSize.height == 0) {
		texelSize = { 8, 8 }; // Fallback
	}

	// Exported games can enter this path while the window is minimized or still
	// negotiating its drawable size. Keep the shading-rate image legal and let
	// later swapchain recreation resize it to the real dimensions.
	width = std::max((screenWidth + texelSize.width - 1) / texelSize.width, 1u);
	height = std::max((screenHeight + texelSize.height - 1) / texelSize.height, 1u);

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = VK_FORMAT_R8_UINT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;


	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image, &allocation, nullptr) != VK_SUCCESS) {
		throw std::runtime_error("failed to create VRS image!");
	}
	vmaSetAllocationName(allocator, allocation, "VRS Shading Rate Image");

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8_UINT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;


	if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
		throw std::runtime_error("failed to create VRS image view!");
	}
}


void VulkanVRS::Destroy(VkDevice device, VmaAllocator allocator) {
	if (view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, view, nullptr);
		view = VK_NULL_HANDLE;
	}

	if (image != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, image, allocation);
		image = VK_NULL_HANDLE;
	}
}
