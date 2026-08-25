// VulkanContextResources.cpp — Core Vulkan buffer, texture, mesh, and descriptor resources
//
// This module owns the shared resource primitives used by the specialized resource modules.
// All allocations remain synchronized through VulkanContext::Impl and its existing locks.

#include "VulkanResourceSource.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"

// VulkanContext::Impl::CreateDefaultTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateDefaultTexture() {
	// 1. Create Default White Texture (Index 0)
	{
		uint32_t whitePixel = 0xFFFFFFFF;
		VkDeviceSize imageSize = 4;
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Generic Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, &whitePixel, imageSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {1, 1, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultTexture.image, &defaultTexture.allocation, nullptr);
		vmaSetAllocationName(allocator, defaultTexture.allocation, "Default White Texture");

		TransitionImageLayout(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, defaultTexture.image, 1, 1);
		TransitionImageLayout(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		defaultTexture.view = CreateImageView(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(device, &samplerInfo, nullptr, &defaultTexture.sampler);

		uint32_t idx = AllocateTextureIndex(); // Should be 0
		UpdateGlobalTexture(idx, defaultTexture);
	}

	// 2. Create Default Normal Texture (Index 1)
	{
		uint32_t bluePixel = 0xFFFF7F7F; // (127, 127, 255, 255) -> Flat Normal
		VkDeviceSize imageSize = 4;
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Default Normal Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, &bluePixel, imageSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {1, 1, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultNormalTexture.image, &defaultNormalTexture.allocation, nullptr);

		TransitionImageLayout(defaultNormalTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, defaultNormalTexture.image, 1, 1);
		TransitionImageLayout(defaultNormalTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		defaultNormalTexture.view = CreateImageView(defaultNormalTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(device, &samplerInfo, nullptr, &defaultNormalTexture.sampler);

		uint32_t idx = AllocateTextureIndex(); // Should be 1
		UpdateGlobalTexture(idx, defaultNormalTexture);
	}

	// 3. Create Default Material Mask Texture (Index 2)
	{
		// Mask channels follow the fragment shader convention:
		// R = occlusion, G = roughness, B = metallic, A = unused. A missing
		// mask should describe a fully visible, rough, non-metal surface so
		// untextured primitives receive ordinary diffuse lighting.
		uint32_t maskPixel = 0xFF00FFFF; // (255, 255, 0, 255)
		VkDeviceSize imageSize = 4;
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Default Material Mask Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, &maskPixel, imageSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {1, 1, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultMaterialMaskTexture.image, &defaultMaterialMaskTexture.allocation, nullptr);
		vmaSetAllocationName(allocator, defaultMaterialMaskTexture.allocation, "Default Material Mask Texture");

		TransitionImageLayout(defaultMaterialMaskTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, defaultMaterialMaskTexture.image, 1, 1);
		TransitionImageLayout(defaultMaterialMaskTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		defaultMaterialMaskTexture.view = CreateImageView(defaultMaterialMaskTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(device, &samplerInfo, nullptr, &defaultMaterialMaskTexture.sampler);

		uint32_t idx = AllocateTextureIndex(); // Should be 2
		UpdateGlobalTexture(idx, defaultMaterialMaskTexture);
	}
}

// VulkanContext::Impl::CreateBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanBuffer VulkanContext::Impl::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, const char* name) {
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = memoryUsage;
	const bool hostVisible = memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY
		|| memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU
		|| memoryUsage == VMA_MEMORY_USAGE_GPU_TO_CPU;
	if(hostVisible) {
		// VMA keeps this allocation mapped until destruction and returns the stable
		// CPU address in allocationInfo. Device-local buffers retain the ordinary
		// unmapped path and cannot accidentally become host-accessible.
		allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
	}

	VulkanBuffer buffer{};
	buffer.size = size;
	VmaAllocationInfo allocationInfo{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer,
		&buffer.allocation, &allocationInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to create buffer!");
	}
	buffer.mappedData = hostVisible ? allocationInfo.pMappedData : nullptr;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name) {
		vmaSetAllocationName(allocator, buffer.allocation, name);
	}

	return buffer;
}

// VulkanContext::Impl::DestroyBufferImmediate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyBufferImmediate(VulkanBuffer& buffer)
{
	if (buffer.buffer != VK_NULL_HANDLE && buffer.allocation != VK_NULL_HANDLE)
	{
		vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
	}

	buffer = {};
}

// VulkanContext::Impl::RetireBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RetireBuffer(VulkanBuffer& buffer)
{
	if (buffer.buffer == VK_NULL_HANDLE || buffer.allocation == VK_NULL_HANDLE)
	{
		buffer = {};
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (retiredBuffersByFrame.size() < MAX_FRAMES_IN_FLIGHT)
	{
		retiredBuffersByFrame.resize(MAX_FRAMES_IN_FLIGHT);
	}

	// Shared buffers can still be referenced by other frames already queued on
	// the GPU. Defer destruction until this frame slot comes back around and
	// its fence has been waited, which guarantees the old submissions are done.
	retiredBuffersByFrame[currentFrame].push_back(buffer);
	buffer = {};
}


// VulkanContext::Impl::CopyBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	(void)EndSingleTimeCommands(commandBuffer, "buffer copy");
}

// VulkanContext::Impl::BeginSingleTimeCommands is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::BeginSingleTimeCommands() {
	std::lock_guard<std::mutex> lock(immediateCommandMutex);
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = immediateCommandPool != VK_NULL_HANDLE ? immediateCommandPool : commandPools[currentFrame];
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to allocate immediate command buffer");
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to begin immediate command buffer");
		vkFreeCommandBuffers(device, allocInfo.commandPool, 1, &commandBuffer);
		return VK_NULL_HANDLE;
	}
	RecordDiagnosticCheckpoint(commandBuffer, "immediate.begin");

	return commandBuffer;
}

// VulkanContext::Impl::EndSingleTimeCommands is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::EndSingleTimeCommands(VkCommandBuffer commandBuffer, std::string_view operation) {
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(immediateCommandMutex);
	RecordDiagnosticCheckpoint(commandBuffer, "immediate.end");
	const VkCommandPool owningPool = immediateCommandPool != VK_NULL_HANDLE ? immediateCommandPool : commandPools[currentFrame];
	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to end immediate command buffer");
		vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		return false;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence immediateFence = VK_NULL_HANDLE;
	if (vkCreateFence(device, &fenceInfo, nullptr, &immediateFence) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to create immediate command fence");
		vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		return false;
	}

	const VkResult submitResult = SubmitGraphicsQueue(1, &submitInfo, immediateFence);
	if (submitResult != VK_SUCCESS)
	{
		const std::string submitOperation = std::string(operation) + " submit";
		if (MarkDeviceLost(submitResult, submitOperation))
		{
			vkDestroyFence(device, immediateFence, nullptr);
			return false;
		}
		TERRAENGINE_LOG(LogLevel::Error, "failed to submit immediate command buffer: " << submitResult);
		// A failed submit can leave ownership ambiguous on validation/device-loss
		// paths. Do not free the command buffer unless the queue proves idle;
		// freeing a possibly-pending upload buffer creates the secondary
		// vkFreeCommandBuffers validation error seen after editor asset removal.
		const VkResult idleResult = WaitGraphicsQueueIdle();
		vkDestroyFence(device, immediateFence, nullptr);
		if (idleResult == VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		}
		if (MarkDeviceLost(idleResult, "immediate command failed-submit queue idle"))
		{
			return false;
		}
		return false;
	}

	const VkResult waitResult = vkWaitForFences(device, 1, &immediateFence, VK_TRUE, UINT64_MAX);
	if (waitResult != VK_SUCCESS)
	{
		// Preserve the caller's purpose in terminal diagnostics. A device loss is
		// often reported by this fence after an earlier queue command caused the
		// fault, so a generic "immediate command" label hides the useful boundary.
		const std::string waitOperation = std::string(operation) + " fence wait";
		if (MarkDeviceLost(waitResult, waitOperation))
		{
			vkDestroyFence(device, immediateFence, nullptr);
			return false;
		}
		TERRAENGINE_LOG(LogLevel::Error, "failed to wait for immediate command fence: " << waitResult);
		// The fence is a temporary ownership marker for this helper. Destroy it
		// even on a wait failure so shutdown does not leak child objects; command
		// buffer freeing still stays gated on the queue proving idle/completed.
		const VkResult idleResult = WaitGraphicsQueueIdle();
		vkDestroyFence(device, immediateFence, nullptr);
		if (idleResult == VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		}
		if (MarkDeviceLost(idleResult, "immediate command failed-wait queue idle"))
		{
			return false;
		}
		return false;
	}

	vkDestroyFence(device, immediateFence, nullptr);

	vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
	return true;
}

