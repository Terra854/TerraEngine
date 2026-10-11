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
	if (!WaitForDescriptorMutationIdle()) return false;
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
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Static framebuffers borrow these color views. Idle completes accepted GPU
	// work but cannot abandon a recorded primary that might still be submitted.
	if (!ReleaseStaticShadowEntriesBeforeColorReplacement()) return false;
	// The old slot numbers may already be present in prepared light payloads.
	// Make every retired descriptor safe before freeing its native owner, then
	// preserve those semantic assignments when replacement images are bound.
	std::vector<std::uint32_t> retiredSlots;
	retiredSlots.reserve(3U * MAX_SHADOW_RESOURCE_SLOTS + 2U);
	for (const auto& indices : {directionalShadowTextureIndices, spotShadowTextureIndices, areaShadowTextureIndices})
		for (auto index : indices) if (index != 0U) retiredSlots.push_back(index);
	if (ltcAmplitudeTextureIndex != 0U) retiredSlots.push_back(ltcAmplitudeTextureIndex);
	if (ltcInverseTextureIndex != 0U) retiredSlots.push_back(ltcInverseTextureIndex);
	if (!RetireGlobalTextureDescriptorSlotsAfterIdle(retiredSlots)) return false;
	// Rebuilding this block (for example after swapchain recreation) must tear
	// down stale views/framebuffers before allocating replacement images.
	// Revoke submitted and recorded claims before the first physical allocation
	// changes; partial replacement must never retain proof for destroyed depth.
	ResetShadowPublication(true);
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
	DestroyImage(ltcAmplitudeTexture);
	DestroyImage(ltcInverseTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (targetRequest.TotalCount() == 0u) {
		return true;
	}

	const VkFormat shadowDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
	// Validation copies complete native depth images only when the chosen format
	// supports transfer reads. Unsupported formats keep normal production usage.
	VkFormatProperties shadowFormatProperties{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, shadowDepthFormat, &shadowFormatProperties);
	const VkImageUsageFlags shadowTransferUsage =
		(shadowFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0U
		? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0U;
	const VkImageUsageFlags shadowCopyDestinationUsage =
		(shadowFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0U
		? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0U;
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
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | shadowTransferUsage | shadowCopyDestinationUsage;
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
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | shadowTransferUsage | shadowCopyDestinationUsage;
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
	const auto retainSlot = [&](std::uint32_t& index) {
		index = TerraEngine::Renderer::Vulkan::AcquireRetainedTextureDescriptorSlot(
			index, MAX_BINDLESS_TEXTURES, [&] { return AllocateTextureIndex(); });
		return index != 0U;
	};
	const auto depthOwnerReady = [](const VulkanImage& image) {
		return image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE
			&& image.view != VK_NULL_HANDLE && image.sampler != VK_NULL_HANDLE;
	};
	// Validate all active owners and slot assignments before publishing any new
	// descriptor. A failed replacement leaves inactive/old slots fallback-bound.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex)
		if (!depthOwnerReady(directionalShadowDepths[slotIndex]) || !retainSlot(directionalShadowTextureIndices[slotIndex])) return false;
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex)
		if (!depthOwnerReady(spotShadowDepths[slotIndex]) || !retainSlot(spotShadowTextureIndices[slotIndex])) return false;
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex)
		if (!depthOwnerReady(areaShadowDepths[slotIndex]) || !retainSlot(areaShadowTextureIndices[slotIndex])) return false;
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex)
		if (!depthOwnerReady(pointShadowCubeDepths[slotIndex])) return false;
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		BindDepthTexture(directionalShadowTextureIndices[slotIndex], directionalShadowDepths[slotIndex]);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		BindDepthTexture(spotShadowTextureIndices[slotIndex], spotShadowDepths[slotIndex]);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
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
	auto CreateLtcLookupTexture = [&](const std::vector<glm::vec4>& texels, VulkanImage& image) -> bool {
		if (texels.empty()) {
			return false;
		}

		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(sizeof(glm::vec4) * texels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		struct StagingOwner {
			VmaAllocator allocator; VulkanBuffer& buffer;
			~StagingOwner() { if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation); }
		} stagingOwner{allocator, stagingBuffer};
		if (stagingBuffer.buffer == VK_NULL_HANDLE || !WriteMappedBuffer(allocator, stagingBuffer, texels.data(), uploadSize)) return false;

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
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return false;
		}

		ambiguousUploadBuffers.reserve(ambiguousUploadBuffers.size() + 1U);
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			|| !CopyBufferToImage(stagingBuffer.buffer, image.image, LTC_LUT_RESOLUTION, LTC_LUT_RESOLUTION)
			|| !TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
			// A failed completion proof must retain possibly submitted storage.
			RetainAmbiguousUpload(stagingBuffer);
			return false;
		}

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);
		if (image.view == VK_NULL_HANDLE) return false;

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
		return vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler) == VK_SUCCESS;
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

	if (!CreateLtcLookupTexture(amplitudeLut, ltcAmplitudeTexture)
		|| !CreateLtcLookupTexture(inverseLut, ltcInverseTexture)) return false;
	if (!retainSlot(ltcAmplitudeTextureIndex) || !retainSlot(ltcInverseTextureIndex)) return false;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ltcAmplitudeTexture.image != VK_NULL_HANDLE) {
		WriteGlobalTextureAlias(ltcAmplitudeTextureIndex, ltcAmplitudeTexture.view, ltcAmplitudeTexture.sampler);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ltcInverseTexture.image != VK_NULL_HANDLE) {
		WriteGlobalTextureAlias(ltcInverseTextureIndex, ltcInverseTexture.view, ltcInverseTexture.sampler);
	}

	// Only fully uploaded and rebound replacements become the active request.
	activeShadowResourceRequest = targetRequest;
	return true;
}

