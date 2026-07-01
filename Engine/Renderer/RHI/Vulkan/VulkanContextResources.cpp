// VulkanContextResources.cpp — Vulkan Resource Management Implementation
//
// Implements the management of persistent and transient GPU resources within the Vulkan backend.
// This includes the allocation of buffers, creation of image views, and management of descriptor sets and pipeline states.
// It provides specialized caches for meshes, textures, and materials to optimize GPU resource utilization and performance.

#include "VulkanContextImpl.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	constexpr std::uint64_t kClusterRangeHashOffset = 1469598103934665603ull;
	constexpr std::uint64_t kClusterRangeHashPrime = 1099511628211ull;
	constexpr std::uint32_t kFallbackMaterialIndex = 0u;
	constexpr std::uint32_t kFirstRuntimeMaterialIndex = 1u;

	// CreateFallbackMaterialData defines the renderer-owned material contract
	// for missing, loading, or invalid materials. Release builds cannot rely on
	// GPU memory being zeroed, so every unused material slot is initialized with
	// these safe texture indices and scalar defaults.
	MaterialData CreateFallbackMaterialData()
	{
		MaterialData data{};
		data.BaseColorFactor = glm::vec4(1.0f);
		data.EmissiveFactor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		data.UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
		data.MetallicFactor = 0.0f;
		data.RoughnessFactor = 1.0f;
		data.NormalScale = 1.0f;
		data.OcclusionStrength = 1.0f;
		data.AlphaCutoff = 0.5f;
		data.AlphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
		data.ShadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
		data.Flags = 0u;
		data.BaseColorTextureIndex = 0u;
		data.NormalTextureIndex = 1u;
		data.MaskTextureIndex = 2u;
		data.OcclusionTextureIndex = 2u;
		data.EmissiveTextureIndex = 0u;
		data.ShadowBehavior = static_cast<std::uint32_t>(MaterialShadowBehavior::Opaque);
		return data;
	}

	// WriteMappedBuffer copies CPU data into a mapped VMA buffer and flushes the
	// written range before the GPU can copy or read from it. Debug builds often
	// land these uploads in coherent memory, but Release/export runs are free to
	// use non-coherent host-visible memory, so every staging upload must make
	// visibility explicit.
	bool WriteMappedBuffer(VmaAllocator allocator, const VulkanBuffer& buffer, const void* source, VkDeviceSize size, VkDeviceSize offset = 0u)
	{
		if (allocator == VK_NULL_HANDLE
			|| buffer.allocation == VK_NULL_HANDLE
			|| source == nullptr
			|| size == 0u)
		{
			return false;
		}

		void* mapped = nullptr;
		if (vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS)
		{
			return false;
		}

		std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, source, static_cast<std::size_t>(size));
		vmaFlushAllocation(allocator, buffer.allocation, offset, size);
		vmaUnmapMemory(allocator, buffer.allocation);
		return true;
	}

	// HashClusterRangeValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void HashClusterRangeValue(std::uint64_t& hashValue, std::uint64_t input)
	{
		hashValue ^= input;
		hashValue *= kClusterRangeHashPrime;
	}

	// RenderViewTypeToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* RenderViewTypeToString(RenderViewType viewType)
	{
		return ToString(viewType);
	}

	// QualityPresetToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* QualityPresetToString(QualityPreset preset)
	{
		switch (preset)
		{
		case QualityPreset::Low:
			return "Low";
		case QualityPreset::Medium:
			return "Medium";
		case QualityPreset::High:
			return "High";
		case QualityPreset::Ultra:
			return "Ultra";
		case QualityPreset::Custom:
			return "Custom";
		default:
			return "Unknown";
		}
	}

	// LightingReductionCategoryToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* LightingReductionCategoryToString(LightingReductionCategory category)
	{
		switch (category)
		{
		case LightingReductionCategory::None:
			return "None";
		case LightingReductionCategory::Uploaded:
			return "Uploaded";
		case LightingReductionCategory::Special:
			return "Special";
		case LightingReductionCategory::Retained:
			return "Retained";
		case LightingReductionCategory::Shadow:
			return "Shadow";
		default:
			return "Unknown";
		}
	}

	// ShadowResourceKindToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowResourceKindToString(ShadowResourceKind kind)
	{
		switch (kind)
		{
		case ShadowResourceKind::DirectionalCascade:
			return "DirectionalCascade";
		case ShadowResourceKind::PointCube:
			return "PointCube";
		case ShadowResourceKind::SpotAtlas:
			return "SpotAtlas";
		case ShadowResourceKind::AreaAtlas:
			return "AreaAtlas";
		default:
			return "Unsupported";
		}
	}

	// ShadowQualityTierToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowQualityTierToString(ShadowQualityTier tier)
	{
		switch (tier)
		{
		case ShadowQualityTier::FullResolution:
			return "FullResolution";
		case ShadowQualityTier::ReducedResolution:
			return "ReducedResolution";
		case ShadowQualityTier::CachedOnly:
			return "CachedOnly";
		case ShadowQualityTier::Disabled:
			return "Disabled";
		default:
			return "Unknown";
		}
	}

	// ShadowDegradationReasonToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowDegradationReasonToString(ShadowDegradationReason reason)
	{
		switch (reason)
		{
		case ShadowDegradationReason::None:
			return "None";
		case ShadowDegradationReason::LightBudget:
			return "LightBudget";
		case ShadowDegradationReason::StorageBudget:
			return "StorageBudget";
		case ShadowDegradationReason::AllocationFailure:
			return "AllocationFailure";
		case ShadowDegradationReason::UnsupportedLightType:
			return "UnsupportedLightType";
		default:
			return "Unknown";
		}
	}

	// ShadowRefreshReasonToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowRefreshReasonToString(ShadowRefreshReason reason)
	{
		switch (reason)
		{
		case ShadowRefreshReason::RenderedThisFrame:
			return "RenderedThisFrame";
		case ShadowRefreshReason::CacheHit:
			return "CacheHit";
		case ShadowRefreshReason::LightMoved:
			return "LightMoved";
		case ShadowRefreshReason::ReceiverMoved:
			return "ReceiverMoved";
		case ShadowRefreshReason::CacheInvalidated:
			return "CacheInvalidated";
		case ShadowRefreshReason::Unavailable:
			return "Unavailable";
		default:
			return "Unknown";
		}
	}

	// BuildHashHex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string BuildHashHex(std::uint64_t hashValue)
	{
		std::ostringstream stream;
		stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hashValue;
		return stream.str();
	}

	// IsTruthyEnvironmentValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsTruthyEnvironmentValue(const char* value)
	{
		if (value == nullptr || value[0] == '\0')
		{
			return false;
		}

		std::string normalizedValue;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const char* cursor = value; *cursor != '\0'; ++cursor)
		{
			normalizedValue.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*cursor))));
		}

		return normalizedValue == "1"
			|| normalizedValue == "true"
			|| normalizedValue == "yes"
			|| normalizedValue == "on";
	}

	// IsClusterValidationCaptureEnabledFromEnvironment is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsClusterValidationCaptureEnabledFromEnvironment()
	{
		return IsTruthyEnvironmentValue(std::getenv("TERRAENGINE_CLUSTER_VALIDATION_CAPTURE"));
	}
}

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

	VulkanBuffer buffer;
	buffer.size = size;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, nullptr) != VK_SUCCESS) {
		throw std::runtime_error("failed to create buffer!");
	}

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

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	EndSingleTimeCommands(commandBuffer);
}

// VulkanContext::Impl::BeginSingleTimeCommands is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::BeginSingleTimeCommands() {
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = commandPools[currentFrame];
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	return commandBuffer;
}

// VulkanContext::Impl::EndSingleTimeCommands is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EndSingleTimeCommands(VkCommandBuffer commandBuffer) {
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(graphicsQueue);

	vkFreeCommandBuffers(device, commandPools[currentFrame], 1, &commandBuffer);
}

// VulkanContext::Impl::TransitionImageLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

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
	barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

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

	EndSingleTimeCommands(commandBuffer);
}