// VulkanContext::Impl::TransitionImageLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels, uint32_t arrayLayers) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return false;
	}

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;

	// Determine aspect mask based on format
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
		format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
		format == VK_FORMAT_D24_UNORM_S8_UINT) {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT) {
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	} else {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = mipLevels == 0u ? VK_REMAINING_MIP_LEVELS : mipLevels;
	barrier.subresourceRange.baseArrayLayer = 0;
	// Cubemap resources contain six array layers. Let callers transition the
	// complete resource so every framebuffer face starts from the declared layout.
	barrier.subresourceRange.layerCount = std::max(arrayLayers, 1u);

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	} else {
		throw std::invalid_argument("unsupported layout transition!");
	}

	vkCmdPipelineBarrier(
		commandBuffer,
		sourceStage, destinationStage,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier
	);

	const std::string transitionOperation = "image layout transition "
		+ std::to_string(static_cast<int>(oldLayout))
		+ "->"
		+ std::to_string(static_cast<int>(newLayout))
		+ " format="
		+ std::to_string(static_cast<int>(format))
		+ " layers="
		+ std::to_string(arrayLayers);
	return EndSingleTimeCommands(commandBuffer, transitionOperation);
}

// VulkanContext::Impl::CopyBufferToImage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return false;
	}

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {
		width,
		height,
		1
	};

	vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	return EndSingleTimeCommands(commandBuffer, "buffer-to-image copy");
}

// VulkanContext::Impl::CreateImageView is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkImageView VulkanContext::Impl::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels) {
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = std::max(1u, mipLevels);
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VkImageView imageView;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
		throw std::runtime_error("failed to create texture image view!");
	}

	return imageView;
}

// VulkanContext::Impl::StageVisibilityToVulkan is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkShaderStageFlags VulkanContext::Impl::StageVisibilityToVulkan(StageVisibilityMask visibility) {
	VkShaderStageFlags flags = 0;
	for (std::uint32_t bit = 0; bit < 32; ++bit) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((visibility & (1u << bit)) == 0) continue;
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (static_cast<ShaderStage>(bit)) {
			case ShaderStage::Vertex: flags |= VK_SHADER_STAGE_VERTEX_BIT; break;
			case ShaderStage::Fragment: flags |= VK_SHADER_STAGE_FRAGMENT_BIT; break;
			case ShaderStage::Geometry: flags |= VK_SHADER_STAGE_GEOMETRY_BIT; break;
			case ShaderStage::TessellationControl: flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
			case ShaderStage::TessellationEvaluation: flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break;
			case ShaderStage::Compute: flags |= VK_SHADER_STAGE_COMPUTE_BIT; break;
			case ShaderStage::Mesh: flags |= VK_SHADER_STAGE_MESH_BIT_EXT; break;
			case ShaderStage::Task: flags |= VK_SHADER_STAGE_TASK_BIT_EXT; break;
			default: break;
		}
	}
	return flags;
}

// VulkanContext::Impl::MapDescriptorType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkDescriptorType VulkanContext::Impl::MapDescriptorType(DescriptorType type) {
	switch (type) {
		case DescriptorType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case DescriptorType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		case DescriptorType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
		case DescriptorType::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		case DescriptorType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		case DescriptorType::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		case DescriptorType::InputAttachment: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		case DescriptorType::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		default: return VK_DESCRIPTOR_TYPE_SAMPLER;
	}
}

// VulkanContext::Impl::DestroyPipelines is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyPipelines() {
	// Mesh descriptor sets are allocated from per-pipeline set layouts, so they
	// must be freed before the corresponding layouts are destroyed.
	std::vector<VkDescriptorSet> meshDescriptorSets;
	for (auto& [meshId, mesh] : meshCache) {
		for (const auto& [layout, descriptorSet] : mesh.descriptorSetsByLayout) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (descriptorSet != VK_NULL_HANDLE) {
				meshDescriptorSets.push_back(descriptorSet);
			}
		}
		mesh.descriptorSetsByLayout.clear();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!meshDescriptorSets.empty() && device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(meshDescriptorSets.size()), meshDescriptorSets.data());
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& outerEntry : pipelineCache) {
		auto& map = outerEntry.second;
		for (auto& innerEntry : map) {
			VulkanPipeline& pipe = innerEntry.second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (device) {
				vkDestroyPipeline(device, pipe.pipeline, nullptr);
				vkDestroyPipelineLayout(device, pipe.layout, nullptr);
				// Start from 1 to skip the global descriptor set layout (index 0) which is shared and destroyed in Cleanup
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t i = 1; i < pipe.setLayouts.size(); ++i) {
					vkDestroyDescriptorSetLayout(device, pipe.setLayouts[i], nullptr);
				}
			}
		}
	}
	pipelineCache.clear();
}

// VulkanContext::Impl::DestroyWorkerPools is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyWorkerPools() {
	if (!device) return;
	for (size_t frameIndex = 0; frameIndex < workerCommandPools.size(); ++frameIndex) {
		auto& framePools = workerCommandPools[frameIndex];
		std::vector<std::vector<VkCommandBuffer>> emptyBuffers;
		auto& frameBuffers = (frameIndex < workerCommandBuffers.size()) ? workerCommandBuffers[frameIndex] : emptyBuffers;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t poolIndex = 0; poolIndex < framePools.size(); ++poolIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!framePools[poolIndex]) continue;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (poolIndex < frameBuffers.size() && !frameBuffers[poolIndex].empty()) {
				vkFreeCommandBuffers(device, framePools[poolIndex], static_cast<uint32_t>(frameBuffers[poolIndex].size()), frameBuffers[poolIndex].data());
				frameBuffers[poolIndex].clear();
			}
			vkDestroyCommandPool(device, framePools[poolIndex], nullptr);
		}
	}
	workerCommandPools.clear();
	workerCommandBuffers.clear();
	reusableSecondaryCommandSignatures.clear();
}

// VulkanContext::Impl::EnsureWorkerPools is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureWorkerPools(uint32_t frameIndex, size_t workerCount) {
	if (workerCommandPools.size() <= frameIndex) {
		workerCommandPools.resize(frameIndex + 1);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (workerCommandBuffers.size() <= frameIndex) {
		workerCommandBuffers.resize(frameIndex + 1);
	}

	auto& framePools = workerCommandPools[frameIndex];
	auto& frameBuffers = workerCommandBuffers[frameIndex];
	QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (framePools.size() < workerCount) {
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = indices.graphicsFamily.value();

		VkCommandPool pool = VK_NULL_HANDLE;
		vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
		framePools.push_back(pool);
		frameBuffers.emplace_back();
	}
}

// VulkanContext::Impl::AcquirePrimaryCommandBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::AcquirePrimaryCommandBuffer(uint32_t frameIndex) {
	if (frameIndex >= commandBuffers.size()) return VK_NULL_HANDLE;
	return commandBuffers[frameIndex];
}

// VulkanContext::Impl::AcquireWorkerCommandBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::AcquireWorkerCommandBuffer(uint32_t frameIndex, size_t workerIndex) {
	auto& framePools = workerCommandPools[frameIndex];
	auto& frameBuffers = workerCommandBuffers[frameIndex];

	// Ensure tracking structures are sized correctly
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (workerBufferUsage.size() <= frameIndex) workerBufferUsage.resize(frameIndex + 1);
	if (workerBufferUsage[frameIndex].size() <= workerIndex) workerBufferUsage[frameIndex].resize(workerIndex + 1, 0);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameBuffers.size() <= workerIndex) frameBuffers.resize(workerIndex + 1);

	size_t& usageIndex = workerBufferUsage[frameIndex][workerIndex];
	std::vector<VkCommandBuffer>& buffers = frameBuffers[workerIndex];

	// reuse existing buffer if available at this usage index
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (usageIndex < buffers.size()) {
		return buffers[usageIndex++];
	}

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = framePools[workerIndex];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer secondary = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkAllocateCommandBuffers(device, &allocInfo, &secondary) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	buffers.push_back(secondary);
	usageIndex++;
	return secondary;
}

// VulkanContext::Impl::MapTextureFormat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkFormat VulkanContext::Impl::MapTextureFormat(uint32_t vkFormat) {
	return static_cast<VkFormat>(vkFormat);
}

