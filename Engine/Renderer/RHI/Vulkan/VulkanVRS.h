#pragma once

#include "VulkanCommonIncludes.h"

struct VulkanVRS {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkExtent2D texelSize = { 0, 0 };

    void Create(VkDevice device, VkPhysicalDevice physicalDevice, VmaAllocator allocator, uint32_t screenWidth, uint32_t screenHeight);
    void Destroy(VkDevice device, VmaAllocator allocator);
};
