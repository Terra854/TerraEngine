// VulkanLightingResources.cpp — Shadow, light-analysis, and culling resources
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"

// VulkanContext::Impl::EnsureShadowRenderResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::EnsureShadowRenderResources(const ShadowResourceRequest& request) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	ShadowResourceRequest targetRequest{};
	targetRequest.directionalCount = std::min(request.directionalCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.spotCount = std::min(request.spotCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.areaCount = std::min(request.areaCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.pointCount = std::min(request.pointCount, MAX_SHADOW_RESOURCE_SLOTS);
	// Scene and Game views can request different shadow capacities in one CPU
	// frame. Publish and complete every earlier recorded view before replacing
	// framebuffers, image views, or descriptor bindings that those views own.
	WaitForDescriptorMutationIdle();
	if (deviceLost) {
		return false;
	}
	const VkResult idleResult = WaitDeviceIdle();
	if (MarkDeviceLost(idleResult, "shadow render resource rebuild device idle")) {
		return false;
	}
	if (idleResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping shadow render resource rebuild because the device did not become idle: " << idleResult);
		return false;
	}
	// Rebuilding this block (for example after swapchain recreation) must tear
	// down stale views/framebuffers before allocating replacement images.
	auto DestroyFramebuffer = [&](VkFramebuffer& framebuffer) {
		if (framebuffer != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : directionalShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : spotShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : areaShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkFramebuffer, 6>& faceFramebuffers : pointShadowFaceFramebuffersBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFramebuffer& framebuffer : faceFramebuffers) {
			DestroyFramebuffer(framebuffer);
		}
	}

	auto DestroyImageView = [&](VkImageView& imageView) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (imageView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, imageView, nullptr);
			imageView = VK_NULL_HANDLE;
		}
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkImageView, 6>& faceViews : pointShadowDepthFaceViewsBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkImageView, 6>& faceViews : pointShadowColorFaceViewsBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}

	auto DestroyImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
			image.sampler = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
			image.view = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image.image = VK_NULL_HANDLE;
		image.allocation = VK_NULL_HANDLE;
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowColors) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowColors) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowColors) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeColors) {
		DestroyImage(image);
	}
	activeShadowResourceRequest = {};
	shadowStorageRegions.fill({});
	shadowCacheEntries.clear();
	DestroyImage(ltcAmplitudeTexture);
	DestroyImage(ltcInverseTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (targetRequest.TotalCount() == 0u) {
		return true;
	}

	const VkFormat shadowDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
	const uint32_t pointShadowSize = POINT_SHADOW_MAP_RESOLUTION;
	const auto Shadow2DRegionSizeForSlot = [](std::uint32_t typedSlot) {
		// Lower-priority 2D shadows use physically smaller images. This is the
		// memory-saving step; diagnostics and viewport state mirror this size.
		return typedSlot >= 16u ? SHADOW_MAP_RESOLUTION / 2u : SHADOW_MAP_RESOLUTION;
	};

	auto CreateColorAttachment = [&](uint32_t width, uint32_t height, uint32_t layers, VkImageCreateFlags flags, VulkanImage& image, VkImageViewType viewType) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = layers;
		imageInfo.format = swapchainImageFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.flags = flags;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Color Image");
		// Shadow color is reused as an attachment every frame. Initialize it once
		// instead of asking each render pass to discard an unknown prior layout.
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1u, layers)) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image.image;
		viewInfo.viewType = viewType;
		viewInfo.format = imageInfo.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = layers;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateImageView(device, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			image.view = VK_NULL_HANDLE;
		}
	};

	auto CreateDepthAttachment2D = [&](uint32_t width, uint32_t height, VulkanImage& image) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = shadowDepthFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Depth Image");
		// The shadow pass finishes in read-only layout for material sampling. Start
		// the first frame in that same persistent layout so first and later uses
		// follow one render-pass contract.
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_DEPTH_BIT);
	};

	auto CreateDepthAttachmentCube = [&](uint32_t size, VulkanImage& image) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = size;
		imageInfo.extent.height = size;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 6;
		imageInfo.format = shadowDepthFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Cube Depth Image");
		// Transition every cubemap face. Each face has its own framebuffer, but all
		// six layers must agree with the shadow render pass before any face is used.
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 1u, 6u)) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		viewInfo.format = imageInfo.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 6;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateImageView(device, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			image.view = VK_NULL_HANDLE;
		}
	};

	auto CreateShadowSampler = [&](VulkanImage& image, bool useCompareSampler) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image == VK_NULL_HANDLE) {
			return;
		}

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.compareEnable = useCompareSampler ? VK_TRUE : VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler);
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
		CreateColorAttachment(regionSize, regionSize, 1u, 0u, directionalShadowColors[slotIndex], VK_IMAGE_VIEW_TYPE_2D);
		CreateDepthAttachment2D(regionSize, regionSize, directionalShadowDepths[slotIndex]);
		CreateShadowSampler(directionalShadowDepths[slotIndex], false);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
		CreateColorAttachment(regionSize, regionSize, 1u, 0u, spotShadowColors[slotIndex], VK_IMAGE_VIEW_TYPE_2D);
		CreateDepthAttachment2D(regionSize, regionSize, spotShadowDepths[slotIndex]);
		CreateShadowSampler(spotShadowDepths[slotIndex], false);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
		const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
		CreateColorAttachment(regionSize, regionSize, 1u, 0u, areaShadowColors[slotIndex], VK_IMAGE_VIEW_TYPE_2D);
		CreateDepthAttachment2D(regionSize, regionSize, areaShadowDepths[slotIndex]);
		CreateShadowSampler(areaShadowDepths[slotIndex], false);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex) {
		CreateColorAttachment(pointShadowSize, pointShadowSize, 6u, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, pointShadowCubeColors[slotIndex], VK_IMAGE_VIEW_TYPE_CUBE);
		CreateDepthAttachmentCube(pointShadowSize, pointShadowCubeDepths[slotIndex]);
		CreateShadowSampler(pointShadowCubeDepths[slotIndex], false);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
			VkImageViewCreateInfo depthFaceViewInfo{};
			depthFaceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			depthFaceViewInfo.image = pointShadowCubeDepths[slotIndex].image;
			depthFaceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			depthFaceViewInfo.format = shadowDepthFormat;
			depthFaceViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			depthFaceViewInfo.subresourceRange.baseMipLevel = 0;
			depthFaceViewInfo.subresourceRange.levelCount = 1;
			depthFaceViewInfo.subresourceRange.baseArrayLayer = faceIndex;
			depthFaceViewInfo.subresourceRange.layerCount = 1;
			vkCreateImageView(device, &depthFaceViewInfo, nullptr, &pointShadowDepthFaceViewsBySlot[slotIndex][faceIndex]);

			VkImageViewCreateInfo colorFaceViewInfo{};
			colorFaceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			colorFaceViewInfo.image = pointShadowCubeColors[slotIndex].image;
			colorFaceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorFaceViewInfo.format = swapchainImageFormat;
			colorFaceViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorFaceViewInfo.subresourceRange.baseMipLevel = 0;
			colorFaceViewInfo.subresourceRange.levelCount = 1;
			colorFaceViewInfo.subresourceRange.baseArrayLayer = faceIndex;
			colorFaceViewInfo.subresourceRange.layerCount = 1;
			vkCreateImageView(device, &colorFaceViewInfo, nullptr, &pointShadowColorFaceViewsBySlot[slotIndex][faceIndex]);
		}
	}

	auto BindDepthTexture = [&](uint32_t index, const VulkanImage& image) {
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		imageInfo.imageView = image.view;
		imageInfo.sampler = image.sampler;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			VkWriteDescriptorSet descriptorWrite{};
			descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrite.dstSet = globalDescriptorSets[i];
			descriptorWrite.dstBinding = 0;
			descriptorWrite.dstArrayElement = index;
			descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			descriptorWrite.descriptorCount = 1;
			descriptorWrite.pImageInfo = &imageInfo;
			vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
		}
	};

	// Register only the active 2D shadow regions into the bindless texture
	// heap. Each light stores the resolved texture index after CPU assignment.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		directionalShadowTextureIndices[slotIndex] = AllocateTextureIndex();
		BindDepthTexture(directionalShadowTextureIndices[slotIndex], directionalShadowDepths[slotIndex]);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		spotShadowTextureIndices[slotIndex] = AllocateTextureIndex();
		BindDepthTexture(spotShadowTextureIndices[slotIndex], spotShadowDepths[slotIndex]);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
		areaShadowTextureIndices[slotIndex] = AllocateTextureIndex();
		BindDepthTexture(areaShadowTextureIndices[slotIndex], areaShadowDepths[slotIndex]);
	}

	// Point shadows use a dedicated cubemap binding array because the global
	// bindless heap is declared as Texture2D in shaders.
	if (targetRequest.pointCount > 0u) {
		std::vector<VkDescriptorImageInfo> pointShadowCubeImageInfos(targetRequest.pointCount);
		for (uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex) {
			pointShadowCubeImageInfos[slotIndex].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			pointShadowCubeImageInfos[slotIndex].imageView = pointShadowCubeDepths[slotIndex].view;
			pointShadowCubeImageInfos[slotIndex].sampler = VK_NULL_HANDLE;
		}

		VkDescriptorImageInfo pointShadowCubeSamplerInfo{};
		pointShadowCubeSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		pointShadowCubeSamplerInfo.imageView = VK_NULL_HANDLE;
		pointShadowCubeSamplerInfo.sampler = pointShadowCubeDepths[0].sampler;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			std::array<VkWriteDescriptorSet, 2> pointShadowCubeWrites{};
			pointShadowCubeWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			pointShadowCubeWrites[0].dstSet = globalDescriptorSets[i];
			pointShadowCubeWrites[0].dstBinding = 20;
			pointShadowCubeWrites[0].dstArrayElement = 0;
			pointShadowCubeWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			pointShadowCubeWrites[0].descriptorCount = targetRequest.pointCount;
			pointShadowCubeWrites[0].pImageInfo = pointShadowCubeImageInfos.data();

			pointShadowCubeWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			pointShadowCubeWrites[1].dstSet = globalDescriptorSets[i];
			pointShadowCubeWrites[1].dstBinding = 8;
			pointShadowCubeWrites[1].dstArrayElement = 0;
			pointShadowCubeWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			pointShadowCubeWrites[1].descriptorCount = 1;
			pointShadowCubeWrites[1].pImageInfo = &pointShadowCubeSamplerInfo;

			vkUpdateDescriptorSets(device, static_cast<uint32_t>(pointShadowCubeWrites.size()), pointShadowCubeWrites.data(), 0, nullptr);
		}
	}
	activeShadowResourceRequest = targetRequest;
	shadowStorageRegions.fill({});
	const auto estimateRegionBytes = [&](ShadowResourceKind kind) {
		constexpr std::uint64_t kDepthBytesPerPixel = 4u;
		constexpr std::uint64_t kColorBytesPerPixel = 4u;
		const std::uint64_t texelCount = static_cast<std::uint64_t>(SHADOW_MAP_RESOLUTION) * static_cast<std::uint64_t>(SHADOW_MAP_RESOLUTION);
		const std::uint64_t pointTexelCount = static_cast<std::uint64_t>(POINT_SHADOW_MAP_RESOLUTION) * static_cast<std::uint64_t>(POINT_SHADOW_MAP_RESOLUTION) * 6u;
		return (kind == ShadowResourceKind::PointCube ? pointTexelCount : texelCount) * (kDepthBytesPerPixel + kColorBytesPerPixel);
	};
	std::uint32_t regionIndex = 0u;
	auto registerRegion = [&](ShadowResourceKind kind, std::uint32_t typedSlot) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (regionIndex >= shadowStorageRegions.size()) {
			return;
		}
		const bool reduced2DRegion = kind != ShadowResourceKind::PointCube && typedSlot >= 16u;
		ShadowStorageRegion& region = shadowStorageRegions[regionIndex++];
		region.resourceKind = kind;
		region.typedSlot = typedSlot;
		region.atlasId = static_cast<std::uint32_t>(kind);
		region.x = 0u;
		region.y = 0u;
		region.width = reduced2DRegion ? Shadow2DRegionSizeForSlot(typedSlot) : SHADOW_MAP_RESOLUTION;
		region.height = reduced2DRegion ? Shadow2DRegionSizeForSlot(typedSlot) : SHADOW_MAP_RESOLUTION;
		region.qualityTier = reduced2DRegion ? ShadowQualityTier::ReducedResolution : ShadowQualityTier::FullResolution;
		region.estimatedBytes = estimateRegionBytes(kind);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (reduced2DRegion) {
			region.estimatedBytes /= 4u;
		}
		region.live = true;
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::DirectionalCascade, slotIndex);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::SpotAtlas, slotIndex);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::AreaAtlas, slotIndex);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::PointCube, slotIndex);
	}

	// Build lightweight procedural LTC lookup textures. These are deterministic
	// fallback tables until authored LUT assets are imported through content.
	auto CreateLtcLookupTexture = [&](const std::vector<glm::vec4>& texels, VulkanImage& image) {
		if (texels.empty()) {
			return;
		}

		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(sizeof(glm::vec4) * texels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingBuffer, texels.data(), uploadSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = LTC_LUT_RESOLUTION;
		imageInfo.extent.height = LTC_LUT_RESOLUTION;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}

		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, image.image, LTC_LUT_RESOLUTION, LTC_LUT_RESOLUTION);
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler);
	};

	const uint32_t lutSize = LTC_LUT_RESOLUTION;
	std::vector<glm::vec4> amplitudeLut(lutSize * lutSize, glm::vec4(1.0f));
	std::vector<glm::vec4> inverseLut(lutSize * lutSize, glm::vec4(0.0f));
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t y = 0u; y < lutSize; ++y) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t x = 0u; x < lutSize; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(lutSize);
			const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(lutSize);
			const float roughness = std::clamp(v, 0.02f, 1.0f);
			const float fresnelScale = std::clamp(1.0f - 0.5f * roughness, 0.05f, 1.0f);
			amplitudeLut[y * lutSize + x] = glm::vec4(fresnelScale, fresnelScale, fresnelScale, 1.0f);
			inverseLut[y * lutSize + x] = glm::vec4(1.0f, 0.0f, 0.0f, u);
		}
	}

	CreateLtcLookupTexture(amplitudeLut, ltcAmplitudeTexture);
	CreateLtcLookupTexture(inverseLut, ltcInverseTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ltcAmplitudeTexture.image != VK_NULL_HANDLE) {
		ltcAmplitudeTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(ltcAmplitudeTextureIndex, ltcAmplitudeTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ltcInverseTexture.image != VK_NULL_HANDLE) {
		ltcInverseTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(ltcInverseTextureIndex, ltcInverseTexture);
	}

	return true;
}