// VulkanContext::Impl::EnsureTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureTexture(TerraEngine::UUID textureId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (textureCache.contains(textureId)) {
		const VulkanImage& cachedTexture = textureCache[textureId];
		const TerraEngine::Resource::TextureMipResidency desiredMipResidency =
			ResourceManager::Instance().ResolveTextureMipResidencyForTexture(textureId, cachedTexture.sourceMipCount);
		if (desiredMipResidency.residentMipCount != cachedTexture.residentMipCount
			|| desiredMipResidency.sourceMipOffset != cachedTexture.residentMipOffset)
		{
			// Texture streaming policy and view distance can change while a
			// texture is cached. Recreate the image when the desired mip tail no
			// longer matches the cached VkImage dimensions, then fall through to
			// the normal upload path below.
			InvalidateTexture(textureId, false);
		}
		else
		{
		ResourceManager::Instance().MarkResourceUploadedToGpu(
			textureId,
			AssetType::Texture,
			static_cast<std::uint64_t>(currentFrame),
			static_cast<std::uint64_t>(cachedTexture.size),
			"vulkan texture cache hit",
			cachedTexture.residentMipCount,
			cachedTexture.sourceMipCount,
			cachedTexture.residentMipOffset);
		if (!textureIndexMap.contains(textureId)) {
			uint32_t index = AllocateTextureIndex();
			textureIndexMap[textureId] = index;
			UpdateGlobalTexture(index, cachedTexture);
		}
		return;
		}
	}

	Texture* cpuTexture = ResourceManager::Instance().GetTexture(textureId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!cpuTexture) return;

	// Ensure we have transcoded UASTC data
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cpuTexture->transcodedData.empty())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " has no transcoded payload data.");
		return;
	}

	const std::uint32_t sourceMipCount = std::max(1u, static_cast<std::uint32_t>(cpuTexture->descriptor.mipCount));
	const TerraEngine::Resource::TextureMipResidency mipResidency =
		ResourceManager::Instance().ResolveTextureMipResidencyForTexture(textureId, sourceMipCount);
	const std::uint32_t residentMipOffset = std::min(mipResidency.sourceMipOffset, sourceMipCount - 1u);
	const std::uint32_t residentMipCount = std::max(1u, std::min(mipResidency.residentMipCount, sourceMipCount - residentMipOffset));
	VkDeviceSize residentPayloadSize = 0u;
	for (std::uint32_t mipIndex = 0u; mipIndex < residentMipCount; ++mipIndex)
	{
		const std::uint32_t sourceMip = residentMipOffset + mipIndex;
		if (sourceMip >= cpuTexture->levelTable.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " has missing mip metadata.");
			return;
		}
		residentPayloadSize += static_cast<VkDeviceSize>(cpuTexture->levelTable[sourceMip].size);
	}
	if (residentPayloadSize == 0u)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " selected no resident mip payload.");
		return;
	}

	std::vector<std::uint8_t> residentUploadData(static_cast<std::size_t>(residentPayloadSize));
	VkDeviceSize uploadOffset = 0u;
	std::vector<VkBufferImageCopy> regions;
	regions.reserve(residentMipCount);
	for (std::uint32_t mipIndex = 0u; mipIndex < residentMipCount; ++mipIndex)
	{
		const std::uint32_t sourceMip = residentMipOffset + mipIndex;
		const auto& level = cpuTexture->levelTable[sourceMip];
		if (level.offset + level.size > cpuTexture->transcodedData.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " mip payload overruns transcoded data.");
			return;
		}
		std::memcpy(
			residentUploadData.data() + static_cast<std::size_t>(uploadOffset),
			cpuTexture->transcodedData.data() + static_cast<std::size_t>(level.offset),
			static_cast<std::size_t>(level.size));

		VkBufferImageCopy region{};
		region.bufferOffset = uploadOffset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipIndex;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.width) >> sourceMip);
		region.imageExtent.height = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.height) >> sourceMip);
		region.imageExtent.depth = 1;
		regions.push_back(region);
		uploadOffset += static_cast<VkDeviceSize>(level.size);
	}

	VulkanBuffer stagingBuffer = CreateBuffer(residentPayloadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Texture Mip Staging Buffer");
	WriteMappedBuffer(allocator, stagingBuffer, residentUploadData.data(), residentPayloadSize);

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.width) >> residentMipOffset);
	imageInfo.extent.height = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.height) >> residentMipOffset);
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = residentMipCount;
	imageInfo.arrayLayers = 1;
	imageInfo.format = MapTextureFormat(cpuTexture->descriptor.vkFormat);
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VulkanImage vImage;
	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &vImage.image, &vImage.allocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create image for texture");
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		return;
	}

	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		vmaDestroyImage(allocator, vImage.image, vImage.allocation);
		return;
	}
	VkImageMemoryBarrier uploadBarrier{};
	uploadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	uploadBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	uploadBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	uploadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadBarrier.image = vImage.image;
	uploadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	uploadBarrier.subresourceRange.baseMipLevel = 0u;
	uploadBarrier.subresourceRange.levelCount = residentMipCount;
	uploadBarrier.subresourceRange.baseArrayLayer = 0u;
	uploadBarrier.subresourceRange.layerCount = 1u;
	uploadBarrier.srcAccessMask = 0u;
	uploadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0u,
		0u, nullptr,
		0u, nullptr,
		1u, &uploadBarrier);

	vkCmdCopyBufferToImage(commandBuffer, stagingBuffer.buffer, vImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());

	VkImageMemoryBarrier shaderReadBarrier = uploadBarrier;
	shaderReadBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	shaderReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	shaderReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	shaderReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
		0u,
		0u, nullptr,
		0u, nullptr,
		1u, &shaderReadBarrier);

	if (!EndSingleTimeCommands(commandBuffer, "texture upload"))
	{
		// If the queue did not prove completion, leave the upload allocations
		// alone instead of destroying resources a possibly-pending command
		// buffer may still reference. The texture is not cached or published.
		return;
	}

	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	vImage.view = CreateImageView(vImage.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT, residentMipCount);
	vImage.size = residentPayloadSize;
	vImage.sourceMipCount = sourceMipCount;
	vImage.residentMipCount = residentMipCount;
	vImage.residentMipOffset = residentMipOffset;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vImage.view == VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, vImage.image, vImage.allocation);
		return;
	}

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = static_cast<float>(residentMipCount - 1u);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateSampler(device, &samplerInfo, nullptr, &vImage.sampler) != VK_SUCCESS) {
		vkDestroyImageView(device, vImage.view, nullptr);
		vmaDestroyImage(allocator, vImage.image, vImage.allocation);
		return;
	}

	textureCache[textureId] = vImage;
	ResourceManager::Instance().MarkResourceUploadedToGpu(
		textureId,
		AssetType::Texture,
		static_cast<std::uint64_t>(currentFrame),
		static_cast<std::uint64_t>(residentPayloadSize),
		mipResidency.partial ? "vulkan texture partial mip upload" : "vulkan texture upload",
		residentMipCount,
		sourceMipCount,
		residentMipOffset);

	// Assign bindless index
	uint32_t index = AllocateTextureIndex();
	textureIndexMap[textureId] = index;
	UpdateGlobalTexture(index, vImage);
}