// VulkanContext::Impl::InitializeHiZPyramidToFarDepth prepares a new Hi-Z image for always-on sampling.
void VulkanContext::Impl::InitializeHiZPyramidToFarDepth(VulkanHiZ& hiZ)
{
	if (hiZ.image == VK_NULL_HANDLE || hiZ.mipLevels == 0u)
	{
		return;
	}

	// Clear all mips to standard-Z far depth. A freshly created pyramid can then
	// be sampled immediately without false occlusion; every meshlet fails open
	// until GenerateHiZPyramid writes real scene depth.
	TransitionImageLayout(hiZ.image, VK_FORMAT_R32_SFLOAT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	VkClearColorValue clearValue{};
	clearValue.float32[0] = 1.0f;
	clearValue.float32[1] = 1.0f;
	clearValue.float32[2] = 1.0f;
	clearValue.float32[3] = 1.0f;

	VkImageSubresourceRange clearRange{};
	clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	clearRange.baseMipLevel = 0u;
	clearRange.levelCount = hiZ.mipLevels;
	clearRange.baseArrayLayer = 0u;
	clearRange.layerCount = 1u;
	vkCmdClearColorImage(commandBuffer, hiZ.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1u, &clearRange);
	EndSingleTimeCommands(commandBuffer);

	TransitionImageLayout(hiZ.image, VK_FORMAT_R32_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	hiZ.history.valid = true;
	hiZ.history.width = hiZ.width;
	hiZ.history.height = hiZ.height;
	hiZ.history.warmupFrames = 1u;
	hiZ.history.invalidReason = VulkanHiZInvalidReason::None;
	hiZ.latestInvalidReason = VulkanHiZInvalidReason::None;
	hiZ.latestValidation = {};
	hiZ.latestValidation.historyValid = true;
	hiZ.latestValidation.occlusionAllowed = true;
	hiZ.latestValidation.width = hiZ.width;
	hiZ.latestValidation.height = hiZ.height;
	hiZ.latestValidation.warmupFrames = 1u;
	hiZ.latestValidation.pyramidReady = true;
	hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::None;
	hiZ.latestValidation.failOpen = false;
	hiZ.latestOcclusionAllowed = true;
}

// VulkanContext::Impl::CopyBufferToImage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

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

	EndSingleTimeCommands(commandBuffer);
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

	TransitionImageLayout(vImage.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	vkCmdCopyBufferToImage(commandBuffer, stagingBuffer.buffer, vImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
	EndSingleTimeCommands(commandBuffer);

	TransitionImageLayout(vImage.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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
	
	uint32_t materialIndex = AllocateMaterialIndex();
	materialIndexMap[materialId] = materialIndex;
	
	VkDeviceSize bufferSize = sizeof(MaterialData);
	VulkanBuffer stagingBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Material Data Staging Buffer");
	WriteMappedBuffer(allocator, stagingBuffer, &data, bufferSize);
	
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0;
	copyRegion.dstOffset = materialIndex * sizeof(MaterialData);
	copyRegion.size = bufferSize;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBOs[i].buffer, 1, &copyRegion);
	}
	EndSingleTimeCommands(commandBuffer);
	
	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	return { globalDescriptorSets[currentFrame] };
}

// VulkanContext::Impl::InvalidateMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateMaterial(TerraEngine::UUID materialId) {
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
		// Clear bindless buffer slot
		UpdateGlobalBuffer(it->second.bindlessIndex, VK_NULL_HANDLE, 0, 0);
		freeBufferIndices.push_back(it->second.bindlessIndex);

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
	InvalidateMesh(id);
	InvalidateTexture(id, reloadTexturesAfterInvalidate);
	InvalidateMaterial(id);
}

// VulkanContext::Impl::EnsureMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureMesh(TerraEngine::UUID meshId) {
	auto existingMeshIt = meshCache.find(meshId);
	if (existingMeshIt != meshCache.end())
	{
		const VulkanMesh& mesh = existingMeshIt->second;
		const VkDeviceSize totalGpuBytes =
			mesh.meshShaderVertexBuffer.size
			+ mesh.meshletVertexRemapBuffer.size
			+ mesh.meshSkinningBuffer.size
			+ mesh.meshDecodeInfoBuffer.size
			+ mesh.indexBuffer.size
			+ mesh.meshletBuffer.size
			+ mesh.meshletSectionBuffer.size
			+ mesh.meshletBoundsBuffer.size;
		ResourceManager::Instance().MarkResourceUploadedToGpu(
			meshId,
			AssetType::Mesh,
			static_cast<std::uint64_t>(currentFrame),
			static_cast<std::uint64_t>(totalGpuBytes),
			"vulkan mesh cache hit");
		return;
	}
	
	Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!cpuMesh) return;

	const VkDeviceSize packedVertexSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexData.size());
	const VkDeviceSize vertexRemapSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexRemapData.size());
	const VkDeviceSize skinningDataSize = static_cast<VkDeviceSize>(cpuMesh->meshletSkinningData.size());
	const VkDeviceSize primitiveIndexSize = static_cast<VkDeviceSize>(cpuMesh->meshletPrimitiveIndexData.size());
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
		|| cpuMesh->header.meshletCount == 0)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload is missing packed vertex/remap/primitive/table data for UUID " << ToHexString(meshId));
		return;
	}

	const uint32_t packedStride = cpuMesh->header.layout.strideBytes;
	const uint32_t meshletVertexCount = cpuMesh->header.meshletVertexCount;
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
	decodeInfo.positionOffset[0] = cpuMesh->header.quantization.positionOffset[0];
	decodeInfo.positionOffset[1] = cpuMesh->header.quantization.positionOffset[1];
	decodeInfo.positionOffset[2] = cpuMesh->header.quantization.positionOffset[2];
	decodeInfo.positionScale[0] = cpuMesh->header.quantization.positionScale[0];
	decodeInfo.positionScale[1] = cpuMesh->header.quantization.positionScale[1];
	decodeInfo.positionScale[2] = cpuMesh->header.quantization.positionScale[2];
	decodeInfo.uvOffsetScale[0] = cpuMesh->header.quantization.uvOffset[0];
	decodeInfo.uvOffsetScale[1] = cpuMesh->header.quantization.uvOffset[1];
	decodeInfo.uvOffsetScale[2] = cpuMesh->header.quantization.uvScale[0];
	decodeInfo.uvOffsetScale[3] = cpuMesh->header.quantization.uvScale[1];
	decodeInfo.counts[0] = cpuMesh->header.meshletVertexCount;
	decodeInfo.counts[1] = cpuMesh->header.meshletVertexRemapCount;
	decodeInfo.counts[2] = cpuMesh->header.meshletPrimitiveIndexCount;
	// counts.w: bits [0:30] = meshletCount, bit 31 = remapEncoding (0=u32, 1=u16)
	decodeInfo.counts[3] = (cpuMesh->header.meshletCount & 0x7FFFFFFFu) | (cpuMesh->header.meshletVertexRemapEncoding << 31u);

	VulkanBuffer stagingPackedVertex = CreateBuffer(packedVertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	WriteMappedBuffer(allocator, stagingPackedVertex, cpuMesh->meshletVertexData.data(), packedVertexSize);

	VulkanBuffer stagingVertexRemap = CreateBuffer(vertexRemapSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	// Zero-copy: upload raw on-disk bytes (u16 or u32) without CPU expansion.
	WriteMappedBuffer(allocator, stagingVertexRemap, cpuMesh->meshletVertexRemapData.data(), vertexRemapSize);

	VulkanBuffer stagingSkinning = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0 };
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (skinningDataSize > 0)
	{
		stagingSkinning = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingSkinning, cpuMesh->meshletSkinningData.data(), skinningDataSize);
	}

	VulkanBuffer stagingIndex = CreateBuffer(primitiveIndexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
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

	VulkanMesh vMesh{};
	// No CPU repack path: mesh shader reads the packed payload directly from storage buffers.
	vMesh.meshShaderVertexBuffer = CreateBuffer(packedVertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.meshletVertexRemapBuffer = CreateBuffer(vertexRemapSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	if (skinningDataSize > 0)
	{
		vMesh.meshSkinningBuffer = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	}
	vMesh.meshDecodeInfoBuffer = CreateBuffer(sizeof(MeshDecodeInfo), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.indexBuffer = CreateBuffer(primitiveIndexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.meshletBuffer = CreateBuffer(meshletTableSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.meshletSectionBuffer = CreateBuffer(meshletSectionSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.meshletBoundsBuffer = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.hasMeshletBounds = hasBoundsPayload;
	vMesh.indexCount = cpuMesh->header.meshletPrimitiveIndexCount;
	vMesh.indexType = VK_INDEX_TYPE_UINT16;
	vMesh.meshletCount = cpuMesh->header.meshletCount;

	CopyBuffer(stagingPackedVertex.buffer, vMesh.meshShaderVertexBuffer.buffer, packedVertexSize);
	CopyBuffer(stagingVertexRemap.buffer, vMesh.meshletVertexRemapBuffer.buffer, vertexRemapSize);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (skinningDataSize > 0)
	{
		CopyBuffer(stagingSkinning.buffer, vMesh.meshSkinningBuffer.buffer, skinningDataSize);
	}
	CopyBuffer(stagingDecodeInfo.buffer, vMesh.meshDecodeInfoBuffer.buffer, sizeof(MeshDecodeInfo));
	CopyBuffer(stagingIndex.buffer, vMesh.indexBuffer.buffer, primitiveIndexSize);
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
	vmaDestroyBuffer(allocator, stagingDecodeInfo.buffer, stagingDecodeInfo.allocation);
	vmaDestroyBuffer(allocator, stagingIndex.buffer, stagingIndex.allocation);
	vmaDestroyBuffer(allocator, stagingMeshletTable.buffer, stagingMeshletTable.allocation);
	vmaDestroyBuffer(allocator, stagingMeshletSection.buffer, stagingMeshletSection.allocation);
	vmaDestroyBuffer(allocator, stagingMeshletBounds.buffer, stagingMeshletBounds.allocation);

	vMesh.bindlessIndex = AllocateBufferIndex();
	UpdateGlobalBuffer(vMesh.bindlessIndex, vMesh.meshDecodeInfoBuffer.buffer, 0, vMesh.meshDecodeInfoBuffer.size);

	meshCache[meshId] = vMesh;
	const VkDeviceSize totalGpuBytes =
		packedVertexSize
		+ vertexRemapSize
		+ skinningDataSize
		+ static_cast<VkDeviceSize>(sizeof(MeshDecodeInfo))
		+ primitiveIndexSize
		+ meshletTableSize
		+ meshletSectionSize
		+ boundsUploadSize;
	const bool hasVirtualGeometryPageMetadata = cpuMesh->hasVirtualGeometry && !cpuMesh->virtualGeometryPages.empty();
	// Virtual geometry pages are still uploaded as part of the owning mesh
	// buffers in this milestone. Keep the publication reason distinct so
	// ResourceManager diagnostics can later split page-resident publication
	// without changing the protected Vulkan upload boundary.
	ResourceManager::Instance().MarkResourceUploadedToGpu(
		meshId,
		AssetType::Mesh,
		static_cast<std::uint64_t>(currentFrame),
		static_cast<std::uint64_t>(totalGpuBytes),
		hasVirtualGeometryPageMetadata ? "vulkan mesh buffer upload with virtual geometry page metadata" : "vulkan mesh buffer upload");
}

// VulkanContext::Impl::EnsureMeshDescriptorSet is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkDescriptorSet VulkanContext::Impl::EnsureMeshDescriptorSet(TerraEngine::UUID meshId, const VulkanPipeline& pipeline) {
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
	bool hasSkinning = mesh.meshSkinningBuffer.buffer != VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasSkinning)
	{
		skinningInfo.buffer = mesh.meshSkinningBuffer.buffer;
		skinningInfo.offset = 0;
		skinningInfo.range = VK_WHOLE_SIZE;
	}

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(7);

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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasSkinning && HasStorageBinding(6))
	{
		w.dstBinding = 6; w.pBufferInfo = &skinningInfo; writes.push_back(w);
	}

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	mesh.descriptorSetsByLayout[layout] = descriptorSet;
	return descriptorSet;
}

// VulkanContext::Impl::BuildPipelineLayouts is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants) {
	if (globalDescriptorSetLayout == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Global descriptor set layout is not initialized; cannot build reflected pipeline layouts.");
		return false;
	}

	// struct AggregatedBinding groups related state and operations so callers have one documented contract.
	struct AggregatedBinding {
		DescriptorBinding binding{};
		VkShaderStageFlags stages = 0;
		bool initialized = false;
	};

	std::map<uint32_t, std::map<uint32_t, AggregatedBinding>> setBindings;
	std::map<std::pair<uint32_t, uint32_t>, VkShaderStageFlags> pushConstantMerge;
	supportsDefaultPushConstants = false;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& stage : collection.stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (!shader) {
			TERRAENGINE_LOG(LogLevel::Error, "Shader stage missing for collection; cannot build pipeline layouts");
			return false;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DescriptorSetLayoutDesc& setDesc : shader->descriptorSets) {
			for (uint32_t idx = 0; idx < setDesc.bindingCount; ++idx) {
				const uint32_t bindingIndex = setDesc.firstBinding + idx;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (bindingIndex >= shader->descriptorBindings.size()) continue;
			const DescriptorBinding& srcBinding = shader->descriptorBindings[bindingIndex];
			auto& aggregated = setBindings[setDesc.setIndex][srcBinding.binding];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!aggregated.initialized) {
				aggregated.binding = srcBinding;
				if (aggregated.binding.arrayCount == 0) {
					aggregated.binding.arrayCount = 1; // default to 1 so descriptors get valid writes
				}
				aggregated.initialized = true;
			} else {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (aggregated.binding.type != srcBinding.type || aggregated.binding.arrayCount != srcBinding.arrayCount) {
					TERRAENGINE_LOG(LogLevel::Warning, "Descriptor binding mismatch across stages for set " << setDesc.setIndex << ", binding " << srcBinding.binding);
				}
				}
				StageVisibilityMask visibility = srcBinding.visibility;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (visibility == 0) {
					visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
				}
				aggregated.stages |= StageVisibilityToVulkan(visibility);
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const PushConstantRange& pc : shader->pushConstants) {
			StageVisibilityMask visibility = pc.visibility;
			if (visibility == 0) {
				visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
			}
			const VkShaderStageFlags vkStages = StageVisibilityToVulkan(visibility);
			std::pair<uint32_t, uint32_t> key = {pc.offsetBytes, pc.sizeBytes};
			pushConstantMerge[key] |= vkStages;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (pc.offsetBytes == 0
				&& pc.sizeBytes >= sizeof(PushConstants)
				&& (vkStages & VK_SHADER_STAGE_MESH_BIT_EXT)) {
				supportsDefaultPushConstants = true;
			}
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [key, stages] : pushConstantMerge) {
		VkPushConstantRange range{};
		range.offset = key.first;
		range.size = key.second;
		range.stageFlags = stages;
		pushConstants.push_back(range);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pushConstants.empty()) {
		VkPushConstantRange fallback{};
		fallback.offset = 0;
		fallback.size = sizeof(PushConstants);
		fallback.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_TASK_BIT_EXT;
		pushConstants.push_back(fallback);
		supportsDefaultPushConstants = true;
	}

	// Set 0 is always the Global Bindless Heap
	setLayouts.push_back(globalDescriptorSetLayout);
	bindingsPerSet.push_back({}); 

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [setIndex, bindingMap] : setBindings) {
		if (setIndex == 0) continue; // Skip Set 0, handled globally

		// Handle gaps if sets are not contiguous (e.g. Set 0 and Set 2 but no Set 1)
		// This simple implementation assumes sequential or just appends. 
		// Real robustness would require resizing setLayouts to setIndex+1.
		// For now, we assume materials use Set 1.
		while (setLayouts.size() <= setIndex) {
			setLayouts.push_back(VK_NULL_HANDLE); // Placeholder for gaps?
			bindingsPerSet.push_back({});
		}
		
		// If we filled a gap with NULL, we need to create a real layout now?
		// Actually, standard behavior: just append. 
		// If setIndex is 1, and size is 1 (Set 0), we push back. Correct.
		
		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		std::vector<DescriptorBinding> storedBindings;
		vkBindings.reserve(bindingMap.size());
		storedBindings.reserve(bindingMap.size());

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& [bindingNumber, aggregated] : bindingMap) {
			VkDescriptorSetLayoutBinding vkBinding{};
			vkBinding.binding = bindingNumber;
			vkBinding.descriptorCount = aggregated.binding.arrayCount;
			vkBinding.descriptorType = MapDescriptorType(static_cast<DescriptorType>(aggregated.binding.type));
			vkBinding.stageFlags = aggregated.stages;
			vkBindings.push_back(vkBinding);
			storedBindings.push_back(aggregated.binding);
		}

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
		layoutInfo.pBindings = vkBindings.data();

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create descriptor set layout from reflection");
			for (size_t i = 1; i < setLayouts.size(); ++i) { // Start from 1 to avoid destroying global layout
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (setLayouts[i] != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayouts[i], nullptr);
			}
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (setLayouts.size() <= setIndex) {
			setLayouts.resize(setIndex + 1, VK_NULL_HANDLE);
			bindingsPerSet.resize(setIndex + 1);
		}
		setLayouts[setIndex] = layout;
		bindingsPerSet[setIndex] = std::move(storedBindings);
	}

	return true;
}