// VulkanContext::Impl::UploadLightingData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UploadLightingData(const std::vector<GPULight>& lights) {
	if (globalLightSSBOs.empty() || globalLightSSBOs[currentFrame].buffer == VK_NULL_HANDLE) {
		return;
	}

	const uint32_t clampedLightCount = static_cast<uint32_t>(std::min<size_t>(lights.size(), MAX_GLOBAL_LIGHTS));
	const uint32_t lightOffset = activeRenderSnapshot.valid ? activeRenderSnapshot.lightDataOffset : 0u;

	void* mappedLights = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalLightSSBOs[currentFrame].allocation, &mappedLights) == VK_SUCCESS) {
		auto* gpuLights = static_cast<GPULight*>(mappedLights) + lightOffset;
		std::fill(gpuLights, gpuLights + MAX_GLOBAL_LIGHTS, GPULight{});
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clampedLightCount > 0u) {
			std::memcpy(gpuLights, lights.data(), sizeof(GPULight) * clampedLightCount);
		}
		const VkDeviceSize byteOffset = static_cast<VkDeviceSize>(lightOffset) * sizeof(GPULight);
		const VkDeviceSize byteSize = static_cast<VkDeviceSize>(MAX_GLOBAL_LIGHTS) * sizeof(GPULight);
		vmaFlushAllocation(allocator, globalLightSSBOs[currentFrame].allocation, byteOffset, byteSize);
		vmaUnmapMemory(allocator, globalLightSSBOs[currentFrame].allocation);
	}
}

