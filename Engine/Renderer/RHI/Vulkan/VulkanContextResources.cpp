#include "VulkanContextImpl.h"

#include <algorithm>

void VulkanContext::Impl::CreateDefaultTexture() {
	uint32_t pixel = 0xFFFFFFFF;
	VkDeviceSize imageSize = 4;
	
	VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	
	void* data;
	vmaMapMemory(allocator, stagingBuffer.allocation, &data);
	memcpy(data, &pixel, static_cast<size_t>(imageSize));
	vmaUnmapMemory(allocator, stagingBuffer.allocation);

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = 1;
	imageInfo.extent.height = 1;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultTexture.image, &defaultTexture.allocation, nullptr);

	TransitionImageLayout(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	CopyBufferToImage(stagingBuffer.buffer, defaultTexture.image, 1, 1);
	TransitionImageLayout(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	defaultTexture.view = CreateImageView(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	
	if (vkCreateSampler(device, &samplerInfo, nullptr, &defaultTexture.sampler) != VK_SUCCESS) {
		throw std::runtime_error("failed to create texture sampler!");
	}

	// Register default texture at index 0 of bindless array
	uint32_t idx = AllocateTextureIndex(); // Should be 0
	UpdateGlobalTexture(idx, defaultTexture);
}

VulkanBuffer VulkanContext::Impl::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage) {
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = memoryUsage;

	VulkanBuffer buffer;
	buffer.size = size;
	if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, nullptr) != VK_SUCCESS) {
		throw std::runtime_error("failed to create buffer!");
	}

	return buffer;
}

void VulkanContext::Impl::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	EndSingleTimeCommands(commandBuffer);
}

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

void VulkanContext::Impl::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT) {
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
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

	EndSingleTimeCommands(commandBuffer);
}

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

VkImageView VulkanContext::Impl::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VkImageView imageView;
	if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
		throw std::runtime_error("failed to create texture image view!");
	}

	return imageView;
}