// VulkanContext::Impl::EnsurePipeline is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsurePipeline(TerraEngine::UUID shaderCollectionId, VkRenderPass renderPass, uint32_t materialFlags) {
	PipelineKey key = { shaderCollectionId, materialFlags };

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (renderPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Cannot ensure pipeline: render pass handle is null.");
		return;
	}

	auto passIt = pipelineCache.find(renderPass);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (passIt != pipelineCache.end()) {
		auto cachedPipelineIt = passIt->second.find(key);
		if (cachedPipelineIt != passIt->second.end()) {
			VulkanPipeline& cached = cachedPipelineIt->second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (cached.pipeline != VK_NULL_HANDLE && cached.layout != VK_NULL_HANDLE) {
				return;
			}

			// A stale/invalid cache entry can appear when creation failed previously.
			// Remove it now so the next path can attempt a clean rebuild.
			if (device != VK_NULL_HANDLE) {
				if (cached.pipeline != VK_NULL_HANDLE) {
					vkDestroyPipeline(device, cached.pipeline, nullptr);
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (cached.layout != VK_NULL_HANDLE) {
					vkDestroyPipelineLayout(device, cached.layout, nullptr);
				}
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t layoutIndex = 1; layoutIndex < cached.setLayouts.size(); ++layoutIndex) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (cached.setLayouts[layoutIndex] != VK_NULL_HANDLE) {
						vkDestroyDescriptorSetLayout(device, cached.setLayouts[layoutIndex], nullptr);
					}
				}
			}
			passIt->second.erase(cachedPipelineIt);
		}
	}
	
	ShaderCollection* collection = ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!collection) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to build graphics pipeline: shader collection not found.");
		return;
	}

	const bool shadowPipeline = (renderPass == shadowRenderPass);
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
	bool hasMeshStage = false;
	bool hasTaskStage = false;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& stage : collection->stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (shader) {
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = shader->bytecode.size();
			createInfo.pCode = reinterpret_cast<const uint32_t*>(shader->bytecode.data());

			VkShaderModule shaderModule;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module");
				continue;
			}
			
			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (stage.stage == ShaderStage::Fragment) {
				stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (stage.stage == ShaderStage::Mesh) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!meshShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build mesh pipeline because mesh shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
				hasMeshStage = true;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (stage.stage == ShaderStage::Task) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!taskShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build task shader stage because task shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
				hasTaskStage = true;
			}
			else {
				TERRAENGINE_LOG(LogLevel::Warning, "Skipping unsupported shader stage for graphics pipeline build.");
				vkDestroyShaderModule(device, shaderModule, nullptr);
				continue;
			}
			
			stageInfo.module = shaderModule;
			stageInfo.pName = shader->stringTable.data() + shader->binary.entryPointOffset;
			shaderStages.push_back(stageInfo);
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (shaderStages.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "No shader stages available to build pipeline");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Graphics pipeline must include a mesh stage in this renderer.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasTaskStage && !hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Shader collection includes a task stage without a mesh stage.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<std::vector<DescriptorBinding>> bindingsPerSet;
	std::vector<VkPushConstantRange> pushConstants;
	bool supportsDefaultPushConstants = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!BuildPipelineLayouts(*collection, setLayouts, bindingsPerSet, pushConstants, supportsDefaultPushConstants)) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 0;
	vertexInputInfo.pVertexBindingDescriptions = nullptr;
	vertexInputInfo.vertexAttributeDescriptionCount = 0;
	vertexInputInfo.pVertexAttributeDescriptions = nullptr;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	// Viewport and scissor are dynamic so editor window resizing and reduced
	// shadow targets use the current framebuffer dimensions instead of the size
	// that happened to be active when the pipeline was created.
	viewport.width = shadowPipeline ? static_cast<float>(SHADOW_MAP_RESOLUTION) : static_cast<float>(swapchainExtent.width);
	viewport.height = shadowPipeline ? static_cast<float>(SHADOW_MAP_RESOLUTION) : static_cast<float>(swapchainExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = shadowPipeline
		? VkExtent2D{SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION}
		: swapchainExtent;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	std::array<VkDynamicState, 2> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	// Shadow maps are visibility tests, not final shaded surfaces. Rendering
	// both sides makes primitive winding and negative scale less likely to make
	// a blocker disappear from the light's point of view.
	rasterizer.cullMode = shadowPipeline
		? VK_CULL_MODE_NONE
		: ((materialFlags & DoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	// Shadow pipelines still keep a tiny fragment stage for driver-compatible
	// depth rendering, but the color attachment is only present to satisfy the
	// shared render-pass shape and does not need stored color output.
	colorBlendAttachment.colorWriteMask = shadowPipeline ? 0u : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	colorBlendAttachment.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f;
	depthStencil.maxDepthBounds = 1.0f;
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.front = {};
	depthStencil.back = {};

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
	pipelineLayoutInfo.pPushConstantRanges = pushConstants.data();

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline layout.");
		return;
	}

	VkPipelineFragmentShadingRateStateCreateInfoKHR vrsState{};
	vrsState.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
	vrsState.fragmentSize = { 1, 1 };
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vrsFeatures.primitiveFragmentShadingRate) {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (UsesVrsAttachment()) {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext = &vrsState;
	pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = pipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

	VkPipeline graphicsPipeline = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline.");
		return;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& stage : shaderStages) {
		vkDestroyShaderModule(device, stage.module, nullptr);
	}

	VkShaderStageFlags defaultPushConstantStages = 0;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const VkPushConstantRange& range : pushConstants) {
		defaultPushConstantStages |= range.stageFlags;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (defaultPushConstantStages == 0) {
		defaultPushConstantStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VulkanPipeline pipeline{};
	pipeline.pipeline = graphicsPipeline;
	pipeline.layout = pipelineLayout;
	pipeline.setLayouts = std::move(setLayouts);
	pipeline.bindingsPerSet = std::move(bindingsPerSet);
	pipeline.pushConstants = std::move(pushConstants);
	pipeline.supportsDefaultPushConstants = supportsDefaultPushConstants;
	pipeline.defaultPushConstantStages = defaultPushConstantStages;
	pipeline.hasTaskStage = hasTaskStage;

	pipelineCache[renderPass][key] = std::move(pipeline);
}

// VulkanContext::Impl::CreateGlobalDescriptorResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateGlobalDescriptorResources() {
	std::array<VkDescriptorSetLayoutBinding, 25> bindings{};
	// Textures
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = MAX_BINDLESS_TEXTURES;
	bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[0].pImmutableSamplers = nullptr;
	// Materials
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[1].pImmutableSamplers = nullptr;
	// Frame culling payload (frustum planes + camera data) consumed by task stage.
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[2].pImmutableSamplers = nullptr;
	// Meshlet counters used for stats/debug output.
	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[3].pImmutableSamplers = nullptr;
	// Global light payload array consumed by fragment and compute stages.
	bindings[4].binding = 4;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[4].descriptorCount = 1;
	bindings[4].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[4].pImmutableSamplers = nullptr;
	// Cluster metadata table (offset/count per cluster).
	bindings[5].binding = 5;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[5].descriptorCount = 1;
	bindings[5].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[5].pImmutableSamplers = nullptr;
	// Flat light-index list addressed by the cluster metadata.
	bindings[6].binding = 6;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[6].descriptorCount = 1;
	bindings[6].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[6].pImmutableSamplers = nullptr;
	// Dedicated omnidirectional point-shadow cubemap image used by fragment shading.
	bindings[7].binding = 7;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[7].pImmutableSamplers = nullptr;
	// Dedicated sampler for the point-shadow cubemap.
	bindings[8].binding = 8;
	bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bindings[8].descriptorCount = 1;
	bindings[8].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[8].pImmutableSamplers = nullptr;
	// Bindless Buffers (for MDI and mesh data)
	bindings[9].binding = 9;
	bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[9].descriptorCount = MAX_BINDLESS_BUFFERS;
	bindings[9].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[9].pImmutableSamplers = nullptr;
	// Hi-Z Pyramid Texture
	bindings[10].binding = 10;
	bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[10].descriptorCount = kHiZViewCount;
	bindings[10].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[10].pImmutableSamplers = nullptr;
	// Hi-Z Sampler
	bindings[11].binding = 11;
	bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bindings[11].descriptorCount = 1;
	bindings[11].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[11].pImmutableSamplers = nullptr;
	// Global Depth Buffer for masking and VRS
	bindings[12].binding = 12;
	bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[12].descriptorCount = 1;
	bindings[12].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[12].pImmutableSamplers = nullptr;
	// Cluster Depth Bounds for light culling masking
	bindings[13].binding = 13;
	bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[13].descriptorCount = 1;
	bindings[13].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[13].pImmutableSamplers = nullptr;
	// VRS Shading Rate Image
	bindings[14].binding = 14;
	bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[14].descriptorCount = 1;
	bindings[14].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[14].pImmutableSamplers = nullptr;
	// VRS Color Input
	bindings[15].binding = 15;
	bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[15].descriptorCount = 1;
	bindings[15].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[15].pImmutableSamplers = nullptr;
	// Hi-Z Storage Output (one mip at a time)
	bindings[16].binding = 16;
	bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[16].descriptorCount = 1;
	bindings[16].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[16].pImmutableSamplers = nullptr;
	// Lightweight GPU light-analysis output consumed by the hybrid selection path.
	bindings[17].binding = 17;
	bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[17].descriptorCount = 1;
	bindings[17].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[17].pImmutableSamplers = nullptr;
	// Separate candidate-light input for the GPU analysis pass so the final
	// retained-light upload can use a different buffer in the same frame.
	bindings[18].binding = 18;
	bindings[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[18].descriptorCount = 1;
	bindings[18].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[18].pImmutableSamplers = nullptr;
	// Screen-space ambient occlusion produced by GTAO and sampled by blur and material shading.
	bindings[19].binding = 19;
	bindings[19].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[19].descriptorCount = 1;
	bindings[19].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[19].pImmutableSamplers = nullptr;
	// Point-light shadows are sampled as cubemaps and cannot share the 2D
	// bindless texture heap. Keep a small slot array that mirrors the CPU shadow
	// resource assignment table.
	bindings[20].binding = 20;
	bindings[20].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[20].descriptorCount = MAX_SHADOW_RESOURCE_SLOTS;
	bindings[20].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[20].pImmutableSamplers = nullptr;
	// Previous-frame AO is sampled by the bilateral blur pass for temporal
	// smoothing. It is a separate binding so the raw AO input can keep using 19.
	bindings[21].binding = 21;
	bindings[21].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[21].descriptorCount = 1;
	bindings[21].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[21].pImmutableSamplers = nullptr;
	// Raw AO input is kept separate from the final material AO binding so
	// command recording never depends on rebinding one descriptor to two
	// meanings inside the same command buffer.
	bindings[22].binding = 22;
	bindings[22].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[22].descriptorCount = 1;
	bindings[22].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[22].pImmutableSamplers = nullptr;
	// GTAO raw output uses a dedicated storage binding instead of Hi-Z's
	// storage binding. Sharing binding 16 made optimized frames race against
	// whichever descriptor update happened last during command recording.
	bindings[23].binding = 23;
	bindings[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[23].descriptorCount = 1;
	bindings[23].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[23].pImmutableSamplers = nullptr;
	// GTAO blur output is separate from raw output so blur never reads and
	// writes through the same descriptor binding.
	bindings[24].binding = 24;
	bindings[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[24].descriptorCount = 1;
	bindings[24].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[24].pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{};
	bindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	std::array<VkDescriptorBindingFlags, 25> flags = {
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 0: Textures
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 1: Materials (updated mid-frame)
		0, // 2
		0, // 3
		0, // 4
		0, // 5
		0, // 6
		0, // 7
		0, // 8
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 9: Bindless Buffers
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 10: Hi-Z Texture
		0, // 11: Hi-Z Sampler
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 12: Depth Buffer
		0, // 13: Cluster Depth
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 14: VRS Image
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 15: VRS Color
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 16: Hi-Z Output
		0, // 17: Light analysis output
		0, // 18: Light analysis input
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 19: Ambient occlusion texture
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 20: Point shadow cubemap array
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 21: AO temporal history
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 22: AO raw sampled input
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 23: AO raw storage output
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT // 24: AO blur storage output
	};
	bindingFlags.bindingCount = static_cast<uint32_t>(flags.size());
	bindingFlags.pBindingFlags = flags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	layoutInfo.pNext = &bindingFlags;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &globalDescriptorSetLayout) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create global descriptor set layout!");
		return false;
	}

	std::array<VkDescriptorPoolSize, 5> poolSizes{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[0].descriptorCount = MAX_BINDLESS_TEXTURES * MAX_FRAMES_IN_FLIGHT;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	poolSizes[1].descriptorCount = (6 + kHiZViewCount + MAX_SHADOW_RESOURCE_SLOTS) * MAX_FRAMES_IN_FLIGHT; // Point shadows + per-view Hi-Z + Depth + VRS Color + AO + AO history + AO raw
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	poolSizes[2].descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT; // Point shadow + Hi-Z
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[3].descriptorCount = (9 + MAX_BINDLESS_BUFFERS) * MAX_FRAMES_IN_FLIGHT;
	poolSizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[4].descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT; // VRS Image + Hi-Z Output + AO raw + AO blur

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &globalDescriptorPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create global descriptor pool!");
		return false;
	}

	globalDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = globalDescriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &globalDescriptorSetLayout;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkAllocateDescriptorSets(device, &allocInfo, &globalDescriptorSets[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate global descriptor set for frame " << i);
			return false;
		}
	}

	// Binding 8 is a standalone sampler that the material shader statically
	// declares for point-shadow cubemaps. Initialize it even before any point
	// light requests shadow storage so descriptor validation always sees a
	// valid sampler; real point-shadow allocation overwrites this descriptor
	// with the active cubemap sampler later.
	VkSamplerCreateInfo fallbackPointSamplerInfo{};
	fallbackPointSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	fallbackPointSamplerInfo.magFilter = VK_FILTER_LINEAR;
	fallbackPointSamplerInfo.minFilter = VK_FILTER_LINEAR;
	fallbackPointSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	fallbackPointSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	fallbackPointSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	fallbackPointSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	fallbackPointSamplerInfo.anisotropyEnable = VK_FALSE;
	fallbackPointSamplerInfo.maxAnisotropy = 1.0f;
	fallbackPointSamplerInfo.compareEnable = VK_FALSE;
	fallbackPointSamplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	fallbackPointSamplerInfo.minLod = 0.0f;
	fallbackPointSamplerInfo.maxLod = 1.0f;
	fallbackPointSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	fallbackPointSamplerInfo.unnormalizedCoordinates = VK_FALSE;
	if (vkCreateSampler(device, &fallbackPointSamplerInfo, nullptr, &fallbackPointShadowSampler) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create fallback point-shadow sampler.");
		return false;
	}

	VkDescriptorImageInfo fallbackPointSamplerDescriptor{};
	fallbackPointSamplerDescriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	fallbackPointSamplerDescriptor.imageView = VK_NULL_HANDLE;
	fallbackPointSamplerDescriptor.sampler = fallbackPointShadowSampler;

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkWriteDescriptorSet samplerWrite{};
		samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		samplerWrite.dstSet = globalDescriptorSets[i];
		samplerWrite.dstBinding = 8;
		samplerWrite.dstArrayElement = 0;
		samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerWrite.descriptorCount = 1;
		samplerWrite.pImageInfo = &fallbackPointSamplerDescriptor;
		vkUpdateDescriptorSets(device, 1, &samplerWrite, 0, nullptr);
	}

	return true;
}

// VulkanContext::Impl::CreateMaterialBufferResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateMaterialBufferResources() {
	VkDeviceSize bufferSize = sizeof(MaterialData) * MAX_BINDLESS_MATERIALS;
	globalMaterialSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		globalMaterialSSBOs[i] = CreateBuffer(bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = globalMaterialSSBOs[i].buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = bufferSize;

		VkWriteDescriptorSet descriptorWrite{};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = globalDescriptorSets[i];
		descriptorWrite.dstBinding = 1;
		descriptorWrite.dstArrayElement = 0;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
	}

	const MaterialData fallbackMaterial = CreateFallbackMaterialData();
	std::vector<MaterialData> fallbackMaterials(MAX_BINDLESS_MATERIALS, fallbackMaterial);
	VulkanBuffer stagingBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Fallback Material Staging Buffer");
	void* mappedData = nullptr;
	if (vmaMapMemory(allocator, stagingBuffer.allocation, &mappedData) == VK_SUCCESS) {
		std::memcpy(mappedData, fallbackMaterials.data(), static_cast<std::size_t>(bufferSize));
		vmaFlushAllocation(allocator, stagingBuffer.allocation, 0, bufferSize);
		vmaUnmapMemory(allocator, stagingBuffer.allocation);

		VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
		VkBufferCopy copyRegion{};
		copyRegion.srcOffset = 0u;
		copyRegion.dstOffset = 0u;
		copyRegion.size = bufferSize;
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBOs[i].buffer, 1, &copyRegion);
		}
		EndSingleTimeCommands(commandBuffer);
	}
	else {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to map fallback material staging buffer.");
	}
	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	// Slot zero is permanently reserved for missing/unloaded materials. Real
	// material assets start at one so draw commands that fall back to zero never
	// accidentally sample an authored material or uninitialized GPU memory.
	freeMaterialIndices.clear();
	materialIndexMap.clear();
	nextMaterialIndex = kFirstRuntimeMaterialIndex;
}

