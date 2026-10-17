// VulkanHiZ.cpp — Vulkan Hierarchical Z-Buffer Implementation
//
// Implements the creation and management of the Hi-Z pyramid image and its associated Vulkan resources.
// It provides the backend logic for building depth-reduction mipmaps and releasing resources during shutdown or resizing.
// This module is essential for implementing GPU-driven occlusion culling and other visibility optimizations.

#include "VulkanHiZ.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

VulkanHiZOcclusionSchedule BuildVulkanHiZOcclusionSchedule(
	const VulkanHiZHistoryMetadata& history,
	const VulkanHiZValidationInput& input) noexcept
{
	VulkanHiZOcclusionSchedule schedule{};
	// A post pass may consume only a current, ready pyramid. The main pass stays
	// scheduled after every invalidation so conservative visible coverage is
	// rebuilt instead of accidentally sampling stale depth.
	schedule.runPostPass = input.pyramidReady;
	schedule.reusesHistory = history.valid
		&& history.viewId == input.viewId
		&& history.viewClass == input.viewClass
		&& history.targetId == input.targetId
		&& history.width == input.width
		&& history.height == input.height
		&& history.projectionSignature == input.projectionSignature
		&& history.depthSceneSignature == input.depthSceneSignature
		&& !input.cameraCut;
	if (schedule.reusesHistory)
	{
		schedule.invalidation = VulkanHiZInvalidReason::None;
	}
	else if (!input.pyramidReady)
	{
		schedule.invalidation = VulkanHiZInvalidReason::MissingPyramid;
	}
	else if (input.cameraCut)
	{
		schedule.invalidation = VulkanHiZInvalidReason::CameraCut;
	}
	else if (!history.valid)
	{
		schedule.invalidation = VulkanHiZInvalidReason::FirstFrame;
	}
	else if (history.width != input.width || history.height != input.height)
	{
		schedule.invalidation = VulkanHiZInvalidReason::Resize;
	}
	else if (history.projectionSignature != input.projectionSignature)
	{
		schedule.invalidation = VulkanHiZInvalidReason::ProjectionChanged;
	}
	else if (history.depthSceneSignature != input.depthSceneSignature)
	{
		schedule.invalidation = VulkanHiZInvalidReason::DepthSceneChanged;
	}
	else
	{
		schedule.invalidation = VulkanHiZInvalidReason::ViewMismatch;
	}
	return schedule;
}

void VulkanHiZ::Create(VkDevice device, VmaAllocator allocator, uint32_t screenWidth, uint32_t screenHeight, VkFormat /*ignored*/) {
	// Swapchain creation can briefly report a zero-sized drawable during
	// minimized or export-startup transitions. Vulkan images must still use
	// non-zero extents, so clamp the backing pyramid to the smallest legal size.
	width = std::max(screenWidth, 1u);
	height = std::max(screenHeight, 1u);
	mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
	history = {};
	history.width = width;
	history.height = height;
	history.warmupFrames = 0u;
	history.invalidReason = VulkanHiZInvalidReason::Resize;
	latestInvalidReason = VulkanHiZInvalidReason::Resize;
	latestValidation = {};
	latestValidation.width = width;
	latestValidation.height = height;
	latestValidation.pyramidReady = false;
	latestValidation.invalidReason = VulkanHiZInvalidReason::Resize;
	// Creation starts with a diagnostic fail-open state until the current frame
	// builds a ready pyramid. This preserves the accepted Hi-Z baseline: only a
	// missing pyramid disables occlusion, and all other metadata remains
	// diagnostic once a current pyramid exists.
	latestValidation.failOpen = true;
	latestOcclusionAllowed = false;
	layoutInitialized = false;
	layoutTransitionRecorded = false;

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.format = VK_FORMAT_R32_SFLOAT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;


	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image, &allocation, nullptr) != VK_SUCCESS) {
		throw std::runtime_error("failed to create Hi-Z image!");
	}
	vmaSetAllocationName(allocator, allocation, "Hi-Z Pyramid Image");

	{
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R32_SFLOAT;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = mipLevels;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;


		if (vkCreateImageView(device, &viewInfo, nullptr, &fullView) != VK_SUCCESS) {
			throw std::runtime_error("failed to create Hi-Z full image view!");
		}
	}

	mipViews.resize(mipLevels);

	for (uint32_t i = 0; i < mipLevels; i++) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R32_SFLOAT;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = i;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;


		if (vkCreateImageView(device, &viewInfo, nullptr, &mipViews[i]) != VK_SUCCESS) {
			throw std::runtime_error("failed to create Hi-Z image view!");
		}
	}

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = static_cast<float>(mipLevels);


	if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
		throw std::runtime_error("failed to create Hi-Z sampler!");
	}
}


void VulkanHiZ::Destroy(VkDevice device, VmaAllocator allocator) {
	if (fullView != VK_NULL_HANDLE) {
		vkDestroyImageView(device, fullView, nullptr);
		fullView = VK_NULL_HANDLE;
	}

	if (sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, sampler, nullptr);
		sampler = VK_NULL_HANDLE;
	}

	for (auto view : mipViews) {
		vkDestroyImageView(device, view, nullptr);
	}
	mipViews.clear();


	if (image != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, image, allocation);
		image = VK_NULL_HANDLE;
	}
	history = {};
	history.warmupFrames = 0u;
	history.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
	latestInvalidReason = VulkanHiZInvalidReason::MissingPyramid;
	latestValidation = {};
	latestValidation.pyramidReady = false;
	latestValidation.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
	// Destroying the pyramid must never leave a stale "active" result behind.
	// Consumers see an explicit missing-pyramid fail-open state and keep
	// geometry visible until a fresh pyramid is published.
	latestValidation.failOpen = true;
	latestOcclusionAllowed = false;
	layoutInitialized = false;
	layoutTransitionRecorded = false;
}