// VulkanContext::Impl::EnsureMaterialDescriptorSets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<VkDescriptorSet> VulkanContext::Impl::EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	// No longer returns descriptor sets, but acts as an upload mechanism for material data.
	// The return value is kept for now to minimize API breakage, but it will be empty besides the global set.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (materialIndexMap.contains(materialId)) {
		return { globalDescriptorSets[currentFrame] };
	}

	Material* material = ResourceManager::Instance().GetMaterial(materialId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!material) return { globalDescriptorSets[currentFrame] };

	MaterialData data = CreateFallbackMaterialData();
	data.BaseColorFactor = glm::vec4(
		material->header.baseColorFactor[0],
		material->header.baseColorFactor[1],
		material->header.baseColorFactor[2],
		material->header.baseColorFactor[3]
	);
	data.EmissiveFactor = glm::vec4(
		material->header.emissiveFactor[0],
		material->header.emissiveFactor[1],
		material->header.emissiveFactor[2],
		1.0f
	);
	data.MetallicFactor = material->header.metallicFactor;
	data.RoughnessFactor = material->header.roughnessFactor;
	data.NormalScale = material->header.normalScale;
	data.OcclusionStrength = material->header.occlusionStrength;
	data.AlphaCutoff = material->header.alphaCutoff;
	data.AlphaMode = material->header.alphaMode;
	data.ShadingModel = material->header.shadingModel;
	data.Flags = material->header.flags;
	// Shadow behavior is packed beside the ordinary material parameters so
	// both the default shading pass and shadow-map pass can make the same
	// opacity decision without guessing from alpha mode alone.
	data.ShadowBehavior = static_cast<std::uint32_t>(material->shadowBehavior);
	data.UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); // Default: Scale 1, Offset 0

	// Initialize texture slots to renderer-owned fallbacks:
	// 0 = white base color, 1 = flat normal, 2 = non-metal rough material mask.
	data.BaseColorTextureIndex = 0u;
	data.NormalTextureIndex = 1u;
	data.MaskTextureIndex = 2u;
	data.OcclusionTextureIndex = 2u;
	data.EmissiveTextureIndex = 0u;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t i = 0; i < material->header.textureCount; ++i) {
		const auto& binding = material->textures[i];
		if (binding.textureUuid == TerraEngine::EMPTY_UUID) continue;

		EnsureTexture(binding.textureUuid);
		uint32_t gpuIndex = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (textureIndexMap.contains(binding.textureUuid)) {
			gpuIndex = textureIndexMap[binding.textureUuid];
		}

		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (binding.semantic) {
			case TextureSemantic::Base:
				data.BaseColorTextureIndex = gpuIndex;
				// Use Base's UV transform as the material-wide transform
				data.UVTransform = glm::vec4(binding.uvScale[0], binding.uvScale[1], binding.uvOffset[0], binding.uvOffset[1]);
				break;
			case TextureSemantic::Normal:            data.NormalTextureIndex = gpuIndex; break;
			case TextureSemantic::MetallicRoughness: data.MaskTextureIndex = gpuIndex; break;
			case TextureSemantic::Occlusion:         data.OcclusionTextureIndex = gpuIndex; break;
			case TextureSemantic::Emissive:          data.EmissiveTextureIndex = gpuIndex; break;
			default: break;
		}
	}

	if(const auto terrainBinding = TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().Resolve(materialId);
		terrainBinding && terrainBinding->IsComplete())
	{
		// Terrain control maps and layer tables are runtime-derived resources. They
		// stay outside TMAT v1 serialization while sharing the ordinary bindless heap
		// and PBR shader, so non-terrain materials retain zeroed extension fields.
		EnsureTexture(terrainBinding->weightTextureId);
		EnsureTexture(terrainBinding->layerIdTextureId);
		const auto weightIndex = textureIndexMap.find(terrainBinding->weightTextureId);
		const auto idIndex = textureIndexMap.find(terrainBinding->layerIdTextureId);
		if(weightIndex != textureIndexMap.end() && idIndex != textureIndexMap.end())
		{
			data.Flags |= TerrainLayerBlend;
			data.TerrainControl = glm::uvec4(
				weightIndex->second, idIndex->second, terrainBinding->layerCount, 0u);
			for(std::uint32_t layerIndex = 0u; layerIndex < terrainBinding->layerCount; ++layerIndex)
			{
				const auto& layer = terrainBinding->layers[layerIndex];
				std::uint32_t textureIndex = 0u;
				if(layer.baseColorTextureId != TerraEngine::EMPTY_UUID)
				{
					EnsureTexture(layer.baseColorTextureId);
					if(const auto found = textureIndexMap.find(layer.baseColorTextureId);
						found != textureIndexMap.end()) textureIndex = found->second;
				}
				data.TerrainLayerTextureIndices[layerIndex / 4u][layerIndex % 4u] = textureIndex;
				data.TerrainLayerStableIds[layerIndex / 4u][layerIndex % 4u] = layer.stableLayerId;
				data.TerrainLayerBaseColorFactors[layerIndex] = glm::vec4(
					layer.baseColorFactor[0], layer.baseColorFactor[1],
					layer.baseColorFactor[2], layer.baseColorFactor[3]);
				data.TerrainLayerUVTransforms[layerIndex] = glm::vec4(
					layer.uvTransform[0], layer.uvTransform[1],
					layer.uvTransform[2], layer.uvTransform[3]);
			}
		}
	}

	uint32_t materialIndex = AllocateMaterialIndex();
	materialIndexMap[materialId] = materialIndex;

	VkDeviceSize bufferSize = sizeof(MaterialData);
	VulkanBuffer stagingBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Material Data Staging Buffer");
	WriteMappedBuffer(allocator, stagingBuffer, &data, bufferSize);

	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer != VK_NULL_HANDLE)
	{
		VkBufferCopy copyRegion{};
		copyRegion.srcOffset = 0;
		copyRegion.dstOffset = materialIndex * sizeof(MaterialData);
		copyRegion.size = bufferSize;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBOs[i].buffer, 1, &copyRegion);
		}
		EndSingleTimeCommands(commandBuffer, "material data upload");
	}

	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	return { globalDescriptorSets[currentFrame] };
}

// VulkanContext::Impl::InvalidateMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateMaterial(TerraEngine::UUID materialId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	// Material edits can change alpha clipping or shadow behavior, so cached
	// shadow results are conservatively invalidated before the material is
	// rebound.
	shadowCacheEntries.clear();
	auto it = materialIndexMap.find(materialId);
	if (it != materialIndexMap.end()) {
		if (it->second != kFallbackMaterialIndex) {
			freeMaterialIndices.push_back(it->second);
		}
		materialIndexMap.erase(it);
	}
}

// VulkanContext::Impl::InvalidateMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateMesh(TerraEngine::UUID meshId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Mesh edits change caster geometry. Clear every cache entry rather than
	// trying to prove which light was affected.
	shadowCacheEntries.clear();
	auto it = meshCache.find(meshId);
	if (it != meshCache.end()) {
		// Pending command buffers can still reference the mesh-local descriptor
		// sets and bindless buffer slot while a project reload is invalidating
		// resources. Wait once before mutating or freeing any descriptor-backed
		// state owned by this mesh.
		WaitForDescriptorMutationIdle();
		if (it->second.bindlessIndex != INVALID_BINDLESS_BUFFER_INDEX) {
			// The descriptor still contains the old buffer handle until the slot
			// is reused, but no live draw should index this mesh after the cache
			// entry is erased. Writing VK_NULL_HANDLE would require the Vulkan
			// nullDescriptor feature, so recycle only the CPU-side slot owner.
			freeBufferIndices.push_back(it->second.bindlessIndex);
			it->second.bindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!it->second.descriptorSetsByLayout.empty() && descriptorPool != VK_NULL_HANDLE) {
			std::vector<VkDescriptorSet> descriptorSets;
			descriptorSets.reserve(it->second.descriptorSetsByLayout.size());
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& [layout, set] : it->second.descriptorSetsByLayout) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (set != VK_NULL_HANDLE) {
					descriptorSets.push_back(set);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!descriptorSets.empty()) {
				vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
			}
			it->second.descriptorSetsByLayout.clear();
		}
		vmaDestroyBuffer(allocator, it->second.meshShaderVertexBuffer.buffer, it->second.meshShaderVertexBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletVertexRemapBuffer.buffer, it->second.meshletVertexRemapBuffer.allocation);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (it->second.meshSkinningBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->second.meshSkinningBuffer.buffer, it->second.meshSkinningBuffer.allocation);
		}
		if (it->second.meshMorphDeltaBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->second.meshMorphDeltaBuffer.buffer, it->second.meshMorphDeltaBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, it->second.meshDecodeInfoBuffer.buffer, it->second.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.indexBuffer.buffer, it->second.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletBuffer.buffer, it->second.meshletBuffer.allocation);
		if (it->second.meshletSectionBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->second.meshletSectionBuffer.buffer, it->second.meshletSectionBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, it->second.meshletBoundsBuffer.buffer, it->second.meshletBoundsBuffer.allocation);
		meshCache.erase(it);
	}
}

// VulkanContext::Impl::InvalidateTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateTexture(TerraEngine::UUID textureId, bool reloadAfterInvalidate) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Texture edits may affect alpha-tested caster silhouettes, so shadow cache
	// reuse falls back to refresh after the texture changes.
	shadowCacheEntries.clear();
	bool textureWasKnown = false;
	auto it = textureCache.find(textureId);
	if (it != textureCache.end()) {
		// Don't invalidate default texture
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (it->second.image == defaultTexture.image) return;

		// Texture reloads update bindless descriptors and destroy old image
		// views. Wait for submitted command buffers before those descriptors can
		// point at replacement resources.
		WaitForDescriptorMutationIdle();
		vkDestroySampler(device, it->second.sampler, nullptr);
		vkDestroyImageView(device, it->second.view, nullptr);
		vmaDestroyImage(allocator, it->second.image, it->second.allocation);
		textureCache.erase(it);
		textureWasKnown = true;
	}

	if (auto indexIt = textureIndexMap.find(textureId); indexIt != textureIndexMap.end())
	{
		const uint32_t releasedIndex = indexIt->second;
		UpdateGlobalTexture(releasedIndex, defaultTexture);
		textureIndexMap.erase(indexIt);
		if (releasedIndex > 2u)
		{
			freeTextureIndices.push_back(releasedIndex);
		}
		textureWasKnown = true;
	}

	if (!reloadAfterInvalidate && textureWasKnown && !materialIndexMap.empty())
	{
		// Materials store bindless texture indices in the material SSBO. When
		// residency evicts a texture, any cached material that referenced that
		// index must be rebuilt the next time it is drawn so EnsureTexture can
		// reload visible texture dependencies.
		for (const auto& [materialId, materialIndex] : materialIndexMap)
		{
			(void)materialId;
			if (materialIndex != kFallbackMaterialIndex)
			{
				freeMaterialIndices.push_back(materialIndex);
			}
		}
		materialIndexMap.clear();
	}

	if (reloadAfterInvalidate)
	{
		// Reload paths wait for CPU transcode first, so they can re-upload here.
		// Residency retirement passes false so an evicted texture stays unloaded
		// until visibility streaming demands it again.
		EnsureTexture(textureId);
	}
}

// VulkanContext::Impl::InvalidateResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateResource(TerraEngine::UUID id, bool reloadTexturesAfterInvalidate) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	InvalidateMesh(id);
	InvalidateTexture(id, reloadTexturesAfterInvalidate);
	InvalidateMaterial(id);
}

