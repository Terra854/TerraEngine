#pragma once

#include "VulkanCommonIncludes.h"
#include <vector>

struct VulkanHiZ {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageView fullView = VK_NULL_HANDLE;
    std::vector<VkImageView> mipViews;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;

    void Create(VkDevice device, VmaAllocator allocator, uint32_t screenWidth, uint32_t screenHeight, VkFormat format);
    void Destroy(VkDevice device, VmaAllocator allocator);
};