// VulkanContext::Impl::CreateCullingBufferResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateCullingBufferResources() {
	const VkDeviceSize frameDataBufferSize = static_cast<VkDeviceSize>(sizeof(FrameCullingDataGPU) * kStatsSlotCount);
	const VkDeviceSize countersBufferSize = static_cast<VkDeviceSize>(sizeof(MeshletCullingCountersGPU) * kStatsSlotCount);

	globalFrameCullingSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	globalMeshletCountersSSBOs.resize(MAX_FRAMES_IN_FLIGHT);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		globalFrameCullingSSBOs[i] = CreateBuffer(frameDataBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		globalMeshletCountersSSBOs[i] = CreateBuffer(countersBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		FrameCullingDataGPU defaultFrameData{};
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0; slotIndex < kStatsSlotCount; ++slotIndex) {
			// Initialize all slots for this frame's buffer
			void* mapped = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vmaMapMemory(allocator, globalFrameCullingSSBOs[i].allocation, &mapped) == VK_SUCCESS) {
				const VkDeviceSize offset = static_cast<VkDeviceSize>(slotIndex) * sizeof(FrameCullingDataGPU);
				std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &defaultFrameData, sizeof(FrameCullingDataGPU));
				vmaFlushAllocation(allocator, globalFrameCullingSSBOs[i].allocation, offset, sizeof(FrameCullingDataGPU));
				vmaUnmapMemory(allocator, globalFrameCullingSSBOs[i].allocation);
			}
		}

		VkDescriptorBufferInfo frameCullingInfo{};
		frameCullingInfo.buffer = globalFrameCullingSSBOs[i].buffer;
		frameCullingInfo.offset = 0;
		frameCullingInfo.range = frameDataBufferSize;

		VkDescriptorBufferInfo meshletCountersInfo{};
		meshletCountersInfo.buffer = globalMeshletCountersSSBOs[i].buffer;
		meshletCountersInfo.offset = 0;
		meshletCountersInfo.range = countersBufferSize;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[i];
		writes[0].dstBinding = 2;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &frameCullingInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[i];
		writes[1].dstBinding = 3;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].descriptorCount = 1;
		writes[1].pBufferInfo = &meshletCountersInfo;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t viewIndex = 0; viewIndex < kStatsViewCount; ++viewIndex) {
		latestStatsByView[viewIndex] = {};
		latestStatsByView[viewIndex].viewType = (viewIndex == 0u) ? RenderViewType::Game : RenderViewType::Scene;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0; slotIndex < kStatsSlotCount; ++slotIndex) {
		pendingStatsBySlot[slotIndex] = {};
		pendingStatsBySlot[slotIndex].viewType = (slotIndex % kStatsViewCount == 0u) ? RenderViewType::Game : RenderViewType::Scene;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (ClusterValidationSnapshot& snapshot : pendingClusterValidationByFrame) {
		snapshot = {};
		snapshot.viewType = RenderViewType::Game;
	}
	previousClusterValidationSnapshot = {};
	previousClusterValidationSnapshot.viewType = RenderViewType::Game;
	previousClusterValidationSnapshotValid = false;
}