// VulkanContext::Impl::EnsureMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureMesh(TerraEngine::UUID meshId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	auto existingMeshIt = meshCache.find(meshId);
	if (existingMeshIt != meshCache.end())
	{
		const VulkanMesh& mesh = existingMeshIt->second;
		const VkDeviceSize totalGpuBytes = TotalMeshGpuBytes(mesh);
		const Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
		std::string uploadDetail = "vulkan mesh cache hit";
		if (cpuMesh != nullptr && cpuMesh->hasVirtualGeometry && cpuMesh->virtualGeometryPageStreamingReady)
		{
			uploadDetail = cpuMesh->virtualGeometryOwnerFallbackReason.empty()
				? "vulkan mesh cache hit with page-local virtual geometry owner placeholders"
				: "vulkan mesh cache hit with owning-mesh virtual geometry fallback: " + cpuMesh->virtualGeometryOwnerFallbackReason;
		}
		ResourceManager::Instance().MarkResourceUploadedToGpu(
			meshId,
			AssetType::Mesh,
			static_cast<std::uint64_t>(currentFrame),
			static_cast<std::uint64_t>(totalGpuBytes),
			uploadDetail);
		return;
	}

	Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!cpuMesh) return;

	const VkDeviceSize packedVertexSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexData.size());
	const VkDeviceSize vertexRemapSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexRemapData.size());
	const VkDeviceSize skinningDataSize = static_cast<VkDeviceSize>(cpuMesh->meshletSkinningData.size());
	const VkDeviceSize primitiveIndexSize = static_cast<VkDeviceSize>(cpuMesh->meshletPrimitiveIndexData.size());
	// ByteAddressBuffer.Load reads one complete 32-bit word even when the
	// logical payload stores 16-bit remaps or 8-bit primitive indices. Pad the
	// Vulkan allocation and descriptor range to that access granularity while
	// retaining the original logical counts in MeshDecodeInfo.
	const auto AlignByteAddressRange = [](VkDeviceSize byteCount)
	{
		return (byteCount + 3u) & ~VkDeviceSize{3u};
	};
	const VkDeviceSize vertexRemapUploadSize =
		AlignByteAddressRange(vertexRemapSize);
	const VkDeviceSize primitiveIndexUploadSize =
		AlignByteAddressRange(primitiveIndexSize);
	const VkDeviceSize meshletTableSize = static_cast<VkDeviceSize>(cpuMesh->meshletRecords.size() * sizeof(MeshletRecord));
	const VkDeviceSize meshletSectionSize = static_cast<VkDeviceSize>(cpuMesh->meshletSections.size() * sizeof(MeshletSection));
	const VkDeviceSize meshletBoundsSize = static_cast<VkDeviceSize>(cpuMesh->meshletBounds.size() * sizeof(MeshletBounds));
	const bool hasBoundsPayload = meshletBoundsSize > 0;
	const VkDeviceSize boundsUploadSize = hasBoundsPayload ? meshletBoundsSize : static_cast<VkDeviceSize>(sizeof(MeshletBounds));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (packedVertexSize == 0
		|| vertexRemapSize == 0
		|| primitiveIndexSize == 0
		|| meshletTableSize == 0
		|| meshletSectionSize == 0
		|| cpuMesh->runtimeLayout.meshletCount == 0)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload is missing packed vertex/remap/primitive/table data for UUID " << ToHexString(meshId));
		return;
	}

	const uint32_t packedStride = cpuMesh->runtimeLayout.layout.strideBytes;
	const uint32_t meshletVertexCount = cpuMesh->runtimeLayout.meshletVertexCount;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (packedStride != kMeshletPackedVertexStrideBytes || meshletVertexCount == 0) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload has invalid packed vertex stride/count for UUID " << ToHexString(meshId));
		return;
	}

	const VkDeviceSize expectedVertexBytes = static_cast<VkDeviceSize>(packedStride) * static_cast<VkDeviceSize>(meshletVertexCount);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (expectedVertexBytes != packedVertexSize) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload packed vertex range is truncated for UUID " << ToHexString(meshId));
		return;
	}

	// struct MeshDecodeInfo groups related state and operations so callers have one documented contract.
	struct MeshDecodeInfo
	{
		float positionOffset[4];
		float positionScale[4];
		float uvOffsetScale[4];
		std::uint32_t counts[4];
	};
	static_assert(sizeof(MeshDecodeInfo) == 64, "Mesh decode info buffer must keep a stable 64-byte layout.");

	MeshDecodeInfo decodeInfo{};
	decodeInfo.positionOffset[0] = cpuMesh->runtimeLayout.quantization.positionOffset[0];
	decodeInfo.positionOffset[1] = cpuMesh->runtimeLayout.quantization.positionOffset[1];
	decodeInfo.positionOffset[2] = cpuMesh->runtimeLayout.quantization.positionOffset[2];
	decodeInfo.positionScale[0] = cpuMesh->runtimeLayout.quantization.positionScale[0];
	decodeInfo.positionScale[1] = cpuMesh->runtimeLayout.quantization.positionScale[1];
	decodeInfo.positionScale[2] = cpuMesh->runtimeLayout.quantization.positionScale[2];
	decodeInfo.uvOffsetScale[0] = cpuMesh->runtimeLayout.quantization.uvOffset[0];
	decodeInfo.uvOffsetScale[1] = cpuMesh->runtimeLayout.quantization.uvOffset[1];
	decodeInfo.uvOffsetScale[2] = cpuMesh->runtimeLayout.quantization.uvScale[0];
	decodeInfo.uvOffsetScale[3] = cpuMesh->runtimeLayout.quantization.uvScale[1];
	decodeInfo.counts[0] = cpuMesh->runtimeLayout.meshletVertexCount;
	decodeInfo.counts[1] = cpuMesh->runtimeLayout.meshletVertexRemapCount;
	decodeInfo.counts[2] = cpuMesh->runtimeLayout.meshletPrimitiveIndexCount;
	// counts.w: bits [0:30] = meshletCount, bit 31 = remapEncoding (0=u32, 1=u16)
	decodeInfo.counts[3] = (cpuMesh->runtimeLayout.meshletCount & 0x7FFFFFFFu) | (cpuMesh->runtimeLayout.meshletVertexRemapEncoding << 31u);

	std::vector<TemfDeformablePageMorphVertex> morphDeltaRecords;
	const bool hasValidatedMorphDeltas = BuildMeshMorphDeltaRecords(*cpuMesh, morphDeltaRecords);
	const VkDeviceSize morphDeltaSize = static_cast<VkDeviceSize>(
		morphDeltaRecords.size() * sizeof(TemfDeformablePageMorphVertex));
	if(morphDeltaRecords.empty() || morphDeltaSize == 0U) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh morph delta preparation produced no safe fallback record for UUID " << ToHexString(meshId));
		return;
	}
	if(!hasValidatedMorphDeltas) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh morph runtime data was inconsistent; uploading a zero morph delta fallback for UUID " << ToHexString(meshId));
	}

	VulkanMesh vMesh{};
	vMesh.indexCount = cpuMesh->runtimeLayout.meshletPrimitiveIndexCount;
	vMesh.indexType = VK_INDEX_TYPE_UINT16;
	vMesh.meshletCount = cpuMesh->runtimeLayout.meshletCount;
	auto createPlaceholderStorageBuffer = [&](VkDeviceSize byteSize, const char* name) -> VulkanBuffer {
		// Reflected storage-buffer descriptors may be statically used even when
		// this mesh has no corresponding payload. Give those bindings a small,
		// zero-filled owner instead of leaving an invalid descriptor unwritten.
		const VkDeviceSize safeSize = std::max<VkDeviceSize>(byteSize, 4u);
		VulkanBuffer buffer = CreateBuffer(safeSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, name);
		std::vector<std::uint8_t> zeros(static_cast<std::size_t>(safeSize), 0u);
		WriteMappedBuffer(allocator, buffer, zeros.data(), safeSize);
		return buffer;
	};

	const bool usePageLocalVirtualGeometry =
		cpuMesh->hasVirtualGeometry
		&& cpuMesh->virtualGeometryPageStreamingReady
		&& cpuMesh->virtualGeometryOwnerFallbackReason.empty()
		// Deformable draws currently consume owner skinning and morph streams
		// through the graphics bridge. Retain those full streams until the
		// page-local path can provide equivalent deformable payloads.
		&& !cpuMesh->hasDeformablePayload;
	if (usePageLocalVirtualGeometry)
	{
		// Page-streamed virtual geometry uses the per-frame page-local payload
		// buffer for real vertices, remaps, primitive indices, records, bounds,
		// and decode constants. These tiny owner buffers keep the existing mesh
		// descriptor set valid for legacy shader layouts without uploading the
		// full high-detail owning mesh as a hidden residency cost.
		vMesh.meshShaderVertexBuffer = createPlaceholderStorageBuffer(kMeshletPackedVertexStrideBytes, "Virtual Geometry Owner Placeholder Vertex Buffer");
		vMesh.meshletVertexRemapBuffer = createPlaceholderStorageBuffer(sizeof(std::uint32_t), "Virtual Geometry Owner Placeholder Remap Buffer");
		vMesh.meshSkinningBuffer = createPlaceholderStorageBuffer(kTemfSkinningStrideBytes, "Virtual Geometry Owner Placeholder Skinning Buffer");
		vMesh.meshDecodeInfoBuffer = createPlaceholderStorageBuffer(sizeof(MeshDecodeInfo), "Virtual Geometry Owner Placeholder Decode Buffer");
		vMesh.indexBuffer = createPlaceholderStorageBuffer(sizeof(std::uint32_t), "Virtual Geometry Owner Placeholder Primitive Buffer");
		vMesh.meshletBuffer = createPlaceholderStorageBuffer(sizeof(MeshletRecord), "Virtual Geometry Owner Placeholder Meshlet Buffer");
		vMesh.meshletBoundsBuffer = createPlaceholderStorageBuffer(sizeof(MeshletBounds), "Virtual Geometry Owner Placeholder Bounds Buffer");
		vMesh.meshMorphDeltaBuffer = createPlaceholderStorageBuffer(sizeof(TemfDeformablePageMorphVertex), "Virtual Geometry Owner Placeholder Morph Delta Buffer");
		vMesh.hasMeshletBounds = true;
	}
	else
	{
		VulkanBuffer stagingPackedVertex = CreateBuffer(packedVertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingPackedVertex, cpuMesh->meshletVertexData.data(), packedVertexSize);

		VulkanBuffer stagingVertexRemap = CreateBuffer(vertexRemapUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		// Zero-copy: upload raw on-disk bytes (u16 or u32) without CPU expansion.
		ClearMappedBuffer(allocator, stagingVertexRemap);
		WriteMappedBuffer(allocator, stagingVertexRemap, cpuMesh->meshletVertexRemapData.data(), vertexRemapSize);

		VulkanBuffer stagingSkinning = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0 };
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skinningDataSize > 0)
		{
			stagingSkinning = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
			WriteMappedBuffer(allocator, stagingSkinning, cpuMesh->meshletSkinningData.data(), skinningDataSize);
		}

		VulkanBuffer stagingMorphDeltas = CreateBuffer(morphDeltaSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingMorphDeltas, morphDeltaRecords.data(), morphDeltaSize);

		VulkanBuffer stagingIndex = CreateBuffer(primitiveIndexUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		ClearMappedBuffer(allocator, stagingIndex);
		WriteMappedBuffer(allocator, stagingIndex, cpuMesh->meshletPrimitiveIndexData.data(), primitiveIndexSize);

		VulkanBuffer stagingMeshletTable = CreateBuffer(meshletTableSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingMeshletTable, cpuMesh->meshletRecords.data(), meshletTableSize);

		VulkanBuffer stagingMeshletSection = CreateBuffer(meshletSectionSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		// Virtual geometry stores one meshlet section range per emitted LOD in the MVP.
		// Upload it with the other resident streams now; later stories bind it when
		// shaders start selecting LOD ranges directly.
		WriteMappedBuffer(allocator, stagingMeshletSection, cpuMesh->meshletSections.data(), meshletSectionSize);

		VulkanBuffer stagingMeshletBounds = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasBoundsPayload)
		{
			WriteMappedBuffer(allocator, stagingMeshletBounds, cpuMesh->meshletBounds.data(), meshletBoundsSize);
		}
		else
		{
			MeshletBounds fallbackBounds{};
			fallbackBounds.coneCutoff = 2.0f;
			WriteMappedBuffer(allocator, stagingMeshletBounds, &fallbackBounds, sizeof(fallbackBounds));
		}

		VulkanBuffer stagingDecodeInfo = CreateBuffer(sizeof(MeshDecodeInfo), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingDecodeInfo, &decodeInfo, sizeof(MeshDecodeInfo));

		// No CPU repack path: mesh shader reads the packed payload directly from storage buffers.
		vMesh.meshShaderVertexBuffer = CreateBuffer(packedVertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletVertexRemapBuffer = CreateBuffer(vertexRemapUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		if (skinningDataSize > 0)
		{
			vMesh.meshSkinningBuffer = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		}
		else
		{
			// Default mesh shaders statically read binding 6. A zero joint and
			// zero weight record makes that binding valid while preserving the
			// shader's identity-deformation behavior for non-skinned meshes.
			vMesh.meshSkinningBuffer = createPlaceholderStorageBuffer(kTemfSkinningStrideBytes, "Static Mesh Skinning Fallback Buffer");
		}
		vMesh.meshMorphDeltaBuffer = CreateBuffer(morphDeltaSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshDecodeInfoBuffer = CreateBuffer(sizeof(MeshDecodeInfo), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.indexBuffer = CreateBuffer(primitiveIndexUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletBuffer = CreateBuffer(meshletTableSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletSectionBuffer = CreateBuffer(meshletSectionSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletBoundsBuffer = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.hasMeshletBounds = hasBoundsPayload;

		CopyBuffer(stagingPackedVertex.buffer, vMesh.meshShaderVertexBuffer.buffer, packedVertexSize);
		CopyBuffer(stagingVertexRemap.buffer, vMesh.meshletVertexRemapBuffer.buffer, vertexRemapUploadSize);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skinningDataSize > 0)
		{
			CopyBuffer(stagingSkinning.buffer, vMesh.meshSkinningBuffer.buffer, skinningDataSize);
		}
		CopyBuffer(stagingMorphDeltas.buffer, vMesh.meshMorphDeltaBuffer.buffer, morphDeltaSize);
		CopyBuffer(stagingDecodeInfo.buffer, vMesh.meshDecodeInfoBuffer.buffer, sizeof(MeshDecodeInfo));
		CopyBuffer(stagingIndex.buffer, vMesh.indexBuffer.buffer, primitiveIndexUploadSize);
		CopyBuffer(stagingMeshletTable.buffer, vMesh.meshletBuffer.buffer, meshletTableSize);
		CopyBuffer(stagingMeshletSection.buffer, vMesh.meshletSectionBuffer.buffer, meshletSectionSize);
		CopyBuffer(stagingMeshletBounds.buffer, vMesh.meshletBoundsBuffer.buffer, boundsUploadSize);

		vmaDestroyBuffer(allocator, stagingPackedVertex.buffer, stagingPackedVertex.allocation);
		vmaDestroyBuffer(allocator, stagingVertexRemap.buffer, stagingVertexRemap.allocation);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skinningDataSize > 0)
		{
			vmaDestroyBuffer(allocator, stagingSkinning.buffer, stagingSkinning.allocation);
		}
		vmaDestroyBuffer(allocator, stagingMorphDeltas.buffer, stagingMorphDeltas.allocation);
		vmaDestroyBuffer(allocator, stagingDecodeInfo.buffer, stagingDecodeInfo.allocation);
		vmaDestroyBuffer(allocator, stagingIndex.buffer, stagingIndex.allocation);
		vmaDestroyBuffer(allocator, stagingMeshletTable.buffer, stagingMeshletTable.allocation);
		vmaDestroyBuffer(allocator, stagingMeshletSection.buffer, stagingMeshletSection.allocation);
		vmaDestroyBuffer(allocator, stagingMeshletBounds.buffer, stagingMeshletBounds.allocation);
	}

	vMesh.bindlessIndex = AllocateBufferIndex();
	UpdateGlobalBuffer(vMesh.bindlessIndex, vMesh.meshDecodeInfoBuffer.buffer, 0, vMesh.meshDecodeInfoBuffer.size);

	meshCache[meshId] = vMesh;
	const VkDeviceSize totalGpuBytes = TotalMeshGpuBytes(meshCache[meshId]);
	const bool hasVirtualGeometryPageMetadata = cpuMesh->hasVirtualGeometry && !cpuMesh->virtualGeometryPages.empty();
	std::string uploadDetail = "vulkan mesh buffer upload";
	if (usePageLocalVirtualGeometry)
	{
		uploadDetail = "vulkan page-local virtual geometry placeholder owner descriptors; owner mesh buffers are skipped for page-local virtual geometry";
	}
	else if (cpuMesh->hasDeformablePayload
		&& cpuMesh->virtualGeometryPageStreamingReady
		&& cpuMesh->virtualGeometryOwnerFallbackReason.empty())
	{
		uploadDetail = "vulkan deformable virtual geometry owner upload retained for graphics deformation";
	}
	else if (hasVirtualGeometryPageMetadata)
	{
		// The current shader path still binds owner mesh buffers for decoding.
		// Label that allocation as an owning-mesh fallback so exact residency
		// tools do not mistake it for a page-local physical payload.
		uploadDetail = cpuMesh->virtualGeometryPageStreamingReady
			? "vulkan owning-mesh virtual geometry fallback upload: " + cpuMesh->virtualGeometryOwnerFallbackReason
			: "vulkan mesh buffer upload with virtual geometry metadata that is not page-streaming ready";
	}
	ResourceManager::Instance().MarkResourceUploadedToGpu(
		meshId,
		AssetType::Mesh,
		static_cast<std::uint64_t>(currentFrame),
		static_cast<std::uint64_t>(totalGpuBytes),
		uploadDetail);
}

// VulkanContext::Impl::EnsureMeshDescriptorSet is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkDescriptorSet VulkanContext::Impl::EnsureMeshDescriptorSet(TerraEngine::UUID meshId, const VulkanPipeline& pipeline) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	auto meshIt = meshCache.find(meshId);
	if (meshIt == meshCache.end()) {
		return VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pipeline.setLayouts.size() <= 1 || pipeline.setLayouts[1] == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader pipeline is missing descriptor set layout 1 for mesh payload buffers.");
		return VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pipeline.bindingsPerSet.size() <= 1) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader pipeline is missing reflected binding metadata for descriptor set 1.");
		return VK_NULL_HANDLE;
	}

	const auto& meshBindings = pipeline.bindingsPerSet[1];
	auto HasStorageBinding = [&](uint32_t bindingIndex) -> bool {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DescriptorBinding& binding : meshBindings) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (binding.binding == bindingIndex) {
				return binding.type == static_cast<uint32_t>(DescriptorType::StorageBuffer);
			}
		}

		return false;
	};

	// Validate reflected layout before descriptor writes to avoid VK validation
	// crashes when stale shader artifacts expose incomplete bindings.
	if (!HasStorageBinding(0)
		|| !HasStorageBinding(1)
		|| !HasStorageBinding(2)
		|| !HasStorageBinding(3)
		|| !HasStorageBinding(4)) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader descriptor set 1 must expose storage-buffer bindings 0 through 4.");
		return VK_NULL_HANDLE;
	}

	VulkanMesh& mesh = meshIt->second;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mesh.meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
		|| mesh.indexBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshletBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload buffers are not initialized for mesh shader descriptor binding.");
		return VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (HasStorageBinding(5) && mesh.meshletBoundsBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires bounds binding 5 but meshlet bounds payload is unavailable.");
		return VK_NULL_HANDLE;
	}
	if (HasStorageBinding(6) && mesh.meshSkinningBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires skinning binding 6 but neither real data nor the zero fallback buffer is available.");
		return VK_NULL_HANDLE;
	}
	if (HasStorageBinding(7) && mesh.meshMorphDeltaBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires morph binding 7 but the safe morph delta buffer is unavailable.");
		return VK_NULL_HANDLE;
	}

	const VkDescriptorSetLayout layout = pipeline.setLayouts[1];
	auto cachedSetIt = mesh.descriptorSetsByLayout.find(layout);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cachedSetIt != mesh.descriptorSetsByLayout.end() && cachedSetIt->second != VK_NULL_HANDLE) {
		return cachedSetIt->second;
	}

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate mesh descriptor set for mesh shader pipeline.");
		return VK_NULL_HANDLE;
	}

	VkDescriptorBufferInfo vertexInfo{};
	vertexInfo.buffer = mesh.meshShaderVertexBuffer.buffer;
	vertexInfo.offset = 0;
	vertexInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo primitiveIndexInfo{};
	primitiveIndexInfo.buffer = mesh.indexBuffer.buffer;
	primitiveIndexInfo.offset = 0;
	primitiveIndexInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo meshletTableInfo{};
	meshletTableInfo.buffer = mesh.meshletBuffer.buffer;
	meshletTableInfo.offset = 0;
	meshletTableInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo vertexRemapInfo{};
	vertexRemapInfo.buffer = mesh.meshletVertexRemapBuffer.buffer;
	vertexRemapInfo.offset = 0;
	vertexRemapInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo decodeInfo{};
	decodeInfo.buffer = mesh.meshDecodeInfoBuffer.buffer;
	decodeInfo.offset = 0;
	decodeInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo boundsInfo{};
	boundsInfo.buffer = mesh.meshletBoundsBuffer.buffer;
	boundsInfo.offset = 0;
	boundsInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo skinningInfo{};
	skinningInfo.buffer = mesh.meshSkinningBuffer.buffer;
	skinningInfo.offset = 0;
	skinningInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo morphDeltaInfo{};
	morphDeltaInfo.buffer = mesh.meshMorphDeltaBuffer.buffer;
	morphDeltaInfo.offset = 0;
	morphDeltaInfo.range = VK_WHOLE_SIZE;

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(8);

	VkWriteDescriptorSet w{};
	w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w.dstSet = descriptorSet;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

	w.dstBinding = 0; w.pBufferInfo = &vertexInfo; writes.push_back(w);
	w.dstBinding = 1; w.pBufferInfo = &primitiveIndexInfo; writes.push_back(w);
	w.dstBinding = 2; w.pBufferInfo = &meshletTableInfo; writes.push_back(w);
	w.dstBinding = 3; w.pBufferInfo = &vertexRemapInfo; writes.push_back(w);
	w.dstBinding = 4; w.pBufferInfo = &decodeInfo; writes.push_back(w);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (HasStorageBinding(5))
	{
		w.dstBinding = 5; w.pBufferInfo = &boundsInfo; writes.push_back(w);
	}

	// Binding 6 is always written when declared. Static meshes own an explicit
	// zero skinning record so the statically used shader resource stays valid.
	if (HasStorageBinding(6))
	{
		w.dstBinding = 6; w.pBufferInfo = &skinningInfo; writes.push_back(w);
	}

	// Binding 7 is always written when declared. Static and legacy meshes own a
	// one-record zero fallback, so default mesh shaders never receive an invalid
	// descriptor while their morph weight resolves to zero.
	if (HasStorageBinding(7))
	{
		w.dstBinding = 7; w.pBufferInfo = &morphDeltaInfo; writes.push_back(w);
	}

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	mesh.descriptorSetsByLayout[layout] = descriptorSet;
	return descriptorSet;
}