// VulkanContext::Impl::UploadLightAnalysisInputData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UploadLightAnalysisInputData(const std::vector<GPULight>& lights) {
	if (lightAnalysisInputSSBOs.empty() || lightAnalysisInputSSBOs[currentFrame].buffer == VK_NULL_HANDLE) {
		return;
	}

	const uint32_t clampedLightCount = static_cast<uint32_t>(std::min<size_t>(lights.size(), MAX_GLOBAL_LIGHTS));
	const uint32_t lightOffset = activeRenderSnapshot.valid ? activeRenderSnapshot.lightAnalysisInputOffset : 0u;

	void* mappedLights = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, lightAnalysisInputSSBOs[currentFrame].allocation, &mappedLights) == VK_SUCCESS) {
		auto* gpuLights = static_cast<GPULight*>(mappedLights) + lightOffset;
		std::fill(gpuLights, gpuLights + MAX_GLOBAL_LIGHTS, GPULight{});
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clampedLightCount > 0u) {
			std::memcpy(gpuLights, lights.data(), sizeof(GPULight) * clampedLightCount);
		}
		const VkDeviceSize byteOffset = static_cast<VkDeviceSize>(lightOffset) * sizeof(GPULight);
		const VkDeviceSize byteSize = static_cast<VkDeviceSize>(MAX_GLOBAL_LIGHTS) * sizeof(GPULight);
		vmaFlushAllocation(allocator, lightAnalysisInputSSBOs[currentFrame].allocation, byteOffset, byteSize);
		vmaUnmapMemory(allocator, lightAnalysisInputSSBOs[currentFrame].allocation);
	}
}