// VulkanContext::Impl::CreateLightingBufferResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateLightingBufferResources() {
	const VkDeviceSize lightBufferSize = static_cast<VkDeviceSize>(sizeof(GPULight) * MAX_GLOBAL_LIGHTS * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize clusterBufferSize = static_cast<VkDeviceSize>(sizeof(ClusterData) * MAX_LIGHT_CLUSTERS * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize lightIndexBufferSize = static_cast<VkDeviceSize>(sizeof(uint32_t) * MAX_LIGHT_INDEX_ENTRIES * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize clusterDepthBufferSize = static_cast<VkDeviceSize>(sizeof(float) * 2 * kLightingClusterDimX * kLightingClusterDimY * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize lightAnalysisBufferSize = static_cast<VkDeviceSize>(sizeof(GPULightAnalysisRecord) * MAX_GLOBAL_LIGHTS * kStatsSlotCount);

	clusterValidationCaptureEnabled = IsClusterValidationCaptureEnabledFromEnvironment();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterValidationCaptureEnabled) {
		TERRAENGINE_LOG(LogLevel::Info, "Cluster validation capture enabled via TERRAENGINE_CLUSTER_VALIDATION_CAPTURE. Lighting buffers will remain CPU-visible for frame-end readback.");
	}

	globalLightSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	lightAnalysisInputSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	clusterGridSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	lightIndexSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	clusterDepthSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	lightAnalysisSSBOs.resize(MAX_FRAMES_IN_FLIGHT);

	const VmaMemoryUsage clusterBufferMemoryUsage = clusterValidationCaptureEnabled
		? VMA_MEMORY_USAGE_CPU_TO_GPU
		: VMA_MEMORY_USAGE_GPU_ONLY;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		// Light uploads are CPU-driven every frame, so this buffer must remain
		// CPU-visible regardless of whether validation capture is active.
		globalLightSSBOs[i] = CreateBuffer(lightBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		lightAnalysisInputSSBOs[i] = CreateBuffer(lightBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "Light Analysis Input Buffer");
		clusterGridSSBOs[i] = CreateBuffer(clusterBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterBufferMemoryUsage);
		lightIndexSSBOs[i] = CreateBuffer(lightIndexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterBufferMemoryUsage);
		clusterDepthSSBOs[i] = CreateBuffer(clusterDepthBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterBufferMemoryUsage);
		lightAnalysisSSBOs[i] = CreateBuffer(lightAnalysisBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, "Light Analysis Buffer");

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterValidationCaptureEnabled) {
			// Validation mode seeds buffers with deterministic defaults so the
			// frame-end readback code can flag incomplete writes immediately.
			void* mappedClusterGrid = nullptr;
			if (vmaMapMemory(allocator, clusterGridSSBOs[i].allocation, &mappedClusterGrid) == VK_SUCCESS) {
				std::memset(mappedClusterGrid, 0, static_cast<size_t>(clusterBufferSize));
				vmaFlushAllocation(allocator, clusterGridSSBOs[i].allocation, 0, clusterBufferSize);
				vmaUnmapMemory(allocator, clusterGridSSBOs[i].allocation);
			}

			void* mappedLightIndices = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vmaMapMemory(allocator, lightIndexSSBOs[i].allocation, &mappedLightIndices) == VK_SUCCESS) {
				std::fill_n(static_cast<std::uint32_t*>(mappedLightIndices), MAX_LIGHT_INDEX_ENTRIES * MAX_RENDER_SNAPSHOTS_PER_FRAME, 0xFFFFFFFFu);
				vmaFlushAllocation(allocator, lightIndexSSBOs[i].allocation, 0, lightIndexBufferSize);
				vmaUnmapMemory(allocator, lightIndexSSBOs[i].allocation);
			}

			void* mappedClusterDepth = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vmaMapMemory(allocator, clusterDepthSSBOs[i].allocation, &mappedClusterDepth) == VK_SUCCESS) {
				std::fill_n(static_cast<float*>(mappedClusterDepth), static_cast<size_t>(clusterDepthBufferSize / sizeof(float)), 1.0f);
				vmaFlushAllocation(allocator, clusterDepthSSBOs[i].allocation, 0, clusterDepthBufferSize);
				vmaUnmapMemory(allocator, clusterDepthSSBOs[i].allocation);
			}
		}

		void* mappedLightAnalysis = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaMapMemory(allocator, lightAnalysisSSBOs[i].allocation, &mappedLightAnalysis) == VK_SUCCESS) {
			std::memset(mappedLightAnalysis, 0, static_cast<size_t>(lightAnalysisBufferSize));
			vmaFlushAllocation(allocator, lightAnalysisSSBOs[i].allocation, 0, lightAnalysisBufferSize);
			vmaUnmapMemory(allocator, lightAnalysisSSBOs[i].allocation);
		}

		VkDescriptorBufferInfo lightInfo{};
		lightInfo.buffer = globalLightSSBOs[i].buffer;
		lightInfo.offset = 0;
		lightInfo.range = lightBufferSize;

		VkDescriptorBufferInfo clusterInfo{};
		clusterInfo.buffer = clusterGridSSBOs[i].buffer;
		clusterInfo.offset = 0;
		clusterInfo.range = clusterBufferSize;

		VkDescriptorBufferInfo lightIndexInfo{};
		lightIndexInfo.buffer = lightIndexSSBOs[i].buffer;
		lightIndexInfo.offset = 0;
		lightIndexInfo.range = lightIndexBufferSize;

		VkDescriptorImageInfo depthInfo{};
		depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		depthInfo.imageView = depthImageView;
		// Cluster-depth consumers only need a sampler object. Reuse the
		// sampler from the same frame's Hi-Z resource so descriptor set `i`
		// never references a sampler owned by another frame slot.
		depthInfo.sampler = HiZForFrame(kHiZGameViewIndex, i).sampler;

		VkDescriptorBufferInfo clusterDepthInfo{};
		clusterDepthInfo.buffer = clusterDepthSSBOs[i].buffer;
		clusterDepthInfo.offset = 0;
		clusterDepthInfo.range = clusterDepthBufferSize;

		VkDescriptorBufferInfo lightAnalysisInfo{};
		lightAnalysisInfo.buffer = lightAnalysisSSBOs[i].buffer;
		lightAnalysisInfo.offset = 0;
		lightAnalysisInfo.range = lightAnalysisBufferSize;

		VkDescriptorBufferInfo lightAnalysisInputInfo{};
		lightAnalysisInputInfo.buffer = lightAnalysisInputSSBOs[i].buffer;
		lightAnalysisInputInfo.offset = 0;
		lightAnalysisInputInfo.range = lightBufferSize;

		std::array<VkWriteDescriptorSet, 7> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[i];
		writes[0].dstBinding = 4;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &lightInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[i];
		writes[1].dstBinding = 5;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].descriptorCount = 1;
		writes[1].pBufferInfo = &clusterInfo;

		writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[2].dstSet = globalDescriptorSets[i];
		writes[2].dstBinding = 6;
		writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[2].descriptorCount = 1;
		writes[2].pBufferInfo = &lightIndexInfo;

		writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[3].dstSet = globalDescriptorSets[i];
		writes[3].dstBinding = 12;
		writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[3].descriptorCount = 1;
		writes[3].pImageInfo = &depthInfo;

		writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[4].dstSet = globalDescriptorSets[i];
		writes[4].dstBinding = 13;
		writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[4].descriptorCount = 1;
		writes[4].pBufferInfo = &clusterDepthInfo;

		writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[5].dstSet = globalDescriptorSets[i];
		writes[5].dstBinding = 17;
		writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[5].descriptorCount = 1;
		writes[5].pBufferInfo = &lightAnalysisInfo;

		writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[6].dstSet = globalDescriptorSets[i];
		writes[6].dstBinding = 18;
		writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[6].descriptorCount = 1;
		writes[6].pBufferInfo = &lightAnalysisInputInfo;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}

	UploadLightingData({});
}

// VulkanContext::Impl::CreateIndirectDrawResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateIndirectDrawResources() {
	VkDeviceSize indirectBufferSize = MAX_BINDLESS_DRAWS * MAX_RENDER_SNAPSHOTS_PER_FRAME * sizeof(VkDrawMeshTasksIndirectCommandEXT);
	indirectDrawBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		indirectDrawBuffers[i] = CreateBuffer(indirectBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
}

// VulkanContext::Impl::DestroyEnvironmentLightingResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyEnvironmentLightingResources()
{
	auto DestroyImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image = {};
	};

	DestroyImage(environmentIrradianceTexture);
	DestroyImage(prefilteredEnvironmentTexture);
	DestroyImage(brdfIntegrationTexture);
	environmentIrradianceTextureIndex = 0u;
	prefilteredEnvironmentTextureIndex = 0u;
	brdfIntegrationTextureIndex = 0u;
}

// VulkanContext::Impl::CreateEnvironmentLightingResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateEnvironmentLightingResources()
{
	if (environmentIrradianceTexture.view != VK_NULL_HANDLE
		&& prefilteredEnvironmentTexture.view != VK_NULL_HANDLE
		&& brdfIntegrationTexture.view != VK_NULL_HANDLE) {
		return;
	}

	DestroyEnvironmentLightingResources();

	auto CreateFloatTexture = [&](const std::vector<glm::vec4>& texels, uint32_t width, uint32_t height, const char* allocationName, VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (texels.empty() || width == 0u || height == 0u) {
			return;
		}

		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(sizeof(glm::vec4) * texels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Environment Lighting Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, texels.data(), uploadSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { width, height, 1u };
		imageInfo.mipLevels = 1u;
		imageInfo.arrayLayers = 1u;
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
			image = {};
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, allocationName);

		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, image.image, width, height);
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view == VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
			return;
		}

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler);
	};

	// These deterministic fallback textures give the renderer real IBL
	// descriptors before imported sky probes exist. The material shader can
	// continue using its procedural fallback without risking unbound resources.
	CreateFloatTexture({ glm::vec4(0.18f, 0.22f, 0.28f, 1.0f) }, 1u, 1u, "Fallback Environment Irradiance", environmentIrradianceTexture);
	CreateFloatTexture({ glm::vec4(0.45f, 0.55f, 0.72f, 1.0f) }, 1u, 1u, "Fallback Prefiltered Environment", prefilteredEnvironmentTexture);

	const uint32_t brdfSize = 64u;
	std::vector<glm::vec4> brdfTexels(brdfSize * brdfSize);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t y = 0u; y < brdfSize; ++y) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t x = 0u; x < brdfSize; ++x) {
			const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(brdfSize);
			const float nDotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(brdfSize);
			brdfTexels[y * brdfSize + x] = glm::vec4(1.0f - 0.35f * roughness, nDotV * (1.0f - roughness), 0.0f, 1.0f);
		}
	}
	CreateFloatTexture(brdfTexels, brdfSize, brdfSize, "Fallback BRDF Integration LUT", brdfIntegrationTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (environmentIrradianceTexture.view != VK_NULL_HANDLE) {
		environmentIrradianceTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(environmentIrradianceTextureIndex, environmentIrradianceTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (prefilteredEnvironmentTexture.view != VK_NULL_HANDLE) {
		prefilteredEnvironmentTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(prefilteredEnvironmentTextureIndex, prefilteredEnvironmentTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (brdfIntegrationTexture.view != VK_NULL_HANDLE) {
		brdfIntegrationTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(brdfIntegrationTextureIndex, brdfIntegrationTexture);
	}
}

// VulkanContext::Impl::DestroyAmbientOcclusionResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyAmbientOcclusionResources()
{
	auto DestroyImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image = {};
	};

	for (VulkanImage& image : ambientOcclusionRawByFrame) {
		DestroyImage(image);
	}
	for (VulkanImage& image : ambientOcclusionBlurredByFrame) {
		DestroyImage(image);
	}
	for (VulkanImage& image : ambientOcclusionHistoryByFrame) {
		DestroyImage(image);
	}
	for (ViewDepthHistoryState& viewHistory : depthHistoryByView) {
		for (VulkanImage& image : viewHistory.ambientOcclusionDepthByFrame) {
			DestroyImage(image);
		}
		viewHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}
	ambientOcclusionWidth = 0u;
	ambientOcclusionHeight = 0u;
	ambientOcclusionHistoryValidByFrame.fill(false);
}

// VulkanContext::Impl::CreateAmbientOcclusionResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateAmbientOcclusionResources()
{
	const uint32_t width = std::max(swapchainExtent.width, 1u);
	const uint32_t height = std::max(swapchainExtent.height, 1u);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ambientOcclusionRawByFrame.empty()
		&& ambientOcclusionRawByFrame[0].image != VK_NULL_HANDLE
		&& ambientOcclusionBlurredByFrame[0].image != VK_NULL_HANDLE
		&& ambientOcclusionHistoryByFrame[0].image != VK_NULL_HANDLE
		&& depthHistoryByView[0].ambientOcclusionDepthByFrame[0].image != VK_NULL_HANDLE
		&& ambientOcclusionWidth == width
		&& ambientOcclusionHeight == height) {
		return;
	}

	DestroyAmbientOcclusionResources();

	auto CreateAoImage = [&](const char* allocationName, VulkanImage& image) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { width, height, 1u };
		imageInfo.mipLevels = 1u;
		imageInfo.arrayLayers = 1u;
		imageInfo.format = AMBIENT_OCCLUSION_FORMAT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		// AO images are shader-readable/writable, clearable during creation,
		// and the resolved frame image is copied into the history image after
		// blur. Include both transfer directions so the temporal path never has
		// to swap Vulkan handles or rewrite material descriptors mid-frame.
		imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image = {};
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, allocationName);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view == VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
			return;
		}

		// Newly created AO targets must contain explicit visibility data before
		// any optimized shader is allowed to sample them. A value of 1.0 means
		// fully open/no contact darkening, which is the safe fallback until the
		// compute pass writes real occlusion.
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkCommandBuffer clearCommandBuffer = BeginSingleTimeCommands();
		VkImageSubresourceRange clearRange{};
		clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		clearRange.baseMipLevel = 0u;
		clearRange.levelCount = 1u;
		clearRange.baseArrayLayer = 0u;
		clearRange.layerCount = 1u;
		VkClearColorValue clearValue{};
		clearValue.float32[0] = 1.0f;
		clearValue.float32[1] = 1.0f;
		clearValue.float32[2] = 1.0f;
		clearValue.float32[3] = 1.0f;
		vkCmdClearColorImage(clearCommandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1u, &clearRange);
		EndSingleTimeCommands(clearCommandBuffer);
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	};

	auto CreateAoDepthImage = [&](const char* allocationName, VulkanImage& image) {
		const VkFormat historyDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { width, height, 1u };
		imageInfo.mipLevels = 1u;
		imageInfo.arrayLayers = 1u;
		imageInfo.format = historyDepthFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image = {};
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, allocationName);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_DEPTH_BIT);
		if (image.view == VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
			return;
		}

		// The history image starts readable so descriptor sets can be populated
		// immediately; it is sampled like a texture, not used as a depth
		// attachment, so keep it in shader-read layout between transfer writes.
		VkCommandBuffer transitionCommandBuffer = BeginSingleTimeCommands();
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = 0u;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			transitionCommandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier);
		EndSingleTimeCommands(transitionCommandBuffer);
	};

	bool allImagesReady = true;
	for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
		CreateAoImage("GTAO Raw Occlusion", ambientOcclusionRawByFrame[frameIndex]);
		CreateAoImage("GTAO Blurred Occlusion", ambientOcclusionBlurredByFrame[frameIndex]);
		CreateAoImage("GTAO History Occlusion", ambientOcclusionHistoryByFrame[frameIndex]);
		allImagesReady = allImagesReady
			&& ambientOcclusionRawByFrame[frameIndex].view != VK_NULL_HANDLE
			&& ambientOcclusionBlurredByFrame[frameIndex].view != VK_NULL_HANDLE
			&& ambientOcclusionHistoryByFrame[frameIndex].view != VK_NULL_HANDLE;
	}
	for (ViewDepthHistoryState& viewHistory : depthHistoryByView) {
		for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
			CreateAoDepthImage("GTAO Depth History", viewHistory.ambientOcclusionDepthByFrame[frameIndex]);
			allImagesReady = allImagesReady
				&& viewHistory.ambientOcclusionDepthByFrame[frameIndex].view != VK_NULL_HANDLE;
		}
		viewHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}
	ambientOcclusionWidth = allImagesReady ? width : 0u;
	ambientOcclusionHeight = allImagesReady ? height : 0u;
	ambientOcclusionHistoryValidByFrame.fill(false);

	if (!allImagesReady) {
		return;
	}

	for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
		VkDescriptorImageInfo finalAoInfo{};
		finalAoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		finalAoInfo.imageView = ambientOcclusionBlurredByFrame[frameIndex].view;
		finalAoInfo.sampler = VK_NULL_HANDLE;

		// Global binding 19 is the only AO descriptor consumed by material
		// shading. Compute-only AO inputs and outputs are bound through the
		// private per-snapshot AO descriptor set during GenerateAmbientOcclusion.
		std::array<VkWriteDescriptorSet, 1> descriptorWrites{};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].dstSet = globalDescriptorSets[frameIndex];
		descriptorWrites[0].dstBinding = 19;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pImageInfo = &finalAoInfo;
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}
}