// VulkanContext::Impl::AllocateTextureIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::AllocateTextureIndex() {
	if (!freeTextureIndices.empty()) {
		uint32_t index = freeTextureIndices.back();
		freeTextureIndices.pop_back();
		return index;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextTextureIndex >= MAX_BINDLESS_TEXTURES) {
		TERRAENGINE_LOG(LogLevel::Error, "Max bindless textures exceeded!");
		return 0; // Return default texture
	}
	return nextTextureIndex++;
}

// VulkanContext::Impl::AllocateMaterialIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::AllocateMaterialIndex() {
	while (!freeMaterialIndices.empty()) {
		uint32_t index = freeMaterialIndices.back();
		freeMaterialIndices.pop_back();
		if (index != kFallbackMaterialIndex) {
			return index;
		}
	}
	if (nextMaterialIndex <= kFallbackMaterialIndex) {
		nextMaterialIndex = kFirstRuntimeMaterialIndex;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextMaterialIndex >= MAX_BINDLESS_MATERIALS) {
		TERRAENGINE_LOG(LogLevel::Error, "Max bindless materials exceeded!");
		return 0;
	}
	return nextMaterialIndex++;
}

// VulkanContext::Impl::UpdateGlobalTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalTexture(uint32_t index, const VulkanImage& image) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	WaitForDescriptorMutationIdle();

	VkDescriptorImageInfo imageInfo{};
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
}