// Resets revoke reuse proof without discarding last successful diagnostic bytes.
// Allocation/session identity additionally prevents handle reuse from aliasing.
void VulkanContext::Impl::ResetLightingUploadMirrors()
{
	for (auto& frame : lightingUploadMirrors)
		for (auto& snapshot : frame)
			for (auto& mirror : snapshot)
				TerraEngine::Renderer::VulkanLightingUpload::InvalidateLightingMirror(mirror);
	lightingUploadFrameSlotSafe.fill(false);
}

bool VulkanContext::Impl::UploadLightingData(
	const TerraEngine::Renderer::VulkanLightingUpload::CanonicalBlock& lights,
	RenderViewType view, std::uint32_t output, bool forceFull,
	TerraEngine::Renderer::RendererEfficiencyLightingUploadEvidence& evidence)
{
	return UploadCanonicalLighting(lights, TerraEngine::Renderer::VulkanLightingUpload::Purpose::FinalLight,
		view, output, forceFull, evidence);
}

bool VulkanContext::Impl::UploadLightAnalysisInputData(
	const TerraEngine::Renderer::VulkanLightingUpload::CanonicalBlock& lights,
	RenderViewType view, std::uint32_t output, bool forceFull,
	TerraEngine::Renderer::RendererEfficiencyLightingUploadEvidence& evidence)
{
	return UploadCanonicalLighting(lights, TerraEngine::Renderer::VulkanLightingUpload::Purpose::AnalysisInput,
		view, output, forceFull, evidence);
}