// VulkanContext::Impl::EnsureShadowRenderResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::EnsureShadowRenderResources(const ShadowResourceRequest& request) {
	ShadowResourceRequest targetRequest{};
	targetRequest.directionalCount = std::min(request.directionalCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.spotCount = std::min(request.spotCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.areaCount = std::min(request.areaCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.pointCount = std::min(request.pointCount, MAX_SHADOW_RESOURCE_SLOTS);
	vkDeviceWaitIdle(device);
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
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Color Image");
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
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Color Image");

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

// VulkanContext::Impl::RecordPendingStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingStats(uint32_t statsSlotIndex, const RenderStatsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingStatsBySlot[statsSlotIndex] = snapshot;
}

// VulkanContext::Impl::RecordPendingBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingBudgetDiagnostics(uint32_t statsSlotIndex, const FrameBudgetDiagnosticsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingBudgetDiagnosticsBySlot[statsSlotIndex] = snapshot;
}

// VulkanContext::Impl::RecordPendingLightAnalysisCandidates is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingLightAnalysisCandidates(uint32_t statsSlotIndex, const std::vector<FrameLightCandidate>& candidates) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingLightAnalysisCandidatesBySlot[statsSlotIndex] = candidates;
}

// VulkanContext::Impl::PublishCompletedFrameStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::PublishCompletedFrameStats(uint32_t frameIndex) {
	const uint32_t gameSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Game);
	const uint32_t sceneSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Scene);

	RenderStatsSnapshot gameSnapshot = pendingStatsBySlot[gameSlot];
	const MeshletCullingCountersGPU gameCounters = ReadMeshletCounters(gameSlot);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (gameSnapshot.valid) {
		const bool useGpuCounters = (gameCounters.meshletsTotal > 0u) || (gameSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			gameSnapshot.meshletsTotal = gameCounters.meshletsTotal;
			gameSnapshot.meshletsDispatched = gameCounters.meshletsDispatched;
			gameSnapshot.meshletsCulled = gameCounters.meshletsCulled;
			gameSnapshot.meshletsCulledHiZ = gameCounters.meshletsCulledHiZ;
			gameSnapshot.hiZTests = gameCounters.hiZTests;
			gameSnapshot.hiZProjectionFailed = gameCounters.hiZProjectionFailed;
			gameSnapshot.hiZFullRectCulled = gameCounters.hiZFullRectCulled;
			gameSnapshot.hiZRectCoverageCulled = gameCounters.hiZRectCoverageCulled;
			gameSnapshot.hiZCenterCulled = gameCounters.hiZCenterCulled;
			gameSnapshot.hiZVertexCoverageCulled = gameCounters.hiZVertexCoverageCulled;
			gameSnapshot.hiZNoOcclusionProof = gameCounters.hiZNoOcclusionProof;
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::GPUReadback;
			gameSnapshot.meshletCounterFrameId = frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = 0u;
			gameSnapshot.meshletCounterReason.clear();
		} else if (gameSnapshot.taskCullingActive) {
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::DelayedGPUReadback;
			gameSnapshot.meshletCounterFrameId = frameIndex > 0u ? frameIndex - 1u : frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = frameIndex > 0u ? 1u : 0u;
			gameSnapshot.meshletCounterReason = "GPU meshlet counter readback is delayed; showing prepared counts.";
		} else {
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::Prepared;
			gameSnapshot.meshletCounterFrameId = frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = 0u;
			gameSnapshot.meshletCounterReason = "Task culling is inactive; counts are prepared CPU estimates.";
		}
		gameSnapshot.hiZActive = gameSnapshot.hiZActive && HiZForFrame(kHiZGameViewIndex, frameIndex).image != VK_NULL_HANDLE;
		gameSnapshot.vrsActive = gameSnapshot.vrsActive && UsesVrsAttachment();
	}

	RenderStatsSnapshot sceneSnapshot = pendingStatsBySlot[sceneSlot];
	const MeshletCullingCountersGPU sceneCounters = ReadMeshletCounters(sceneSlot);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (sceneSnapshot.valid) {
		const bool useGpuCounters = (sceneCounters.meshletsTotal > 0u) || (sceneSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			sceneSnapshot.meshletsTotal = sceneCounters.meshletsTotal;
			sceneSnapshot.meshletsDispatched = sceneCounters.meshletsDispatched;
			sceneSnapshot.meshletsCulled = sceneCounters.meshletsCulled;
			sceneSnapshot.meshletsCulledHiZ = sceneCounters.meshletsCulledHiZ;
			sceneSnapshot.hiZTests = sceneCounters.hiZTests;
			sceneSnapshot.hiZProjectionFailed = sceneCounters.hiZProjectionFailed;
			sceneSnapshot.hiZFullRectCulled = sceneCounters.hiZFullRectCulled;
			sceneSnapshot.hiZRectCoverageCulled = sceneCounters.hiZRectCoverageCulled;
			sceneSnapshot.hiZCenterCulled = sceneCounters.hiZCenterCulled;
			sceneSnapshot.hiZVertexCoverageCulled = sceneCounters.hiZVertexCoverageCulled;
			sceneSnapshot.hiZNoOcclusionProof = sceneCounters.hiZNoOcclusionProof;
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::GPUReadback;
			sceneSnapshot.meshletCounterFrameId = frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = 0u;
			sceneSnapshot.meshletCounterReason.clear();
		} else if (sceneSnapshot.taskCullingActive) {
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::DelayedGPUReadback;
			sceneSnapshot.meshletCounterFrameId = frameIndex > 0u ? frameIndex - 1u : frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = frameIndex > 0u ? 1u : 0u;
			sceneSnapshot.meshletCounterReason = "GPU meshlet counter readback is delayed; showing prepared counts.";
		} else {
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::Prepared;
			sceneSnapshot.meshletCounterFrameId = frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = 0u;
			sceneSnapshot.meshletCounterReason = "Task culling is inactive; counts are prepared CPU estimates.";
		}
		sceneSnapshot.hiZActive = sceneSnapshot.hiZActive && HiZForFrame(kHiZSceneViewIndex, frameIndex).image != VK_NULL_HANDLE;
		sceneSnapshot.vrsActive = sceneSnapshot.vrsActive && UsesVrsAttachment();
	}

	const FrameBudgetDiagnosticsSnapshot gameBudgetDiagnostics = pendingBudgetDiagnosticsBySlot[gameSlot];
	const FrameBudgetDiagnosticsSnapshot sceneBudgetDiagnostics = pendingBudgetDiagnosticsBySlot[sceneSlot];
	const auto buildLightingDiagnostics = [this](const FrameBudgetDiagnosticsSnapshot& budgetDiagnostics) {
		LightingDiagnosticsSnapshot snapshot{};
		snapshot.viewType = budgetDiagnostics.viewType;
		snapshot.valid = budgetDiagnostics.valid;
		snapshot.indirectLightingActive = true;
		const bool gtaoAvailable = ambientOcclusionBlurredByFrame[0].image != VK_NULL_HANDLE
			&& ambientOcclusionBlurredByFrame[0].view != VK_NULL_HANDLE
			&& gtaoComputePipeline != VK_NULL_HANDLE
			&& gtaoBlurComputePipeline != VK_NULL_HANDLE;
		snapshot.ambientOcclusionActive = gtaoAvailable;

		LightingDiagnosticEntry shadowEntry{};
		shadowEntry.category = LightingDiagnosticCategory::Shadow;
		shadowEntry.status = budgetDiagnostics.shadowReductionOccurred
			? LightingDiagnosticStatus::Degraded
			: LightingDiagnosticStatus::Active;
		shadowEntry.message = budgetDiagnostics.shadowReductionOccurred
			? "Some shadow-capable retained lights did not receive shadow resources this frame."
			: "Shadow-capable retained lights are within the active shadow budget.";
		shadowEntry.recommendedAction = budgetDiagnostics.shadowReductionOccurred
			? "Increase the shadow light budget, lower shadow demand, or raise important lights' priority."
			: "";
		snapshot.entries.push_back(std::move(shadowEntry));

		LightingDiagnosticEntry shadowMemoryEntry{};
		shadowMemoryEntry.category = LightingDiagnosticCategory::Shadow;
		shadowMemoryEntry.status = budgetDiagnostics.allocationFailureCount > 0u
			? LightingDiagnosticStatus::Unavailable
			: LightingDiagnosticStatus::Active;
		shadowMemoryEntry.message =
			"Shadow storage allocated "
			+ std::to_string(budgetDiagnostics.allocatedRegionCount)
			+ " active regions for "
			+ std::to_string(budgetDiagnostics.activeShadowCount)
			+ " shadowed lights.";
		shadowMemoryEntry.recommendedAction = budgetDiagnostics.allocationFailureCount > 0u
			? "Reduce shadow demand or inspect Vulkan allocation failures."
			: "";
		snapshot.entries.push_back(std::move(shadowMemoryEntry));

		const auto shadowKindName = [](ShadowResourceKind kind) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (kind)
			{
			case ShadowResourceKind::DirectionalCascade:
				return "directional cascade";
			case ShadowResourceKind::PointCube:
				return "point cubemap";
			case ShadowResourceKind::SpotAtlas:
				return "spot atlas";
			case ShadowResourceKind::AreaAtlas:
				return "area atlas";
			default:
				return "unsupported shadow resource";
			}
		};

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ShadowResourceAssignmentDiagnostic& assignment : budgetDiagnostics.shadowAssignments)
		{
			LightingDiagnosticEntry assignmentEntry{};
			assignmentEntry.category = LightingDiagnosticCategory::Shadow;
			assignmentEntry.status = assignment.assigned
				? LightingDiagnosticStatus::Active
				: LightingDiagnosticStatus::Degraded;
			assignmentEntry.lightId = assignment.lightId;
			assignmentEntry.message = assignment.assigned
				? std::string("Light received ") + shadowKindName(assignment.resourceKind) + " slot " + std::to_string(assignment.resourceSlot) + "."
				: std::string("Light requested shadows but did not receive a ") + shadowKindName(assignment.resourceKind) + " slot this frame.";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assignment.reusedPreviousOwner)
			{
				assignmentEntry.message += " Reused previous assignment for stability.";
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assignment.replacedPreviousOwner)
			{
				assignmentEntry.message += " Replaced the previous owner of this slot.";
			}
			assignmentEntry.recommendedAction = assignment.assigned
				? ""
				: "Increase maxShadowLights or raise this light's authored priority.";
			snapshot.entries.push_back(std::move(assignmentEntry));
		}

		LightingDiagnosticEntry indirectEntry{};
		indirectEntry.category = LightingDiagnosticCategory::IndirectLighting;
		indirectEntry.status = LightingDiagnosticStatus::Active;
		indirectEntry.message = "Indirect lighting is evaluated separately from hard direct shadow visibility.";
		snapshot.entries.push_back(std::move(indirectEntry));

		LightingDiagnosticEntry aoEntry{};
		aoEntry.category = LightingDiagnosticCategory::AmbientOcclusion;
		aoEntry.status = gtaoAvailable
			? LightingDiagnosticStatus::Active
			: LightingDiagnosticStatus::Fallback;
		aoEntry.message = gtaoAvailable
			? "GTAO contact darkening is generated before material shading and applied to indirect lighting."
			: "Contact darkening uses material occlusion until the GTAO compute pass is available.";
		aoEntry.recommendedAction = gtaoAvailable
			? ""
			: "Check GTAO shader packaging and descriptor creation if screen-space contact darkening should be active.";
		snapshot.entries.push_back(std::move(aoEntry));

		LightingDiagnosticEntry materialEntry{};
		materialEntry.category = LightingDiagnosticCategory::MaterialShadow;
		materialEntry.status = LightingDiagnosticStatus::Fallback;
		materialEntry.message = "Opaque and alpha-cutout materials cast shadows; blended transparent materials currently do not cast shadows.";
		materialEntry.recommendedAction = "Use Mask alpha mode for cutout objects that must cast silhouette shadows.";
		snapshot.entries.push_back(std::move(materialEntry));

		LightingDiagnosticEntry budgetEntry{};
		budgetEntry.category = LightingDiagnosticCategory::Budget;
		budgetEntry.status = budgetDiagnostics.budgetReductionOccurred
			? LightingDiagnosticStatus::Degraded
			: LightingDiagnosticStatus::Active;
		budgetEntry.message = budgetDiagnostics.budgetReductionOccurred
			? "The scene exceeded one or more active lighting budgets."
			: "The scene is within active lighting budgets.";
		budgetEntry.recommendedAction = budgetDiagnostics.budgetReductionOccurred
			? "Use quality settings or light priorities to control which lights are retained."
			: "";
		snapshot.entries.push_back(std::move(budgetEntry));
		return snapshot;
	};
	const LightingDiagnosticsSnapshot gameLightingDiagnostics = buildLightingDiagnostics(gameBudgetDiagnostics);
	const LightingDiagnosticsSnapshot sceneLightingDiagnostics = buildLightingDiagnostics(sceneBudgetDiagnostics);

	GpuLightAnalysisViewSnapshot gameLightAnalysis{};
	GpuLightAnalysisViewSnapshot sceneLightAnalysis{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex < lightAnalysisSSBOs.size() && lightAnalysisSSBOs[frameIndex].buffer != VK_NULL_HANDLE) {
		const VulkanBuffer& analysisBuffer = lightAnalysisSSBOs[frameIndex];
		void* mappedAnalysis = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaMapMemory(allocator, analysisBuffer.allocation, &mappedAnalysis) == VK_SUCCESS) {
			vmaInvalidateAllocation(allocator, analysisBuffer.allocation, 0, analysisBuffer.size);
			const auto* analysisRecords = static_cast<const GPULightAnalysisRecord*>(mappedAnalysis);
			const auto readSnapshot = [&](uint32_t statsSlotIndex) {
				GpuLightAnalysisViewSnapshot snapshot{};
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (statsSlotIndex >= pendingLightAnalysisCandidatesBySlot.size()) {
					return snapshot;
				}

				const std::vector<FrameLightCandidate>& candidates = pendingLightAnalysisCandidatesBySlot[statsSlotIndex];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (candidates.empty()) {
					snapshot.valid = true;
					return snapshot;
				}

				const std::size_t clampedCount = std::min<std::size_t>(candidates.size(), MAX_GLOBAL_LIGHTS);
				const std::size_t baseIndex = static_cast<std::size_t>(statsSlotIndex) * static_cast<std::size_t>(MAX_GLOBAL_LIGHTS);
				snapshot.recordsByLightId.reserve(clampedCount);
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (std::size_t lightIndex = 0; lightIndex < clampedCount; ++lightIndex) {
					const GPULightAnalysisRecord& record = analysisRecords[baseIndex + lightIndex];
					snapshot.recordsByLightId.emplace(candidates[lightIndex].lightId, record);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if ((record.flags & kGpuLightAnalysisFlagVisible) != 0u) {
						snapshot.visibleCandidateCount += 1u;
					}
				}
				snapshot.valid = true;
				return snapshot;
			};

			gameLightAnalysis = readSnapshot(gameSlot);
			sceneLightAnalysis = readSnapshot(sceneSlot);
			vmaUnmapMemory(allocator, analysisBuffer.allocation);
		}
	}

	ClusterValidationSnapshot clusterSnapshot{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterValidationCaptureEnabled) {
		clusterSnapshot = ReadCompletedClusterValidationSnapshot(frameIndex);
	}

	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestStatsByView[0] = gameSnapshot;
		latestStatsByView[1] = sceneSnapshot;
		latestBudgetDiagnosticsByView[0] = gameBudgetDiagnostics;
		latestBudgetDiagnosticsByView[1] = sceneBudgetDiagnostics;
		latestLightingDiagnosticsByView[0] = gameLightingDiagnostics;
		latestLightingDiagnosticsByView[1] = sceneLightingDiagnostics;
		latestGpuLightAnalysisByView[0] = std::move(gameLightAnalysis);
		latestGpuLightAnalysisByView[1] = std::move(sceneLightAnalysis);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterSnapshot.valid) {
			previousClusterValidationSnapshot = clusterSnapshot;
			previousClusterValidationSnapshotValid = true;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterSnapshot.valid) {
		const FrameBudgetDiagnosticsSnapshot& captureDiagnostics = (clusterSnapshot.viewType == RenderViewType::Scene)
			? sceneBudgetDiagnostics
			: gameBudgetDiagnostics;
		WriteClusterValidationCapture(clusterSnapshot, captureDiagnostics);
	}
}