// VulkanContext::Impl::AllocateBufferIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::AllocateBufferIndex() {
	if (!freeBufferIndices.empty()) {
		uint32_t index = freeBufferIndices.back();
		freeBufferIndices.pop_back();
		return index;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextBufferIndex >= MAX_BINDLESS_BUFFERS) {
		throw std::runtime_error("Exceeded MAX_BINDLESS_BUFFERS!");
	}
	return nextBufferIndex++;
}

// VulkanContext::Impl::UpdateGlobalBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalBuffer(uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (index >= MAX_BINDLESS_BUFFERS
		|| buffer == VK_NULL_HANDLE
		|| range == 0u)
	{
		return;
	}

	WaitForDescriptorMutationIdle();

	VkDescriptorBufferInfo bufferInfo{};
	bufferInfo.buffer = buffer;
	bufferInfo.offset = offset;
	bufferInfo.range = range;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkWriteDescriptorSet descriptorWrite{};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = globalDescriptorSets[i];
		descriptorWrite.dstBinding = 9;
		descriptorWrite.dstArrayElement = index;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
	}
}

void VulkanContext::Impl::UpdateGlobalBufferForFrame(
	uint32_t frameIndex,
	uint32_t index,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkDeviceSize range,
	bool frameSlotKnownIdle)
{
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (frameIndex >= globalDescriptorSets.size()
		|| index >= MAX_BINDLESS_BUFFERS
		|| globalDescriptorSets[frameIndex] == VK_NULL_HANDLE
		|| buffer == VK_NULL_HANDLE
		|| range == 0u)
	{
		return;
	}

	if (frameIndex == currentFrame && !frameSlotKnownIdle)
	{
		// Updating a bindless buffer slot mutates the shared global descriptor
		// set. Reuse the central descriptor-idle path so already-submitted editor
		// view command buffers are waited on instead of accidentally submitted a
		// second time.
		WaitForDescriptorMutationIdle();
	}

	VkDescriptorBufferInfo bufferInfo{};
	bufferInfo.buffer = buffer;
	bufferInfo.offset = offset;
	bufferInfo.range = range;

	VkWriteDescriptorSet descriptorWrite{};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = globalDescriptorSets[frameIndex];
	descriptorWrite.dstBinding = 9;
	descriptorWrite.dstArrayElement = index;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.pBufferInfo = &bufferInfo;

	vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
}