bool VulkanContext::Impl::UploadCanonicalLighting(
	const TerraEngine::Renderer::VulkanLightingUpload::CanonicalBlock& lights,
	TerraEngine::Renderer::VulkanLightingUpload::Purpose purpose, RenderViewType view,
	std::uint32_t output, bool forceFull,
	TerraEngine::Renderer::RendererEfficiencyLightingUploadEvidence& evidence)
{
	using namespace TerraEngine::Renderer::VulkanLightingUpload;
	static_assert(sizeof(GPULight) == kRecordBytes && sizeof(CanonicalRecord) == kRecordBytes,
		"Canonical light bytes must preserve the shader's existing 176-byte ABI.");
	const auto started = std::chrono::steady_clock::now();
	evidence = {}; evidence.analysisPurpose = purpose == Purpose::AnalysisInput;
	const auto finish = [&](bool success, const char* reason) {
		evidence.succeeded = success; evidence.reason = reason;
		evidence.mirrorCpuDurationNanoseconds = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
		return success;
	};
	const auto slot = activeRenderSnapshot.slotIndex;
	auto& buffers = purpose == Purpose::FinalLight ? globalLightSSBOs : lightAnalysisInputSSBOs;
	if (!activeRenderSnapshot.valid || currentFrame >= buffers.size() || slot >= MAX_RENDER_SNAPSHOTS_PER_FRAME)
		return finish(false, "Physical lighting snapshot is unavailable.");
	auto& mirror = lightingUploadMirrors[currentFrame][slot][purpose == Purpose::FinalLight ? 0 : 1];
	auto& buffer = buffers[currentFrame];
	if (!buffer.buffer || !buffer.allocation || !lightingUploadFrameSlotSafe[currentFrame]) {
		InvalidateLightingMirror(mirror);
		return finish(false, "Lighting allocation or frame-slot completion proof is unavailable.");
	}
	VmaAllocationInfo allocation{};
	vmaGetAllocationInfo(allocator, buffer.allocation, &allocation);
	VkMemoryPropertyFlags properties = 0;
	vmaGetAllocationMemoryProperties(allocator, buffer.allocation, &properties);
	const std::uint64_t offset = static_cast<std::uint64_t>(slot) * kBlockBytes;
	if (offset > buffer.size || kBlockBytes > buffer.size - offset) {
		InvalidateLightingMirror(mirror); return finish(false, "Lighting slice exceeds buffer capacity.");
	}
	PhysicalOwner owner{reinterpret_cast<std::uintptr_t>(buffer.allocation), lightingUploadAllocationGeneration,
		renderSessionGeneration, currentFrame, slot, purpose, view, output, offset, kBlockBytes};
	MappingBounds bounds{allocation.size, 0, allocation.size, lightingUploadAtomSize,
		(properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0};
	// Large fixed blocks live on the heap to protect Windows' default 1-MiB
	// render-thread stack. Full mode uses a detached uninitialized prior so the
	// same validated upload/commit path performs a complete reference write.
	// Planning stays beside mirror validation here because the current owner and
	// revision must remain locked through commit; payload serialization above is
	// the independent worker-safe part. The policy itself remains pure and tested.
	auto plan = std::make_unique<DirtyRangePlan>();
	std::unique_ptr<Mirror> fullPrior;
	if (forceFull) { fullPrior = std::make_unique<Mirror>(); fullPrior->revision = mirror.revision; }
	if (!BuildLightingDirtyRangePlan(owner, bounds, lights, forceFull ? *fullPrior : mirror, *plan)) {
		InvalidateLightingMirror(mirror); return finish(false, "Lighting mapping or aligned range is invalid.");
	}
	evidence.available = true; evidence.coherent = bounds.coherent;
	evidence.allocationId = owner.allocationId; evidence.allocationGeneration = owner.allocationGeneration;
	evidence.allocationSizeBytes = allocation.size; evidence.nonCoherentAtomSize = lightingUploadAtomSize;
	evidence.regionOffsetBytes = owner.regionOffsetBytes; evidence.regionSizeBytes = owner.regionSizeBytes;
	evidence.frameSlot = currentFrame; evidence.snapshotSlot = slot; evidence.activeRecordCount = lights.activeCount;
	// No-op reuse needs no map or flush. It still verifies current ownership and
	// fence safety at the protected boundary before confirming the mirror.
	UploadReceipt receipt{owner, mirror.revision, true, true, false, false, bounds.coherent};
	if (plan->logicalRuns.empty()) {
		receipt.mapSucceeded = receipt.writeSucceeded = receipt.flushSucceeded = true;
		return finish(CommitLightingUpload(*plan, receipt, mirror), "Unchanged canonical lighting region.");
	}
	void* mapped = nullptr;
	const VkResult mapResult = vmaMapMemory(allocator, buffer.allocation, &mapped);
	if (mapResult != VK_SUCCESS || !mapped) {
		if (mapResult == VK_SUCCESS) vmaUnmapMemory(allocator, buffer.allocation);
		(void)CommitLightingUpload(*plan, receipt, mirror);
		return finish(false, "Lighting map failed; full retry remains required.");
	}
	receipt.mapSucceeded = true;
	// Range vectors allocate before the first write: exceptions cannot leave a
	// partly overwritten allocation with a still-initialized successful mirror.
	try {
		evidence.logicalWriteRanges.reserve(plan->logicalRuns.size());
		evidence.physicalFlushRanges.reserve(plan->flushRanges.size());
		for (const auto& run : plan->logicalRuns) {
			const auto bytes = static_cast<std::uint64_t>(run.recordCount) * kRecordBytes;
			const auto destinationOffset = offset + static_cast<std::uint64_t>(run.firstRecord) * kRecordBytes;
			std::memcpy(static_cast<std::uint8_t*>(mapped) + destinationOffset,
				plan->desiredBlock.records.data() + run.firstRecord, static_cast<std::size_t>(bytes));
			evidence.logicalWriteRanges.push_back({destinationOffset, bytes});
			evidence.logicalWriteBytes += bytes; evidence.logicalWriteRecords += run.recordCount;
		}
		receipt.writeSucceeded = true; receipt.flushSucceeded = true;
		for (const auto& range : plan->flushRanges) {
			const VkResult result = vmaFlushAllocation(allocator, buffer.allocation, range.offsetBytes, range.sizeBytes);
			if (result != VK_SUCCESS) { receipt.flushSucceeded = false; break; }
			evidence.physicalFlushRanges.push_back({range.offsetBytes, range.sizeBytes});
			evidence.physicalFlushBytes += range.sizeBytes;
		}
	} catch (...) {
		receipt.writeSucceeded = false;
		vmaUnmapMemory(allocator, buffer.allocation);
		(void)CommitLightingUpload(*plan, receipt, mirror);
		throw;
	}
	vmaUnmapMemory(allocator, buffer.allocation);
	const bool committed = CommitLightingUpload(*plan, receipt, mirror);
	return finish(committed, committed ? "Lighting ranges written and successfully published." : "Lighting flush failed; full retry remains required.");
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