// VulkanContext::Impl::ResetMeshletCounters is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::ResetMeshletCounters(uint32_t statsSlotIndex) {
	if (globalMeshletCountersSSBOs.empty() || globalMeshletCountersSSBOs[currentFrame].buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return;
	}

	MeshletCullingCountersGPU counters{};
	void* mapped = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, &mapped) != VK_SUCCESS) {
		return;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(MeshletCullingCountersGPU);
	std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &counters, sizeof(MeshletCullingCountersGPU));
	vmaFlushAllocation(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, offset, sizeof(MeshletCullingCountersGPU));
	vmaUnmapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation);
}

// VulkanContext::Impl::UpdateFrameCullingData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateFrameCullingData(uint32_t statsSlotIndex, const FrameCullingDataGPU& frameData) {
	if (globalFrameCullingSSBOs.empty() || globalFrameCullingSSBOs[currentFrame].buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return;
	}

	void* mapped = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalFrameCullingSSBOs[currentFrame].allocation, &mapped) != VK_SUCCESS) {
		return;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(FrameCullingDataGPU);
	std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &frameData, sizeof(FrameCullingDataGPU));
	vmaFlushAllocation(allocator, globalFrameCullingSSBOs[currentFrame].allocation, offset, sizeof(FrameCullingDataGPU));
	vmaUnmapMemory(allocator, globalFrameCullingSSBOs[currentFrame].allocation);
}

// VulkanContext::Impl::ReadMeshletCounters is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
MeshletCullingCountersGPU VulkanContext::Impl::ReadMeshletCounters(uint32_t statsSlotIndex) const {
	MeshletCullingCountersGPU counters{};
	if (globalMeshletCountersSSBOs.empty() || globalMeshletCountersSSBOs[currentFrame].buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return counters;
	}

	void* mapped = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, &mapped) != VK_SUCCESS) {
		return counters;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(MeshletCullingCountersGPU);
	vmaInvalidateAllocation(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, offset, sizeof(MeshletCullingCountersGPU));
	std::memcpy(&counters, static_cast<const std::uint8_t*>(mapped) + offset, sizeof(MeshletCullingCountersGPU));
	vmaUnmapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation);
	return counters;
}

// VulkanContext::Impl::GetStatsSlotIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::GetStatsSlotIndex(uint32_t frameIndex, VulkanContext::ViewType view) const {
	const uint32_t clampedFrameIndex = std::min(frameIndex, kMaxStatsFrameSlots - 1u);
	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	return clampedFrameIndex * kStatsViewCount + viewIndex;
}