// VulkanContext::Impl::UpdateGlobalHiZ is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalHiZ(uint32_t frameIndex) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (frameIndex >= globalDescriptorSets.size()) return;

	std::array<VkDescriptorImageInfo, kHiZViewCount> imageInfos{};
	const auto isReadableHiZ = [](const VulkanHiZ& hiZ) {
		return hiZ.fullView != VK_NULL_HANDLE
			&& hiZ.sampler != VK_NULL_HANDLE
			&& hiZ.layoutInitialized
			&& hiZ.history.valid;
	};
	const VulkanHiZ* samplerOwner = nullptr;
	const VulkanHiZ* fallbackHiZ = nullptr;
	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < kHiZViewCount; ++hiZViewIndex) {
		const VulkanHiZ& hiZ = HiZForFrame(hiZViewIndex, frameIndex);
		if (samplerOwner == nullptr && hiZ.sampler != VK_NULL_HANDLE) {
			samplerOwner = &hiZ;
		}
		if (isReadableHiZ(hiZ)) {
			fallbackHiZ = &hiZ;
			break;
		}
	}
	if (samplerOwner == nullptr) {
		return;
	}

	// Each global descriptor set belongs to one frame slot. Initialization has
	// no submitted work, swapchain recreation already idles the device, and the
	// runtime callers publish only after BeginFrame waits this slot's fence.
	// Waiting for the entire graphics queue here would unnecessarily serialize
	// another in-flight slot to the CPU, with dense virtual geometry paying the
	// full outstanding GPU duration on every Hi-Z promotion.
	VkDescriptorImageInfo samplerInfo{};
	samplerInfo.sampler = samplerOwner->sampler;

	// The sampler is safe before the image has a layout. Binding 10 is partially
	// bound and frame data keeps sampling disabled until a completed write is
	// promoted, so do not publish an UNDEFINED image merely to fill the slot.
	if (fallbackHiZ == nullptr) {
		VkWriteDescriptorSet samplerWrite{};
		samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		samplerWrite.dstSet = globalDescriptorSets[frameIndex];
		samplerWrite.dstBinding = 11;
		samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerWrite.descriptorCount = 1;
		samplerWrite.pImageInfo = &samplerInfo;
		vkUpdateDescriptorSets(device, 1, &samplerWrite, 0, nullptr);
		return;
	}

	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < kHiZViewCount; ++hiZViewIndex) {
		const VulkanHiZ& hiZ = HiZForFrame(hiZViewIndex, frameIndex);
		const VulkanHiZ& readableHiZ = isReadableHiZ(hiZ) ? hiZ : *fallbackHiZ;
		// Binding 10 is an array so editor Scene/Game command buffers can be
		// recorded once and submitted together without racing a descriptor
		// mutation between views. If one editor view has not produced a pyramid
		// yet, keep its descriptor valid with the other view's image while that
		// missing view's frame-data gate keeps Hi-Z sampling fail-open.
		imageInfos[hiZViewIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfos[hiZViewIndex].imageView = readableHiZ.fullView;
		imageInfos[hiZViewIndex].sampler = VK_NULL_HANDLE;
	}

	samplerInfo.sampler = fallbackHiZ->sampler;

	VkWriteDescriptorSet writes[2]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = globalDescriptorSets[frameIndex];
	writes[0].dstBinding = 10;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	writes[0].descriptorCount = kHiZViewCount;
	writes[0].pImageInfo = imageInfos.data();

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = globalDescriptorSets[frameIndex];
	writes[1].dstBinding = 11;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[1].descriptorCount = 1;
	writes[1].pImageInfo = &samplerInfo;

	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

void VulkanContext::Impl::WaitForDescriptorMutationIdle() {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (!initialized || device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE) {
		return;
	}

	// Global bindless sets and mesh-local descriptor sets are persistent across
	// frame slots. Asset reloads and export-runtime streaming can update or free
	// them after a view command buffer has recorded/bound descriptors. Queue idle
	// covers already-submitted work. Recorded view prefixes are submitted first,
	// then proven idle, so descriptor safety never silently drops Scene/Game work.
	if (currentFrame < pendingPrimaryCommandBuffers.size())
	{
		std::vector<VkCommandBuffer>& pendingCommandBuffers = pendingPrimaryCommandBuffers[currentFrame];
		if (!pendingCommandBuffers.empty())
		{
			const bool alreadySubmitted = currentFrame < pendingPrimaryCommandBuffersSubmitted.size()
				&& pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0u;
			if (!alreadySubmitted)
			{
				VkSubmitInfo prefixSubmit{};
				prefixSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				prefixSubmit.commandBufferCount = static_cast<std::uint32_t>(pendingCommandBuffers.size());
				prefixSubmit.pCommandBuffers = pendingCommandBuffers.data();

				VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
				VkPipelineStageFlags acquireStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				const bool consumesRuntimeAcquire = !editorMode
					&& swapchainImageAcquired
					&& !swapchainAcquireWaitConsumed;
				if (consumesRuntimeAcquire)
				{
					acquireSemaphore = imageAvailableSemaphores[currentFrame];
					prefixSubmit.waitSemaphoreCount = 1u;
					prefixSubmit.pWaitSemaphores = &acquireSemaphore;
					prefixSubmit.pWaitDstStageMask = &acquireStage;
				}

				const VkResult submitResult = SubmitGraphicsQueue(1u, &prefixSubmit, VK_NULL_HANDLE);
				if (MarkDeviceLost(submitResult, "recorded view prefix submit before descriptor mutation"))
				{
					return;
				}
				if (submitResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(LogLevel::Error, "failed to submit recorded views before descriptor mutation: " << submitResult);
					return;
				}
				if (currentFrame < pendingPrimaryCommandBuffersSubmitted.size())
				{
					// Descriptor publication can re-enter this synchronization
					// path while asset finalization is still unwinding. Mark the
					// one-time primaries immediately after a successful submit so
					// a nested mutation waits for them instead of submitting the
					// same buffers again.
					pendingPrimaryCommandBuffersSubmitted[currentFrame] = 1u;
				}
				if (consumesRuntimeAcquire) swapchainAcquireWaitConsumed = true;
				ConfirmHiZWritesSubmitted(currentFrame);
				for (RenderSnapshotOwnerSummary& owner : pendingRenderSnapshotDiagnostics.ownerSummaries)
				{
					if (owner.state == RenderSnapshotState::Recorded)
					{
						owner.state = RenderSnapshotState::Submitted;
					}
				}
			}

			const VkResult prefixIdleResult = WaitGraphicsQueueIdle();
			if (MarkDeviceLost(prefixIdleResult, "render command buffer idle before descriptor mutation"))
			{
				return;
			}
			if (prefixIdleResult != VK_SUCCESS)
			{
				TERRAENGINE_LOG(LogLevel::Error, "failed to wait for render command buffers before descriptor mutation: " << prefixIdleResult);
				return;
			}
			if (editorMode && editorRenderSubmitted)
			{
				// EndFrame signaled this slot's binary render-finished semaphore,
				// but an asset/descriptor mutation reached the protected boundary
				// before PresentEditor consumed it. Queue idle proves the signal
				// operation completed; an explicit empty wait consumes that signal
				// before this function clears the one-submit marker and permits a
				// later render prefix to signal the same binary semaphore again.
				VkSemaphore renderSemaphore =
					renderFinishedSemaphores[currentFrame];
				VkPipelineStageFlags renderStage =
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				VkSubmitInfo drainSubmit{};
				drainSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				drainSubmit.waitSemaphoreCount = 1u;
				drainSubmit.pWaitSemaphores = &renderSemaphore;
				drainSubmit.pWaitDstStageMask = &renderStage;
				const VkResult drainResult =
					SubmitGraphicsQueue(1u, &drainSubmit, VK_NULL_HANDLE);
				if (MarkDeviceLost(
						drainResult,
						"descriptor mutation editor render semaphore drain"))
				{
					return;
				}
				if (drainResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(
						LogLevel::Error,
						"failed to consume editor render semaphore before descriptor mutation: "
							<< drainResult);
					return;
				}
				const VkResult drainIdleResult = WaitGraphicsQueueIdle();
				if (MarkDeviceLost(
						drainIdleResult,
						"descriptor mutation editor render semaphore drain idle"))
				{
					return;
				}
				if (drainIdleResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(
						LogLevel::Error,
						"failed to finish editor render semaphore drain before descriptor mutation: "
							<< drainIdleResult);
					return;
				}
				editorRenderSubmitted = false;
			}
			pendingCommandBuffers.clear();
			if (currentFrame < pendingPrimaryCommandBuffersSubmitted.size())
			{
				pendingPrimaryCommandBuffersSubmitted[currentFrame] = 0u;
			}
		}
	}

	const VkResult idleResult = WaitGraphicsQueueIdle();
	if (MarkDeviceLost(idleResult, "descriptor mutation queue idle"))
	{
		return;
	}
	if (idleResult != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to wait for descriptor mutation queue idle: " << idleResult);
	}
}