// VulkanContext::Impl::GetLatestStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot VulkanContext::Impl::GetLatestStats(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestStatsByView[index];
}

// VulkanContext::Impl::GetLatestBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot VulkanContext::Impl::GetLatestBudgetDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestBudgetDiagnosticsByView[index];
}

// VulkanContext::Impl::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot VulkanContext::Impl::GetLatestLightingDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestLightingDiagnosticsByView[index];
}

// VulkanContext::Impl::GetLatestGpuLightAnalysis is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanContext::Impl::GpuLightAnalysisViewSnapshot VulkanContext::Impl::GetLatestGpuLightAnalysis(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestGpuLightAnalysisByView[index];
}

// VulkanContext::Impl::RecordPendingClusterValidationSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingClusterValidationSnapshot(uint32_t frameIndex, const ClusterValidationSnapshot& snapshot) {
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return;
	}
	pendingClusterValidationByFrame[frameIndex] = snapshot;
}

// VulkanContext::Impl::ReadCompletedClusterValidationSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ClusterValidationSnapshot VulkanContext::Impl::ReadCompletedClusterValidationSnapshot(uint32_t frameIndex) const {
	if (!clusterValidationCaptureEnabled) {
		return {};
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return {};
	}

	ClusterValidationSnapshot snapshot = pendingClusterValidationByFrame[frameIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!snapshot.valid || !snapshot.buildMetadata.clusteredLightingAvailable) {
		return snapshot;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex >= clusterGridSSBOs.size() || frameIndex >= lightIndexSSBOs.size()) {
		snapshot.valid = false;
		return snapshot;
	}

	const VulkanBuffer& clusterBuffer = clusterGridSSBOs[frameIndex];
	const VulkanBuffer& lightIndexBuffer = lightIndexSSBOs[frameIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterBuffer.buffer == VK_NULL_HANDLE || lightIndexBuffer.buffer == VK_NULL_HANDLE) {
		snapshot.valid = false;
		return snapshot;
	}

	const std::uint32_t clusterCount = snapshot.buildMetadata.clusterCount;
	snapshot.ranges.clear();
	snapshot.ranges.reserve(clusterCount);

	void* mappedClusters = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, clusterBuffer.allocation, &mappedClusters) != VK_SUCCESS) {
		snapshot.valid = false;
		return snapshot;
	}

	const VkDeviceSize clusterReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.clusterDataOffset) * sizeof(ClusterData);
	const VkDeviceSize clusterReadSize = static_cast<VkDeviceSize>(sizeof(ClusterData) * clusterCount);
	vmaInvalidateAllocation(allocator, clusterBuffer.allocation, clusterReadOffset, clusterReadSize);

	const ClusterData* clusterGridData = static_cast<const ClusterData*>(mappedClusters) + snapshot.buildMetadata.clusterDataOffset;
	std::uint64_t rangeHash = kClusterRangeHashOffset;
	std::uint32_t usedIndexCount = 0u;
	std::uint32_t actualAssignedLightCount = 0u;
	std::uint32_t emptyClusterCount = 0u;
	std::uint32_t maxClusterLightCount = 0u;
	std::uint32_t outOfBoundsRangeCount = 0u;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t clusterIndex = 0u; clusterIndex < clusterCount; ++clusterIndex) {
		const ClusterData& cluster = clusterGridData[clusterIndex];
		const std::uint64_t rangeEnd = static_cast<std::uint64_t>(cluster.offset) + static_cast<std::uint64_t>(cluster.count);
		const bool outOfBounds = rangeEnd > snapshot.buildMetadata.lightIndexCapacity
			|| rangeEnd > static_cast<std::uint64_t>(MAX_LIGHT_INDEX_ENTRIES);

		ClusterLightRangeSnapshot rangeSnapshot{};
		rangeSnapshot.clusterIndex = clusterIndex;
		rangeSnapshot.offset = cluster.offset;
		rangeSnapshot.count = cluster.count;
		rangeSnapshot.outOfBounds = outOfBounds;
		snapshot.ranges.push_back(rangeSnapshot);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cluster.count == 0u) {
			emptyClusterCount += 1u;
		}
		maxClusterLightCount = std::max(maxClusterLightCount, cluster.count);
		actualAssignedLightCount += cluster.count;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (rangeEnd > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
			usedIndexCount = std::numeric_limits<std::uint32_t>::max();
		}
		else {
			usedIndexCount = std::max(usedIndexCount, static_cast<std::uint32_t>(rangeEnd));
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outOfBounds) {
			outOfBoundsRangeCount += 1u;
		}

		HashClusterRangeValue(rangeHash, clusterIndex);
		HashClusterRangeValue(rangeHash, cluster.offset);
		HashClusterRangeValue(rangeHash, cluster.count);
		HashClusterRangeValue(rangeHash, outOfBounds ? 1u : 0u);
	}

	vmaUnmapMemory(allocator, clusterBuffer.allocation);

	std::uint32_t invalidLightIndexCount = 0u;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (usedIndexCount > 0u) {
		void* mappedLightIndices = nullptr;
		if (vmaMapMemory(allocator, lightIndexBuffer.allocation, &mappedLightIndices) == VK_SUCCESS) {
			const VkDeviceSize lightIndexReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.lightIndexOffset) * sizeof(std::uint32_t);
			const VkDeviceSize lightIndexReadSize = static_cast<VkDeviceSize>(sizeof(std::uint32_t) * usedIndexCount);
			vmaInvalidateAllocation(allocator, lightIndexBuffer.allocation, lightIndexReadOffset, lightIndexReadSize);

			const std::uint32_t* lightIndexData = static_cast<const std::uint32_t*>(mappedLightIndices) + snapshot.buildMetadata.lightIndexOffset;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::uint32_t entryIndex = 0u; entryIndex < usedIndexCount; ++entryIndex) {
				const std::uint32_t lightIndex = lightIndexData[entryIndex];
				if (lightIndex == 0xFFFFFFFFu || lightIndex >= snapshot.buildMetadata.activeLightCount) {
					invalidLightIndexCount += 1u;
				}
			}

			vmaUnmapMemory(allocator, lightIndexBuffer.allocation);
		}
	}

	std::uint32_t determinismMismatchCount = 0u;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (previousClusterValidationSnapshotValid
		&& previousClusterValidationSnapshot.viewType == snapshot.viewType
		&& previousClusterValidationSnapshot.ranges.size() == snapshot.ranges.size()
		&& previousClusterValidationSnapshot.buildMetadata.activeLightCount == snapshot.buildMetadata.activeLightCount) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t rangeIndex = 0; rangeIndex < snapshot.ranges.size(); ++rangeIndex) {
			const ClusterLightRangeSnapshot& currentRange = snapshot.ranges[rangeIndex];
			const ClusterLightRangeSnapshot& previousRange = previousClusterValidationSnapshot.ranges[rangeIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentRange.offset != previousRange.offset
				|| currentRange.count != previousRange.count
				|| currentRange.outOfBounds != previousRange.outOfBounds) {
				determinismMismatchCount += 1u;
			}
		}
	}

	snapshot.buildMetadata.usedIndexCount = usedIndexCount;
	snapshot.buildMetadata.actualAssignedLightCount = actualAssignedLightCount;
	snapshot.buildMetadata.emptyClusterCount = emptyClusterCount;
	snapshot.buildMetadata.maxClusterLightCount = maxClusterLightCount;
	snapshot.buildMetadata.outOfBoundsRangeCount = outOfBoundsRangeCount;
	snapshot.buildMetadata.invalidLightIndexCount = invalidLightIndexCount;
	snapshot.buildMetadata.overflowClusterCount = outOfBoundsRangeCount;
	snapshot.buildMetadata.determinismMismatchCount = determinismMismatchCount;
	snapshot.buildMetadata.rangeHash = rangeHash;
	snapshot.buildMetadata.overflowDetected = (outOfBoundsRangeCount > 0u) || (invalidLightIndexCount > 0u);
	return snapshot;
}