VkShaderStageFlags VulkanContext::Impl::StageVisibilityToVulkan(StageVisibilityMask visibility) {
	VkShaderStageFlags flags = 0;
	for (std::uint32_t bit = 0; bit < 32; ++bit) {
		if ((visibility & (1u << bit)) == 0) continue;
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

void VulkanContext::Impl::DestroyPipelines() {
	// Mesh descriptor sets are allocated from per-pipeline set layouts, so they
	// must be freed before the corresponding layouts are destroyed.
	std::vector<VkDescriptorSet> meshDescriptorSets;
	for (auto& [meshId, mesh] : meshCache) {
		for (const auto& [layout, descriptorSet] : mesh.descriptorSetsByLayout) {
			if (descriptorSet != VK_NULL_HANDLE) {
				meshDescriptorSets.push_back(descriptorSet);
			}
		}
		mesh.descriptorSetsByLayout.clear();
	}
	if (!meshDescriptorSets.empty() && device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(meshDescriptorSets.size()), meshDescriptorSets.data());
	}

	for (auto& outerEntry : pipelineCache) {
		auto& map = outerEntry.second;
		for (auto& innerEntry : map) {
			VulkanPipeline& pipe = innerEntry.second;
			if (device) {
				vkDestroyPipeline(device, pipe.pipeline, nullptr);
				vkDestroyPipelineLayout(device, pipe.layout, nullptr);
				// Start from 1 to skip the global descriptor set layout (index 0) which is shared and destroyed in Cleanup
				for (size_t i = 1; i < pipe.setLayouts.size(); ++i) {
					vkDestroyDescriptorSetLayout(device, pipe.setLayouts[i], nullptr);
				}
			}
		}
	}
	pipelineCache.clear();
}

void VulkanContext::Impl::DestroyWorkerPools() {
	if (!device) return;
	for (size_t frameIndex = 0; frameIndex < workerCommandPools.size(); ++frameIndex) {
		auto& framePools = workerCommandPools[frameIndex];
		std::vector<std::vector<VkCommandBuffer>> emptyBuffers;
		auto& frameBuffers = (frameIndex < workerCommandBuffers.size()) ? workerCommandBuffers[frameIndex] : emptyBuffers;
		for (size_t poolIndex = 0; poolIndex < framePools.size(); ++poolIndex) {
			if (!framePools[poolIndex]) continue;
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

void VulkanContext::Impl::EnsureWorkerPools(uint32_t frameIndex, size_t workerCount) {
	if (workerCommandPools.size() <= frameIndex) {
		workerCommandPools.resize(frameIndex + 1);
	}
	if (workerCommandBuffers.size() <= frameIndex) {
		workerCommandBuffers.resize(frameIndex + 1);
	}

	auto& framePools = workerCommandPools[frameIndex];
	auto& frameBuffers = workerCommandBuffers[frameIndex];
	QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);

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

VkCommandBuffer VulkanContext::Impl::AcquireWorkerCommandBuffer(uint32_t frameIndex, size_t workerIndex) {
	auto& framePools = workerCommandPools[frameIndex];
	auto& frameBuffers = workerCommandBuffers[frameIndex];
	
	// Ensure tracking structures are sized correctly
	if (workerBufferUsage.size() <= frameIndex) workerBufferUsage.resize(frameIndex + 1);
	if (workerBufferUsage[frameIndex].size() <= workerIndex) workerBufferUsage[frameIndex].resize(workerIndex + 1, 0);

	if (frameBuffers.size() <= workerIndex) frameBuffers.resize(workerIndex + 1);

	size_t& usageIndex = workerBufferUsage[frameIndex][workerIndex];
	std::vector<VkCommandBuffer>& buffers = frameBuffers[workerIndex];

	// reuse existing buffer if available at this usage index
	if (usageIndex < buffers.size()) {
		return buffers[usageIndex++];
	}

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = framePools[workerIndex];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer secondary = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocInfo, &secondary) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	buffers.push_back(secondary);
	usageIndex++;
	return secondary;
}

VkFormat VulkanContext::Impl::MapTextureFormat(uint8_t format, uint8_t channelType, uint8_t semantic) {
	if (format == static_cast<uint8_t>(TETFFormat::RGBA8)) {
		if (semantic == static_cast<uint8_t>(TextureFileSemantic::Color)) {
			return VK_FORMAT_R8G8B8A8_SRGB;
		}
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
	return VK_FORMAT_R8G8B8A8_UNORM;
}

void VulkanContext::Impl::EnsureTexture(TerraEngine::UUID textureId) {
	if (textureCache.contains(textureId)) {
		if (!textureIndexMap.contains(textureId)) {
			uint32_t index = AllocateTextureIndex();
			textureIndexMap[textureId] = index;
			UpdateGlobalTexture(index, textureCache[textureId]);
		}
		return;
	}

	Texture* cpuTexture = ResourceManager::Instance().GetTexture(textureId);
	if (!cpuTexture) return;
	
	if (cpuTexture->mips.empty()) return;

	VkDeviceSize imageSize = cpuTexture->mips[0].data.size();
	
	VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	
	void* data;
	vmaMapMemory(allocator, stagingBuffer.allocation, &data);
	memcpy(data, cpuTexture->mips[0].data.data(), imageSize);
	vmaUnmapMemory(allocator, stagingBuffer.allocation);

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = cpuTexture->descriptor.width;
	imageInfo.extent.height = cpuTexture->descriptor.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1; 
	imageInfo.arrayLayers = 1;
	imageInfo.format = MapTextureFormat(cpuTexture->descriptor.format, cpuTexture->descriptor.channelType, cpuTexture->descriptor.semantic);
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VulkanImage vImage;
	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	
	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &vImage.image, &vImage.allocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create image for texture");
		return;
	}

	TransitionImageLayout(vImage.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	CopyBufferToImage(stagingBuffer.buffer, vImage.image, cpuTexture->descriptor.width, cpuTexture->descriptor.height);
	TransitionImageLayout(vImage.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	vImage.view = CreateImageView(vImage.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.maxAnisotropy = 1.0f;
	
	vkCreateSampler(device, &samplerInfo, nullptr, &vImage.sampler);

	textureCache[textureId] = vImage;

	// Assign bindless index
	uint32_t index = AllocateTextureIndex();
	textureIndexMap[textureId] = index;
	UpdateGlobalTexture(index, vImage);
}

std::vector<VkDescriptorSet> VulkanContext::Impl::EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId) {
	// No longer returns descriptor sets, but acts as an upload mechanism for material data.
	// The return value is kept for now to minimize API breakage, but it will be empty besides the global set.
	if (materialIndexMap.contains(materialId)) {
		return { globalDescriptorSet };
	}

	Material* material = ResourceManager::Instance().GetMaterial(materialId);
	if (!material) return { globalDescriptorSet };
	
	MaterialData data{};
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
	data.Flags = material->header.flags;
	data.UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); // Default: Scale 1, Offset 0

	// Initialize all indices to 0 (default white texture)
	data.BaseColorTextureIndex = 0;
	data.NormalTextureIndex = 0;
	data.MaskTextureIndex = 0;
	data.OcclusionTextureIndex = 0;
	data.EmissiveTextureIndex = 0;

	for (uint32_t i = 0; i < material->header.textureCount; ++i) {
		const auto& binding = material->textures[i];
		if (binding.textureUuid == TerraEngine::EMPTY_UUID) continue;

		EnsureTexture(binding.textureUuid);
		uint32_t gpuIndex = 0;
		if (textureIndexMap.contains(binding.textureUuid)) {
			gpuIndex = textureIndexMap[binding.textureUuid];
		}

		switch (binding.semantic) {
			case TextureSemantic::BaseColor:         
				data.BaseColorTextureIndex = gpuIndex; 
				// Use BaseColor's UV transform as the material-wide transform
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
	VulkanBuffer stagingBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	
	void* mappedData;
	vmaMapMemory(allocator, stagingBuffer.allocation, &mappedData);
	memcpy(mappedData, &data, bufferSize);
	vmaUnmapMemory(allocator, stagingBuffer.allocation);
	
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0;
	copyRegion.dstOffset = materialIndex * sizeof(MaterialData);
	copyRegion.size = bufferSize;
	vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBO.buffer, 1, &copyRegion);
	EndSingleTimeCommands(commandBuffer);
	
	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	return { globalDescriptorSet };
}

void VulkanContext::Impl::InvalidateMaterial(TerraEngine::UUID materialId) {
	auto it = materialIndexMap.find(materialId);
	if (it != materialIndexMap.end()) {
		freeMaterialIndices.push_back(it->second);
		materialIndexMap.erase(it);
	}
}

void VulkanContext::Impl::InvalidateMesh(TerraEngine::UUID meshId) {
	auto it = meshCache.find(meshId);
	if (it != meshCache.end()) {
		if (!it->second.descriptorSetsByLayout.empty() && descriptorPool != VK_NULL_HANDLE) {
			std::vector<VkDescriptorSet> descriptorSets;
			descriptorSets.reserve(it->second.descriptorSetsByLayout.size());
			for (const auto& [layout, set] : it->second.descriptorSetsByLayout) {
				if (set != VK_NULL_HANDLE) {
					descriptorSets.push_back(set);
				}
			}
			if (!descriptorSets.empty()) {
				vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
			}
			it->second.descriptorSetsByLayout.clear();
		}
		vmaDestroyBuffer(allocator, it->second.meshShaderVertexBuffer.buffer, it->second.meshShaderVertexBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletVertexRemapBuffer.buffer, it->second.meshletVertexRemapBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshSkinningBuffer.buffer, it->second.meshSkinningBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshDecodeInfoBuffer.buffer, it->second.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.indexBuffer.buffer, it->second.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletBuffer.buffer, it->second.meshletBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletBoundsBuffer.buffer, it->second.meshletBoundsBuffer.allocation);
		meshCache.erase(it);
	}
}

void VulkanContext::Impl::InvalidateTexture(TerraEngine::UUID textureId) {
	auto it = textureCache.find(textureId);
	if (it != textureCache.end()) {
		vkDestroySampler(device, it->second.sampler, nullptr);
		vkDestroyImageView(device, it->second.view, nullptr);
		vmaDestroyImage(allocator, it->second.image, it->second.allocation);
		textureCache.erase(it);
	}

	auto idxIt = textureIndexMap.find(textureId);
	if (idxIt != textureIndexMap.end()) {
		freeTextureIndices.push_back(idxIt->second);
		textureIndexMap.erase(idxIt);
	}
}

void VulkanContext::Impl::EnsureMesh(TerraEngine::UUID meshId) {
	if (meshCache.contains(meshId)) return;
	
	Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
	if (!cpuMesh) return;

	const VkDeviceSize packedVertexSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexData.size());
	const VkDeviceSize vertexRemapSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexRemapData.size());
	const VkDeviceSize skinningDataSize = static_cast<VkDeviceSize>(cpuMesh->meshletSkinningData.size());
	const VkDeviceSize primitiveIndexSize = static_cast<VkDeviceSize>(cpuMesh->meshletPrimitiveIndexData.size());
	const VkDeviceSize meshletTableSize = static_cast<VkDeviceSize>(cpuMesh->meshletRecords.size() * sizeof(MeshletRecord));
	const VkDeviceSize meshletBoundsSize = static_cast<VkDeviceSize>(cpuMesh->meshletBounds.size() * sizeof(MeshletBounds));
	const bool hasBoundsPayload = meshletBoundsSize > 0;
	const VkDeviceSize boundsUploadSize = hasBoundsPayload ? meshletBoundsSize : static_cast<VkDeviceSize>(sizeof(MeshletBounds));
	if (packedVertexSize == 0
		|| vertexRemapSize == 0
		|| primitiveIndexSize == 0
		|| meshletTableSize == 0
		|| cpuMesh->header.meshletCount == 0)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload is missing packed vertex/remap/primitive/table data for UUID " << ToHexString(meshId));
		return;
	}

	const uint32_t packedStride = cpuMesh->header.layout.strideBytes;
	const uint32_t meshletVertexCount = cpuMesh->header.meshletVertexCount;
	if (packedStride != kMeshletPackedVertexStrideBytes || meshletVertexCount == 0) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload has invalid packed vertex stride/count for UUID " << ToHexString(meshId));
		return;
	}

	const VkDeviceSize expectedVertexBytes = static_cast<VkDeviceSize>(packedStride) * static_cast<VkDeviceSize>(meshletVertexCount);
	if (expectedVertexBytes != packedVertexSize) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload packed vertex range is truncated for UUID " << ToHexString(meshId));
		return;
	}

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
	void* data;
	vmaMapMemory(allocator, stagingPackedVertex.allocation, &data);
	memcpy(data, cpuMesh->meshletVertexData.data(), static_cast<size_t>(packedVertexSize));
	vmaUnmapMemory(allocator, stagingPackedVertex.allocation);

	VulkanBuffer stagingVertexRemap = CreateBuffer(vertexRemapSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	vmaMapMemory(allocator, stagingVertexRemap.allocation, &data);
	// Zero-copy: upload raw on-disk bytes (u16 or u32) without CPU expansion.
	memcpy(data, cpuMesh->meshletVertexRemapData.data(), static_cast<size_t>(vertexRemapSize));
	vmaUnmapMemory(allocator, stagingVertexRemap.allocation);

	VulkanBuffer stagingSkinning = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0 };
	if (skinningDataSize > 0)
	{
		stagingSkinning = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		vmaMapMemory(allocator, stagingSkinning.allocation, &data);
		memcpy(data, cpuMesh->meshletSkinningData.data(), static_cast<size_t>(skinningDataSize));
		vmaUnmapMemory(allocator, stagingSkinning.allocation);
	}

	VulkanBuffer stagingIndex = CreateBuffer(primitiveIndexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	vmaMapMemory(allocator, stagingIndex.allocation, &data);
	memcpy(data, cpuMesh->meshletPrimitiveIndexData.data(), static_cast<size_t>(primitiveIndexSize));
	vmaUnmapMemory(allocator, stagingIndex.allocation);

	VulkanBuffer stagingMeshletTable = CreateBuffer(meshletTableSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	vmaMapMemory(allocator, stagingMeshletTable.allocation, &data);
	memcpy(data, cpuMesh->meshletRecords.data(), static_cast<size_t>(meshletTableSize));
	vmaUnmapMemory(allocator, stagingMeshletTable.allocation);

	VulkanBuffer stagingMeshletBounds = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	vmaMapMemory(allocator, stagingMeshletBounds.allocation, &data);
	if (hasBoundsPayload)
	{
		memcpy(data, cpuMesh->meshletBounds.data(), static_cast<size_t>(meshletBoundsSize));
	}
	else
	{
		MeshletBounds fallbackBounds{};
		fallbackBounds.coneCutoff = 2.0f;
		memcpy(data, &fallbackBounds, sizeof(fallbackBounds));
	}
	vmaUnmapMemory(allocator, stagingMeshletBounds.allocation);

	VulkanBuffer stagingDecodeInfo = CreateBuffer(sizeof(MeshDecodeInfo), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	vmaMapMemory(allocator, stagingDecodeInfo.allocation, &data);
	memcpy(data, &decodeInfo, sizeof(MeshDecodeInfo));
	vmaUnmapMemory(allocator, stagingDecodeInfo.allocation);

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
	vMesh.meshletBoundsBuffer = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.hasMeshletBounds = hasBoundsPayload;
	vMesh.indexCount = cpuMesh->header.meshletPrimitiveIndexCount;
	vMesh.indexType = VK_INDEX_TYPE_UINT16;
	vMesh.meshletCount = cpuMesh->header.meshletCount;

	CopyBuffer(stagingPackedVertex.buffer, vMesh.meshShaderVertexBuffer.buffer, packedVertexSize);
	CopyBuffer(stagingVertexRemap.buffer, vMesh.meshletVertexRemapBuffer.buffer, vertexRemapSize);
	if (skinningDataSize > 0)
	{
		CopyBuffer(stagingSkinning.buffer, vMesh.meshSkinningBuffer.buffer, skinningDataSize);
	}
	CopyBuffer(stagingDecodeInfo.buffer, vMesh.meshDecodeInfoBuffer.buffer, sizeof(MeshDecodeInfo));
	CopyBuffer(stagingIndex.buffer, vMesh.indexBuffer.buffer, primitiveIndexSize);
	CopyBuffer(stagingMeshletTable.buffer, vMesh.meshletBuffer.buffer, meshletTableSize);
	CopyBuffer(stagingMeshletBounds.buffer, vMesh.meshletBoundsBuffer.buffer, boundsUploadSize);

	vmaDestroyBuffer(allocator, stagingPackedVertex.buffer, stagingPackedVertex.allocation);
	vmaDestroyBuffer(allocator, stagingVertexRemap.buffer, stagingVertexRemap.allocation);
	if (skinningDataSize > 0)
	{
		vmaDestroyBuffer(allocator, stagingSkinning.buffer, stagingSkinning.allocation);
	}
	vmaDestroyBuffer(allocator, stagingDecodeInfo.buffer, stagingDecodeInfo.allocation);
	vmaDestroyBuffer(allocator, stagingIndex.buffer, stagingIndex.allocation);
	vmaDestroyBuffer(allocator, stagingMeshletTable.buffer, stagingMeshletTable.allocation);
	vmaDestroyBuffer(allocator, stagingMeshletBounds.buffer, stagingMeshletBounds.allocation);

	meshCache[meshId] = vMesh;
}

VkDescriptorSet VulkanContext::Impl::EnsureMeshDescriptorSet(TerraEngine::UUID meshId, const VulkanPipeline& pipeline) {
	auto meshIt = meshCache.find(meshId);
	if (meshIt == meshCache.end()) {
		return VK_NULL_HANDLE;
	}

	if (pipeline.setLayouts.size() <= 1 || pipeline.setLayouts[1] == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader pipeline is missing descriptor set layout 1 for mesh payload buffers.");
		return VK_NULL_HANDLE;
	}

	if (pipeline.bindingsPerSet.size() <= 1) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader pipeline is missing reflected binding metadata for descriptor set 1.");
		return VK_NULL_HANDLE;
	}

	const auto& meshBindings = pipeline.bindingsPerSet[1];
	auto HasStorageBinding = [&](uint32_t bindingIndex) -> bool {
		for (const DescriptorBinding& binding : meshBindings) {
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
	if (mesh.meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
		|| mesh.indexBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshletBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload buffers are not initialized for mesh shader descriptor binding.");
		return VK_NULL_HANDLE;
	}

	if (HasStorageBinding(5) && mesh.meshletBoundsBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires bounds binding 5 but meshlet bounds payload is unavailable.");
		return VK_NULL_HANDLE;
	}

	const VkDescriptorSetLayout layout = pipeline.setLayouts[1];
	auto cachedSetIt = mesh.descriptorSetsByLayout.find(layout);
	if (cachedSetIt != mesh.descriptorSetsByLayout.end() && cachedSetIt->second != VK_NULL_HANDLE) {
		return cachedSetIt->second;
	}

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
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

	if (HasStorageBinding(5))
	{
		w.dstBinding = 5; w.pBufferInfo = &boundsInfo; writes.push_back(w);
	}

	if (hasSkinning && HasStorageBinding(6))
	{
		w.dstBinding = 6; w.pBufferInfo = &skinningInfo; writes.push_back(w);
	}

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	mesh.descriptorSetsByLayout[layout] = descriptorSet;
	return descriptorSet;
}

bool VulkanContext::Impl::BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants) {
	if (globalDescriptorSetLayout == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Global descriptor set layout is not initialized; cannot build reflected pipeline layouts.");
		return false;
	}

	struct AggregatedBinding {
		DescriptorBinding binding{};
		VkShaderStageFlags stages = 0;
		bool initialized = false;
	};

	std::map<uint32_t, std::map<uint32_t, AggregatedBinding>> setBindings;
	std::map<std::pair<uint32_t, uint32_t>, VkShaderStageFlags> pushConstantMerge;
	supportsDefaultPushConstants = false;

	for (const auto& stage : collection.stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (!shader) {
			TERRAENGINE_LOG(LogLevel::Error, "Shader stage missing for collection; cannot build pipeline layouts");
			return false;
		}

		for (const DescriptorSetLayoutDesc& setDesc : shader->descriptorSets) {
			for (uint32_t idx = 0; idx < setDesc.bindingCount; ++idx) {
				const uint32_t bindingIndex = setDesc.firstBinding + idx;
				if (bindingIndex >= shader->descriptorBindings.size()) continue;
			const DescriptorBinding& srcBinding = shader->descriptorBindings[bindingIndex];
			auto& aggregated = setBindings[setDesc.setIndex][srcBinding.binding];
			if (!aggregated.initialized) {
				aggregated.binding = srcBinding;
				if (aggregated.binding.arrayCount == 0) {
					aggregated.binding.arrayCount = 1; // default to 1 so descriptors get valid writes
				}
				aggregated.initialized = true;
			} else {
				if (aggregated.binding.type != srcBinding.type || aggregated.binding.arrayCount != srcBinding.arrayCount) {
					TERRAENGINE_LOG(LogLevel::Warning, "Descriptor binding mismatch across stages for set " << setDesc.setIndex << ", binding " << srcBinding.binding);
				}
				}
				StageVisibilityMask visibility = srcBinding.visibility;
				if (visibility == 0) {
					visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
				}
				aggregated.stages |= StageVisibilityToVulkan(visibility);
			}
		}

		for (const PushConstantRange& pc : shader->pushConstants) {
			StageVisibilityMask visibility = pc.visibility;
			if (visibility == 0) {
				visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
			}
			const VkShaderStageFlags vkStages = StageVisibilityToVulkan(visibility);
			std::pair<uint32_t, uint32_t> key = {pc.offsetBytes, pc.sizeBytes};
			pushConstantMerge[key] |= vkStages;
			if (pc.offsetBytes == 0
				&& pc.sizeBytes >= sizeof(PushConstants)
				&& (vkStages & VK_SHADER_STAGE_MESH_BIT_EXT)) {
				supportsDefaultPushConstants = true;
			}
		}
	}

	for (const auto& [key, stages] : pushConstantMerge) {
		VkPushConstantRange range{};
		range.offset = key.first;
		range.size = key.second;
		range.stageFlags = stages;
		pushConstants.push_back(range);
	}

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
		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create descriptor set layout from reflection");
			for (size_t i = 1; i < setLayouts.size(); ++i) { // Start from 1 to avoid destroying global layout
				if (setLayouts[i] != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayouts[i], nullptr);
			}
			return false;
		}

		if (setLayouts.size() <= setIndex) {
			setLayouts.resize(setIndex + 1, VK_NULL_HANDLE);
			bindingsPerSet.resize(setIndex + 1);
		}
		setLayouts[setIndex] = layout;
		bindingsPerSet[setIndex] = std::move(storedBindings);
	}

	return true;
}

void VulkanContext::Impl::EnsurePipeline(TerraEngine::UUID shaderCollectionId, VkRenderPass renderPass, uint32_t materialFlags) {
	PipelineKey key = { shaderCollectionId, materialFlags };

	if (renderPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Cannot ensure pipeline: render pass handle is null.");
		return;
	}

	auto passIt = pipelineCache.find(renderPass);
	if (passIt != pipelineCache.end()) {
		auto cachedPipelineIt = passIt->second.find(key);
		if (cachedPipelineIt != passIt->second.end()) {
			VulkanPipeline& cached = cachedPipelineIt->second;
			if (cached.pipeline != VK_NULL_HANDLE && cached.layout != VK_NULL_HANDLE) {
				return;
			}

			// A stale/invalid cache entry can appear when creation failed previously.
			// Remove it now so the next path can attempt a clean rebuild.
			if (device != VK_NULL_HANDLE) {
				if (cached.pipeline != VK_NULL_HANDLE) {
					vkDestroyPipeline(device, cached.pipeline, nullptr);
				}
				if (cached.layout != VK_NULL_HANDLE) {
					vkDestroyPipelineLayout(device, cached.layout, nullptr);
				}
				for (size_t layoutIndex = 1; layoutIndex < cached.setLayouts.size(); ++layoutIndex) {
					if (cached.setLayouts[layoutIndex] != VK_NULL_HANDLE) {
						vkDestroyDescriptorSetLayout(device, cached.setLayouts[layoutIndex], nullptr);
					}
				}
			}
			passIt->second.erase(cachedPipelineIt);
		}
	}
	
	ShaderCollection* collection = ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
	if (!collection) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to build graphics pipeline: shader collection not found.");
		return;
	}

	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
	bool hasMeshStage = false;
	bool hasTaskStage = false;
	for (auto& stage : collection->stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (shader) {
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = shader->bytecode.size();
			createInfo.pCode = reinterpret_cast<const uint32_t*>(shader->bytecode.data());

			VkShaderModule shaderModule;
			if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module");
				continue;
			}
			
			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			if (stage.stage == ShaderStage::Fragment) {
				stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			}
			else if (stage.stage == ShaderStage::Mesh) {
				if (!meshShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build mesh pipeline because mesh shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
				hasMeshStage = true;
			}
			else if (stage.stage == ShaderStage::Task) {
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

	if (shaderStages.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "No shader stages available to build pipeline");
		return;
	}

	if (!hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Graphics pipeline must include a mesh stage in this renderer.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

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
	if (!BuildPipelineLayouts(*collection, setLayouts, bindingsPerSet, pushConstants, supportsDefaultPushConstants)) {
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
	viewport.width = (float)swapchainExtent.width;
	viewport.height = (float)swapchainExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = swapchainExtent;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = (materialFlags & DoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline layout.");
		return;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.layout = pipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

	VkPipeline graphicsPipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline.");
		return;
	}

	for (auto& stage : shaderStages) {
		vkDestroyShaderModule(device, stage.module, nullptr);
	}

	VkShaderStageFlags defaultPushConstantStages = 0;
	for (const VkPushConstantRange& range : pushConstants) {
		defaultPushConstantStages |= range.stageFlags;
	}
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

bool VulkanContext::Impl::CreateGlobalDescriptorResources() {
	std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
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

	VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{};
	bindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	std::array<VkDescriptorBindingFlags, 9> flags = {
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	};
	bindingFlags.bindingCount = static_cast<uint32_t>(flags.size());
	bindingFlags.pBindingFlags = flags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	layoutInfo.pNext = &bindingFlags;

	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &globalDescriptorSetLayout) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create global descriptor set layout!");
		return false;
	}

	std::array<VkDescriptorPoolSize, 4> poolSizes{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[0].descriptorCount = MAX_BINDLESS_TEXTURES;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	poolSizes[1].descriptorCount = 1;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	poolSizes[2].descriptorCount = 1;
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[3].descriptorCount = 6;

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = 1;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &globalDescriptorPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create global descriptor pool!");
		return false;
	}

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = globalDescriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &globalDescriptorSetLayout;

	if (vkAllocateDescriptorSets(device, &allocInfo, &globalDescriptorSet) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate global descriptor set!");
		return false;
	}

	return true;
}

void VulkanContext::Impl::CreateMaterialBufferResources() {
	VkDeviceSize bufferSize = sizeof(MaterialData) * MAX_BINDLESS_MATERIALS;
	globalMaterialSSBO = CreateBuffer(bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	VkDescriptorBufferInfo bufferInfo{};
	bufferInfo.buffer = globalMaterialSSBO.buffer;
	bufferInfo.offset = 0;
	bufferInfo.range = bufferSize;

	VkWriteDescriptorSet descriptorWrite{};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = globalDescriptorSet;
	descriptorWrite.dstBinding = 1;
	descriptorWrite.dstArrayElement = 0;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.pBufferInfo = &bufferInfo;

	vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
}

void VulkanContext::Impl::CreateCullingBufferResources() {
	const VkDeviceSize frameDataBufferSize = static_cast<VkDeviceSize>(sizeof(FrameCullingDataGPU) * kStatsSlotCount);
	const VkDeviceSize countersBufferSize = static_cast<VkDeviceSize>(sizeof(MeshletCullingCountersGPU) * kStatsSlotCount);

	globalFrameCullingSSBO = CreateBuffer(frameDataBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	globalMeshletCountersSSBO = CreateBuffer(countersBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	FrameCullingDataGPU defaultFrameData{};
	for (uint32_t slotIndex = 0; slotIndex < kStatsSlotCount; ++slotIndex) {
		UpdateFrameCullingData(slotIndex, defaultFrameData);
		ResetMeshletCounters(slotIndex);
	}

	VkDescriptorBufferInfo frameCullingInfo{};
	frameCullingInfo.buffer = globalFrameCullingSSBO.buffer;
	frameCullingInfo.offset = 0;
	frameCullingInfo.range = frameDataBufferSize;

	VkDescriptorBufferInfo meshletCountersInfo{};
	meshletCountersInfo.buffer = globalMeshletCountersSSBO.buffer;
	meshletCountersInfo.offset = 0;
	meshletCountersInfo.range = countersBufferSize;

	VkWriteDescriptorSet writes[2]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = globalDescriptorSet;
	writes[0].dstBinding = 2;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].descriptorCount = 1;
	writes[0].pBufferInfo = &frameCullingInfo;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = globalDescriptorSet;
	writes[1].dstBinding = 3;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].descriptorCount = 1;
	writes[1].pBufferInfo = &meshletCountersInfo;

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);

	for (uint32_t viewIndex = 0; viewIndex < kStatsViewCount; ++viewIndex) {
		latestStatsByView[viewIndex] = {};
		latestStatsByView[viewIndex].viewType = (viewIndex == 0u) ? RenderViewType::Game : RenderViewType::Scene;
	}
	for (uint32_t slotIndex = 0; slotIndex < kStatsSlotCount; ++slotIndex) {
		pendingStatsBySlot[slotIndex] = {};
		pendingStatsBySlot[slotIndex].viewType = (slotIndex % kStatsViewCount == 0u) ? RenderViewType::Game : RenderViewType::Scene;
	}
}

void VulkanContext::Impl::CreateLightingBufferResources() {
	const VkDeviceSize lightBufferSize = static_cast<VkDeviceSize>(sizeof(GPULight) * MAX_GLOBAL_LIGHTS);
	const VkDeviceSize clusterBufferSize = static_cast<VkDeviceSize>(sizeof(ClusterData) * MAX_LIGHT_CLUSTERS);
	const VkDeviceSize lightIndexBufferSize = static_cast<VkDeviceSize>(sizeof(uint32_t) * MAX_LIGHT_INDEX_ENTRIES);

	globalLightSSBO = CreateBuffer(lightBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	clusterGridSSBO = CreateBuffer(clusterBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	lightIndexSSBO = CreateBuffer(lightIndexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	VkDescriptorBufferInfo lightInfo{};
	lightInfo.buffer = globalLightSSBO.buffer;
	lightInfo.offset = 0;
	lightInfo.range = lightBufferSize;

	VkDescriptorBufferInfo clusterInfo{};
	clusterInfo.buffer = clusterGridSSBO.buffer;
	clusterInfo.offset = 0;
	clusterInfo.range = clusterBufferSize;

	VkDescriptorBufferInfo lightIndexInfo{};
	lightIndexInfo.buffer = lightIndexSSBO.buffer;
	lightIndexInfo.offset = 0;
	lightIndexInfo.range = lightIndexBufferSize;

	std::array<VkWriteDescriptorSet, 3> writes{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = globalDescriptorSet;
	writes[0].dstBinding = 4;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].descriptorCount = 1;
	writes[0].pBufferInfo = &lightInfo;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = globalDescriptorSet;
	writes[1].dstBinding = 5;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].descriptorCount = 1;
	writes[1].pBufferInfo = &clusterInfo;

	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = globalDescriptorSet;
	writes[2].dstBinding = 6;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].descriptorCount = 1;
	writes[2].pBufferInfo = &lightIndexInfo;

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	UploadLightingData({});

	// Rebuilding this block (for example after swapchain recreation) must tear
	// down stale views/framebuffers before allocating replacement images.
	auto DestroyFramebuffer = [&](VkFramebuffer& framebuffer) {
		if (framebuffer != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
	};
	DestroyFramebuffer(directionalShadowFramebuffer);
	DestroyFramebuffer(spotShadowFramebuffer);
	DestroyFramebuffer(areaShadowFramebuffer);
	for (VkFramebuffer& framebuffer : pointShadowFaceFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}

	for (VkImageView& faceView : pointShadowDepthFaceViews) {
		if (faceView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, faceView, nullptr);
			faceView = VK_NULL_HANDLE;
		}
	}
	for (VkImageView& faceView : pointShadowColorFaceViews) {
		if (faceView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, faceView, nullptr);
			faceView = VK_NULL_HANDLE;
		}
	}

	auto DestroyImage = [&](VulkanImage& image) {
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
			image.sampler = VK_NULL_HANDLE;
		}
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
			image.view = VK_NULL_HANDLE;
		}
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image.image = VK_NULL_HANDLE;
		image.allocation = VK_NULL_HANDLE;
	};

	DestroyImage(directionalShadowDepth);
	DestroyImage(directionalShadowColor);
	DestroyImage(spotShadowDepth);
	DestroyImage(spotShadowColor);
	DestroyImage(areaShadowDepth);
	DestroyImage(areaShadowColor);
	DestroyImage(pointShadowCubeDepth);
	DestroyImage(pointShadowCubeColor);
	DestroyImage(ltcAmplitudeTexture);
	DestroyImage(ltcInverseTexture);

	const VkFormat shadowDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
	const uint32_t shadowWidth = SHADOW_MAP_RESOLUTION;
	const uint32_t shadowHeight = SHADOW_MAP_RESOLUTION;
	const uint32_t pointShadowSize = POINT_SHADOW_MAP_RESOLUTION;

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
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
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
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
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
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
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
		if (vkCreateImageView(device, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			image.view = VK_NULL_HANDLE;
		}
	};

	auto CreateShadowSampler = [&](VulkanImage& image, bool useCompareSampler) {
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

	CreateColorAttachment(shadowWidth, shadowHeight, 1u, 0u, directionalShadowColor, VK_IMAGE_VIEW_TYPE_2D);
	CreateDepthAttachment2D(shadowWidth, shadowHeight, directionalShadowDepth);
	CreateShadowSampler(directionalShadowDepth, false);

	CreateColorAttachment(shadowWidth, shadowHeight, 1u, 0u, spotShadowColor, VK_IMAGE_VIEW_TYPE_2D);
	CreateDepthAttachment2D(shadowWidth, shadowHeight, spotShadowDepth);
	CreateShadowSampler(spotShadowDepth, false);

	CreateColorAttachment(shadowWidth, shadowHeight, 1u, 0u, areaShadowColor, VK_IMAGE_VIEW_TYPE_2D);
	CreateDepthAttachment2D(shadowWidth, shadowHeight, areaShadowDepth);
	CreateShadowSampler(areaShadowDepth, false);

	CreateColorAttachment(pointShadowSize, pointShadowSize, 6u, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, pointShadowCubeColor, VK_IMAGE_VIEW_TYPE_CUBE);
	CreateDepthAttachmentCube(pointShadowSize, pointShadowCubeDepth);
	CreateShadowSampler(pointShadowCubeDepth, false);

	for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
		VkImageViewCreateInfo depthFaceViewInfo{};
		depthFaceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		depthFaceViewInfo.image = pointShadowCubeDepth.image;
		depthFaceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depthFaceViewInfo.format = shadowDepthFormat;
		depthFaceViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthFaceViewInfo.subresourceRange.baseMipLevel = 0;
		depthFaceViewInfo.subresourceRange.levelCount = 1;
		depthFaceViewInfo.subresourceRange.baseArrayLayer = faceIndex;
		depthFaceViewInfo.subresourceRange.layerCount = 1;
		vkCreateImageView(device, &depthFaceViewInfo, nullptr, &pointShadowDepthFaceViews[faceIndex]);

		VkImageViewCreateInfo colorFaceViewInfo{};
		colorFaceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		colorFaceViewInfo.image = pointShadowCubeColor.image;
		colorFaceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		colorFaceViewInfo.format = swapchainImageFormat;
		colorFaceViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		colorFaceViewInfo.subresourceRange.baseMipLevel = 0;
		colorFaceViewInfo.subresourceRange.levelCount = 1;
		colorFaceViewInfo.subresourceRange.baseArrayLayer = faceIndex;
		colorFaceViewInfo.subresourceRange.layerCount = 1;
		vkCreateImageView(device, &colorFaceViewInfo, nullptr, &pointShadowColorFaceViews[faceIndex]);
	}

	auto BindDepthTexture = [&](uint32_t index, const VulkanImage& image) {
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		imageInfo.imageView = image.view;
		imageInfo.sampler = image.sampler;

		VkWriteDescriptorSet descriptorWrite{};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = globalDescriptorSet;
		descriptorWrite.dstBinding = 0;
		descriptorWrite.dstArrayElement = index;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
	};

	// Register 2D shadow maps into the bindless texture heap so each light type
	// can provide its sampled shadow-map index through the GPULight payload.
	directionalShadowTextureIndex = AllocateTextureIndex();
	BindDepthTexture(directionalShadowTextureIndex, directionalShadowDepth);
	spotShadowTextureIndex = AllocateTextureIndex();
	BindDepthTexture(spotShadowTextureIndex, spotShadowDepth);
	areaShadowTextureIndex = AllocateTextureIndex();
	BindDepthTexture(areaShadowTextureIndex, areaShadowDepth);

	// Point shadows use a dedicated cube binding because the global bindless array
	// is typed as Texture2D in the fragment shader.
	VkDescriptorImageInfo pointShadowCubeImageInfo{};
	pointShadowCubeImageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	pointShadowCubeImageInfo.imageView = pointShadowCubeDepth.view;
	pointShadowCubeImageInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo pointShadowCubeSamplerInfo{};
	pointShadowCubeSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	pointShadowCubeSamplerInfo.imageView = VK_NULL_HANDLE;
	pointShadowCubeSamplerInfo.sampler = pointShadowCubeDepth.sampler;

	std::array<VkWriteDescriptorSet, 2> pointShadowCubeWrites{};
	pointShadowCubeWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	pointShadowCubeWrites[0].dstSet = globalDescriptorSet;
	pointShadowCubeWrites[0].dstBinding = 7;
	pointShadowCubeWrites[0].dstArrayElement = 0;
	pointShadowCubeWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pointShadowCubeWrites[0].descriptorCount = 1;
	pointShadowCubeWrites[0].pImageInfo = &pointShadowCubeImageInfo;

	pointShadowCubeWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	pointShadowCubeWrites[1].dstSet = globalDescriptorSet;
	pointShadowCubeWrites[1].dstBinding = 8;
	pointShadowCubeWrites[1].dstArrayElement = 0;
	pointShadowCubeWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	pointShadowCubeWrites[1].descriptorCount = 1;
	pointShadowCubeWrites[1].pImageInfo = &pointShadowCubeSamplerInfo;

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(pointShadowCubeWrites.size()), pointShadowCubeWrites.data(), 0, nullptr);

	// Build lightweight procedural LTC lookup textures. These are deterministic
	// fallback tables until authored LUT assets are imported through content.
	auto CreateLtcLookupTexture = [&](const std::vector<glm::vec4>& texels, VulkanImage& image) {
		if (texels.empty()) {
			return;
		}

		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(sizeof(glm::vec4) * texels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		void* mapped = nullptr;
		vmaMapMemory(allocator, stagingBuffer.allocation, &mapped);
		std::memcpy(mapped, texels.data(), static_cast<size_t>(uploadSize));
		vmaUnmapMemory(allocator, stagingBuffer.allocation);

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
	for (uint32_t y = 0u; y < lutSize; ++y) {
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

	if (ltcAmplitudeTexture.image != VK_NULL_HANDLE) {
		ltcAmplitudeTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(ltcAmplitudeTextureIndex, ltcAmplitudeTexture);
	}
	if (ltcInverseTexture.image != VK_NULL_HANDLE) {
		ltcInverseTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(ltcInverseTextureIndex, ltcInverseTexture);
	}
}

void VulkanContext::Impl::UploadLightingData(const std::vector<GPULight>& lights) {
	if (globalLightSSBO.buffer == VK_NULL_HANDLE || clusterGridSSBO.buffer == VK_NULL_HANDLE || lightIndexSSBO.buffer == VK_NULL_HANDLE) {
		return;
	}

	const uint32_t clampedLightCount = static_cast<uint32_t>(std::min<size_t>(lights.size(), MAX_GLOBAL_LIGHTS));

	void* mappedLights = nullptr;
	if (vmaMapMemory(allocator, globalLightSSBO.allocation, &mappedLights) == VK_SUCCESS) {
		auto* gpuLights = static_cast<GPULight*>(mappedLights);
		std::fill(gpuLights, gpuLights + MAX_GLOBAL_LIGHTS, GPULight{});
		if (clampedLightCount > 0u) {
			std::memcpy(gpuLights, lights.data(), sizeof(GPULight) * clampedLightCount);
		}
		vmaFlushAllocation(allocator, globalLightSSBO.allocation, 0, VK_WHOLE_SIZE);
		vmaUnmapMemory(allocator, globalLightSSBO.allocation);
	}

	void* mappedClusters = nullptr;
	if (vmaMapMemory(allocator, clusterGridSSBO.allocation, &mappedClusters) == VK_SUCCESS) {
		auto* clusters = static_cast<ClusterData*>(mappedClusters);
		const uint32_t visiblePerCluster = std::min(clampedLightCount, MAX_LIGHTS_PER_CLUSTER);
		for (uint32_t clusterIndex = 0; clusterIndex < MAX_LIGHT_CLUSTERS; ++clusterIndex) {
			clusters[clusterIndex].offset = clusterIndex * MAX_LIGHTS_PER_CLUSTER;
			clusters[clusterIndex].count = visiblePerCluster;
		}
		vmaFlushAllocation(allocator, clusterGridSSBO.allocation, 0, VK_WHOLE_SIZE);
		vmaUnmapMemory(allocator, clusterGridSSBO.allocation);
	}

	void* mappedIndices = nullptr;
	if (vmaMapMemory(allocator, lightIndexSSBO.allocation, &mappedIndices) == VK_SUCCESS) {
		auto* indices = static_cast<uint32_t*>(mappedIndices);
		for (uint32_t clusterIndex = 0; clusterIndex < MAX_LIGHT_CLUSTERS; ++clusterIndex) {
			const uint32_t baseOffset = clusterIndex * MAX_LIGHTS_PER_CLUSTER;
			for (uint32_t lane = 0; lane < MAX_LIGHTS_PER_CLUSTER; ++lane) {
				indices[baseOffset + lane] = (lane < clampedLightCount) ? lane : 0xFFFFFFFFu;
			}
		}
		vmaFlushAllocation(allocator, lightIndexSSBO.allocation, 0, VK_WHOLE_SIZE);
		vmaUnmapMemory(allocator, lightIndexSSBO.allocation);
	}
}

void VulkanContext::Impl::ResetMeshletCounters(uint32_t statsSlotIndex) {
	if (globalMeshletCountersSSBO.buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return;
	}

	MeshletCullingCountersGPU counters{};
	void* mapped = nullptr;
	if (vmaMapMemory(allocator, globalMeshletCountersSSBO.allocation, &mapped) != VK_SUCCESS) {
		return;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(MeshletCullingCountersGPU);
	std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &counters, sizeof(MeshletCullingCountersGPU));
	vmaFlushAllocation(allocator, globalMeshletCountersSSBO.allocation, offset, sizeof(MeshletCullingCountersGPU));
	vmaUnmapMemory(allocator, globalMeshletCountersSSBO.allocation);
}

void VulkanContext::Impl::UpdateFrameCullingData(uint32_t statsSlotIndex, const FrameCullingDataGPU& frameData) {
	if (globalFrameCullingSSBO.buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return;
	}

	void* mapped = nullptr;
	if (vmaMapMemory(allocator, globalFrameCullingSSBO.allocation, &mapped) != VK_SUCCESS) {
		return;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(FrameCullingDataGPU);
	std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &frameData, sizeof(FrameCullingDataGPU));
	vmaFlushAllocation(allocator, globalFrameCullingSSBO.allocation, offset, sizeof(FrameCullingDataGPU));
	vmaUnmapMemory(allocator, globalFrameCullingSSBO.allocation);
}

MeshletCullingCountersGPU VulkanContext::Impl::ReadMeshletCounters(uint32_t statsSlotIndex) const {
	MeshletCullingCountersGPU counters{};
	if (globalMeshletCountersSSBO.buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return counters;
	}

	void* mapped = nullptr;
	if (vmaMapMemory(allocator, globalMeshletCountersSSBO.allocation, &mapped) != VK_SUCCESS) {
		return counters;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(MeshletCullingCountersGPU);
	vmaInvalidateAllocation(allocator, globalMeshletCountersSSBO.allocation, offset, sizeof(MeshletCullingCountersGPU));
	std::memcpy(&counters, static_cast<const std::uint8_t*>(mapped) + offset, sizeof(MeshletCullingCountersGPU));
	vmaUnmapMemory(allocator, globalMeshletCountersSSBO.allocation);
	return counters;
}

uint32_t VulkanContext::Impl::GetStatsSlotIndex(uint32_t frameIndex, VulkanContext::ViewType view) const {
	const uint32_t clampedFrameIndex = std::min(frameIndex, kMaxStatsFrameSlots - 1u);
	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	return clampedFrameIndex * kStatsViewCount + viewIndex;
}

void VulkanContext::Impl::RecordPendingStats(uint32_t statsSlotIndex, const RenderStatsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingStatsBySlot[statsSlotIndex] = snapshot;
}

void VulkanContext::Impl::PublishCompletedFrameStats(uint32_t frameIndex) {
	const uint32_t gameSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Game);
	const uint32_t sceneSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Scene);

	RenderStatsSnapshot gameSnapshot = pendingStatsBySlot[gameSlot];
	const MeshletCullingCountersGPU gameCounters = ReadMeshletCounters(gameSlot);
	if (gameSnapshot.valid) {
		const bool useGpuCounters = (gameCounters.meshletsTotal > 0u) || (gameSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			gameSnapshot.meshletsTotal = gameCounters.meshletsTotal;
			gameSnapshot.meshletsDispatched = gameCounters.meshletsDispatched;
			gameSnapshot.meshletsCulled = gameCounters.meshletsCulled;
		}
	}

	RenderStatsSnapshot sceneSnapshot = pendingStatsBySlot[sceneSlot];
	const MeshletCullingCountersGPU sceneCounters = ReadMeshletCounters(sceneSlot);
	if (sceneSnapshot.valid) {
		const bool useGpuCounters = (sceneCounters.meshletsTotal > 0u) || (sceneSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			sceneSnapshot.meshletsTotal = sceneCounters.meshletsTotal;
			sceneSnapshot.meshletsDispatched = sceneCounters.meshletsDispatched;
			sceneSnapshot.meshletsCulled = sceneCounters.meshletsCulled;
		}
	}

	std::lock_guard<std::mutex> lock(statsMutex);
	latestStatsByView[0] = gameSnapshot;
	latestStatsByView[1] = sceneSnapshot;
}

RenderStatsSnapshot VulkanContext::Impl::GetLatestStats(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestStatsByView[index];
}

uint32_t VulkanContext::Impl::AllocateTextureIndex() {
	if (!freeTextureIndices.empty()) {
		uint32_t index = freeTextureIndices.back();
		freeTextureIndices.pop_back();
		return index;
	}
	if (nextTextureIndex >= MAX_BINDLESS_TEXTURES) {
		TERRAENGINE_LOG(LogLevel::Error, "Max bindless textures exceeded!");
		return 0; // Return default texture
	}
	return nextTextureIndex++;
}

uint32_t VulkanContext::Impl::AllocateMaterialIndex() {
	if (!freeMaterialIndices.empty()) {
		uint32_t index = freeMaterialIndices.back();
		freeMaterialIndices.pop_back();
		return index;
	}
	if (nextMaterialIndex >= MAX_BINDLESS_MATERIALS) {
		TERRAENGINE_LOG(LogLevel::Error, "Max bindless materials exceeded!");
		return 0; 
	}
	return nextMaterialIndex++;
}

void VulkanContext::Impl::UpdateGlobalTexture(uint32_t index, const VulkanImage& image) {
	VkDescriptorImageInfo imageInfo{};
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imageInfo.imageView = image.view;
	imageInfo.sampler = image.sampler;

	VkWriteDescriptorSet descriptorWrite{};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = globalDescriptorSet;
	descriptorWrite.dstBinding = 0;
	descriptorWrite.dstArrayElement = index;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.pImageInfo = &imageInfo;

	vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
}
