// VulkanPostProcessLookups.h — immutable, independently owned grading images.
#pragma once
#include "VulkanPostProcessTone.h"
#include <array>
#include <memory>

struct Texture;
namespace TerraEngine::Renderer::Vulkan
{
	// Dirt is an independently retained standalone upload. It never aliases an
	// evictable material texture or the curve/LUT allocation lifecycle.
	struct PostProcessDirtImage
	{
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=VK_NULL_HANDLE;
		PostProcessTargetImage image;
		std::shared_ptr<const Texture> sourceOwner;
		PostProcessDirtImage()=default;
		~PostProcessDirtImage();
		PostProcessDirtImage(const PostProcessDirtImage&)=delete;
		PostProcessDirtImage& operator=(const PostProcessDirtImage&)=delete;
	};
	// Tone descriptors borrow these views. Retain this owner with the prepared
	// pass until its real submissions retire, and release it before its allocator
	// or device. These allocations never enter the evictable material cache.
	struct PostProcessLookupImages
	{
		ToneLookupTextures views;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		// Slot zero is curve; slot one is the horizontal tiled cubic LUT.
		std::array<PostProcessTargetImage, 2> images;
		std::array<std::shared_ptr<const Texture>, 2> sourceOwners;
		PostProcessLookupImages() = default;
		~PostProcessLookupImages();
		PostProcessLookupImages(const PostProcessLookupImages&) = delete;
		PostProcessLookupImages& operator=(const PostProcessLookupImages&) = delete;
	};
}