// VulkanContext::Impl::WriteClusterValidationCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::WriteClusterValidationCapture(const ClusterValidationSnapshot& snapshot, const FrameBudgetDiagnosticsSnapshot& diagnostics) const {
	if (!clusterValidationCaptureEnabled || !snapshot.valid) {
		return;
	}

	try {
		namespace fs = std::filesystem;
		const fs::path outputPath = clusterValidationCapturePath;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outputPath.has_parent_path()) {
			fs::create_directories(outputPath.parent_path());
		}

		const double emptyClusterPercent = snapshot.buildMetadata.clusterCount > 0u
			? (static_cast<double>(snapshot.buildMetadata.emptyClusterCount) * 100.0 / static_cast<double>(snapshot.buildMetadata.clusterCount))
			: 0.0;
		const double logicalIndexOverheadPercent = snapshot.buildMetadata.actualAssignedLightCount > 0u
			? (static_cast<double>(snapshot.buildMetadata.usedIndexCount) * 100.0 / static_cast<double>(snapshot.buildMetadata.actualAssignedLightCount))
			: 0.0;

		nlohmann::json capture;
		capture["view_type"] = RenderViewTypeToString(snapshot.viewType);
		capture["valid"] = snapshot.valid;
		capture["clustered_lighting_available"] = snapshot.buildMetadata.clusteredLightingAvailable;
		capture["overflow_detected"] = snapshot.buildMetadata.overflowDetected;
		capture["active_preset_name"] = QualityPresetToString(diagnostics.activePreset);
		capture["reduction_category"] = LightingReductionCategoryToString(diagnostics.reductionCategory);
		capture["gpu_assist_active"] = diagnostics.gpuAssistActive;
		capture["custom_override_active"] = diagnostics.customOverrideActive;
		capture["uploaded_light_budget"] = diagnostics.uploadedLightBudget;
		capture["retained_light_budget"] = diagnostics.retainedLightBudget;
		capture["shadow_light_budget"] = diagnostics.shadowLightBudget;
		capture["special_light_budget"] = diagnostics.specialLightBudget;
		capture["candidate_light_count"] = diagnostics.candidateLightCount;
		capture["uploaded_light_count"] = diagnostics.uploadedLightCount;
		capture["retained_light_count"] = diagnostics.retainedLightCount;
		capture["special_light_count"] = diagnostics.specialLightCount;
		capture["shadow_eligible_light_count"] = diagnostics.shadowEligibleLightCount;
		capture["shadowed_light_count"] = diagnostics.shadowedLightCount;
		capture["gpu_visible_candidate_count"] = diagnostics.gpuVisibleCandidateCount;
		capture["retained_directional_light_count"] = diagnostics.retainedDirectionalLightCount;
		capture["retained_authored_priority_light_count"] = diagnostics.retainedAuthoredPriorityLightCount;
		capture["budget_reduction_occurred"] = diagnostics.budgetReductionOccurred;
		capture["uploaded_reduction_occurred"] = diagnostics.uploadedReductionOccurred;
		capture["special_reduction_occurred"] = diagnostics.specialReductionOccurred;
		capture["shadow_reduction_occurred"] = diagnostics.shadowReductionOccurred;
		capture["retained_light_churn_count"] = diagnostics.retainedLightChurnCount;
		capture["retained_light_replacement_count"] = diagnostics.retainedLightReplacementCount;
		capture["stability_bias_applied_count"] = diagnostics.stabilityBiasAppliedCount;
		capture["shadow_reuse_count"] = diagnostics.shadowReuseCount;
		capture["shadow_replacement_count"] = diagnostics.shadowReplacementCount;
		capture["configured_shadow_capacity"] = diagnostics.configuredShadowCapacity;
		capture["active_shadow_count"] = diagnostics.activeShadowCount;
		capture["allocated_shadow_region_count"] = diagnostics.allocatedRegionCount;
		capture["allocated_shadow_2d_region_count"] = diagnostics.allocated2DRegionCount;
		capture["allocated_shadow_point_region_count"] = diagnostics.allocatedPointRegionCount;
		capture["allocated_shadow_directional_region_count"] = diagnostics.allocatedDirectionalRegionCount;
		capture["allocated_shadow_spot_region_count"] = diagnostics.allocatedSpotRegionCount;
		capture["allocated_shadow_area_region_count"] = diagnostics.allocatedAreaRegionCount;
		capture["estimated_shadow_memory_bytes"] = diagnostics.estimatedShadowMemoryBytes;
		capture["shadow_cache_reuse_count"] = diagnostics.cacheReuseCount;
		capture["shadow_cache_refresh_count"] = diagnostics.cacheRefreshCount;
		capture["shadow_cache_invalidation_count"] = diagnostics.cacheInvalidationCount;
		capture["degraded_shadow_count"] = diagnostics.degradedShadowCount;
		capture["shadow_allocation_failure_count"] = diagnostics.allocationFailureCount;
		capture["camera_visible_shadow_caster_count"] = diagnostics.cameraVisibleShadowCasterCount;
		capture["light_visible_shadow_caster_count"] = diagnostics.lightVisibleShadowCasterCount;
		capture["shadow_draw_submission_count"] = diagnostics.shadowDrawSubmissionCount;
		capture["shadow_pass_recorded"] = diagnostics.shadowPassRecorded;
		capture["shadow_assignments"] = nlohmann::json::array();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ShadowResourceAssignmentDiagnostic& assignment : diagnostics.shadowAssignments) {
			nlohmann::json assignmentJson;
			assignmentJson["light_id"] = TerraEngine::ToHexString(assignment.lightId);
			assignmentJson["resource_kind"] = ShadowResourceKindToString(assignment.resourceKind);
			assignmentJson["resource_slot"] = assignment.resourceSlot;
			assignmentJson["storage_region_id"] = assignment.storageRegionId;
			assignmentJson["atlas_id"] = assignment.atlasId;
			assignmentJson["region_x"] = assignment.regionX;
			assignmentJson["region_y"] = assignment.regionY;
			assignmentJson["region_width"] = assignment.regionWidth;
			assignmentJson["region_height"] = assignment.regionHeight;
			assignmentJson["estimated_bytes"] = assignment.estimatedBytes;
			assignmentJson["frame_serial"] = assignment.frameSerial;
			assignmentJson["frames_since_last_seen"] = assignment.framesSinceLastSeen;
			assignmentJson["retention_score"] = assignment.retentionScore;
			assignmentJson["quality_tier"] = ShadowQualityTierToString(assignment.qualityTier);
			assignmentJson["degradation_reason"] = ShadowDegradationReasonToString(assignment.degradationReason);
			assignmentJson["refresh_reason"] = ShadowRefreshReasonToString(assignment.refreshReason);
			assignmentJson["assigned"] = assignment.assigned;
			assignmentJson["reused_previous_owner"] = assignment.reusedPreviousOwner;
			assignmentJson["replaced_previous_owner"] = assignment.replacedPreviousOwner;
			assignmentJson["shadowed_previous_frame"] = assignment.shadowedPreviousFrame;
			assignmentJson["reused_cached_result"] = assignment.reusedCachedResult;
			assignmentJson["refreshed_this_frame"] = assignment.refreshedThisFrame;
			assignmentJson["invalidated_this_frame"] = assignment.invalidatedThisFrame;
			capture["shadow_assignments"].push_back(std::move(assignmentJson));
		}
		capture["cluster_count"] = snapshot.buildMetadata.clusterCount;
		capture["active_light_count"] = snapshot.buildMetadata.activeLightCount;
		capture["light_index_capacity"] = snapshot.buildMetadata.lightIndexCapacity;
		capture["used_index_count"] = snapshot.buildMetadata.usedIndexCount;
		capture["actual_assigned_light_count"] = snapshot.buildMetadata.actualAssignedLightCount;
		capture["logical_index_overhead_percent"] = logicalIndexOverheadPercent;
		capture["empty_cluster_count"] = snapshot.buildMetadata.emptyClusterCount;
		capture["empty_cluster_percent"] = emptyClusterPercent;
		capture["max_cluster_light_count"] = snapshot.buildMetadata.maxClusterLightCount;
		capture["retained_eligible_light_count"] = snapshot.buildMetadata.maxClusterLightCount;
		capture["out_of_bounds_range_count"] = snapshot.buildMetadata.outOfBoundsRangeCount;
		capture["invalid_light_index_count"] = snapshot.buildMetadata.invalidLightIndexCount;
		capture["overflow_cluster_count"] = snapshot.buildMetadata.overflowClusterCount;
		capture["determinism_mismatch_count"] = snapshot.buildMetadata.determinismMismatchCount;
		capture["range_hash"] = snapshot.buildMetadata.rangeHash;
		capture["range_hash_hex"] = BuildHashHex(snapshot.buildMetadata.rangeHash);
		capture["ranges"] = nlohmann::json::array();

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ClusterLightRangeSnapshot& range : snapshot.ranges) {
			nlohmann::json rangeJson;
			rangeJson["cluster_index"] = range.clusterIndex;
			rangeJson["offset"] = range.offset;
			rangeJson["count"] = range.count;
			rangeJson["out_of_bounds"] = range.outOfBounds;
			capture["ranges"].push_back(std::move(rangeJson));
		}

		std::ofstream outputFile(outputPath);
		outputFile << capture.dump(2);
	}
	catch (const std::exception& exception) {
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to write cluster validation capture: " << exception.what());
	}
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

// VulkanContext::Impl::UpdateGlobalHiZ is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalHiZ(uint32_t frameIndex) {
	if (frameIndex >= globalDescriptorSets.size()) return;
	WaitForDescriptorMutationIdle();

	std::array<VkDescriptorImageInfo, kHiZViewCount> imageInfos{};
	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < kHiZViewCount; ++hiZViewIndex) {
		const VulkanHiZ& hiZ = HiZForFrame(hiZViewIndex, frameIndex);
		if (hiZ.fullView == VK_NULL_HANDLE || hiZ.sampler == VK_NULL_HANDLE) return;

		// Binding 10 is an array so editor Scene/Game command buffers can be
		// recorded once and submitted together without racing a descriptor
		// mutation between views. The task shader indexes this array from the
		// same stats slot that already selects per-view culling data.
		imageInfos[hiZViewIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfos[hiZViewIndex].imageView = hiZ.fullView;
		imageInfos[hiZViewIndex].sampler = VK_NULL_HANDLE;
	}

	VkDescriptorImageInfo samplerInfo{};
	samplerInfo.sampler = HiZForFrame(kHiZGameViewIndex, frameIndex).sampler;

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
	if (!initialized || device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE) {
		return;
	}

	// Global bindless sets and mesh-local descriptor sets are persistent across
	// frame slots. Project open/reload can update or free them outside the
	// normal BeginFrame fence cadence, so wait for submitted graphics work
	// before mutating descriptors that already-recorded command buffers may
	// reference.
	vkQueueWaitIdle(graphicsQueue);
}
