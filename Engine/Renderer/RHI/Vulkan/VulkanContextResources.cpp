#include "VulkanContextImpl.h"

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
	allocInfo.commandPool = commandPool;
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

	vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
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
	for (auto& outerEntry : pipelineCache) {
		auto& map = outerEntry.second;
		for (auto& innerEntry : map) {
			VulkanPipeline& pipe = innerEntry.second;
			if (device) {
				vkDestroyPipeline(device, pipe.pipeline, nullptr);
				vkDestroyPipelineLayout(device, pipe.layout, nullptr);
				for (VkDescriptorSetLayout layout : pipe.setLayouts) {
					vkDestroyDescriptorSetLayout(device, layout, nullptr);
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
	if (frameBuffers.size() <= workerIndex) frameBuffers.resize(workerIndex + 1);

	std::vector<VkCommandBuffer>& buffers = frameBuffers[workerIndex];
	if (!buffers.empty()) return buffers[0];

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
	if (textureCache.contains(textureId)) return;

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
}

std::vector<VkDescriptorSet> VulkanContext::Impl::EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId) {
	VulkanPipeline* pipeline = nullptr;
	for (auto& entry : pipelineCache) {
		auto& map = entry.second;
		if (map.contains(shaderCollectionId)) {
			pipeline = &map[shaderCollectionId];
			break;
		}
	}
	if (!pipeline) return {};

	auto& cacheForPipeline = materialDescriptorSets[shaderCollectionId];
	if (cacheForPipeline.contains(materialId)) return cacheForPipeline[materialId];

	if (pipeline->setLayouts.empty()) return {};
	if (descriptorPool == VK_NULL_HANDLE) return {};

	std::vector<VkDescriptorSet> sets(pipeline->setLayouts.size(), VK_NULL_HANDLE);

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = descriptorPool;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(pipeline->setLayouts.size());
	allocInfo.pSetLayouts = pipeline->setLayouts.data();

	if (vkAllocateDescriptorSets(device, &allocInfo, sets.data()) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to allocate descriptor sets for material!");
		return {};
	}

	Material* material = ResourceManager::Instance().GetMaterial(materialId);
	TerraEngine::UUID primaryTextureId = TerraEngine::EMPTY_UUID;
	if (material && material->header.textureCount > 0 && !material->textures.empty()) {
		primaryTextureId = material->textures[0].textureUuid;
	}

	if (primaryTextureId != TerraEngine::EMPTY_UUID) {
		EnsureTexture(primaryTextureId);
	}

	const VulkanImage* chosenTexture = &defaultTexture;
	if (primaryTextureId != TerraEngine::EMPTY_UUID && textureCache.contains(primaryTextureId)) {
		chosenTexture = &textureCache[primaryTextureId];
	}

	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkDescriptorImageInfo> imageInfos;
	std::vector<VkDescriptorBufferInfo> bufferInfos;

	std::size_t imageInfoReserve = 0;
	std::size_t bufferInfoReserve = 0;
	for (const auto& setBindings : pipeline->bindingsPerSet) {
		for (const DescriptorBinding& binding : setBindings) {
			const VkDescriptorType vkType = MapDescriptorType(static_cast<DescriptorType>(binding.type));
			const uint32_t descriptorCount = binding.arrayCount > 0 ? binding.arrayCount : 1;
			if (vkType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || vkType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || vkType == VK_DESCRIPTOR_TYPE_SAMPLER) {
				imageInfoReserve += descriptorCount;
			} else if (vkType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || vkType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				bufferInfoReserve += descriptorCount;
			}
		}
	}
	imageInfos.reserve(imageInfoReserve);
	bufferInfos.reserve(bufferInfoReserve);

	for (size_t setIndex = 0; setIndex < pipeline->bindingsPerSet.size(); ++setIndex) {
		for (const DescriptorBinding& binding : pipeline->bindingsPerSet[setIndex]) {
			const VkDescriptorType vkType = MapDescriptorType(static_cast<DescriptorType>(binding.type));
			uint32_t descriptorCount = binding.arrayCount > 0 ? binding.arrayCount : 1;
			if (vkType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || vkType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
				const size_t start = imageInfos.size();
				for (uint32_t i = 0; i < descriptorCount; ++i) {
					VkDescriptorImageInfo info{};
					info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					info.imageView = chosenTexture->view;
					info.sampler = (vkType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ? chosenTexture->sampler : VK_NULL_HANDLE;
					imageInfos.push_back(info);
				}

				VkWriteDescriptorSet write{};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = sets[setIndex];
				write.dstBinding = binding.binding;
				write.descriptorType = vkType;
				write.descriptorCount = descriptorCount;
				write.pImageInfo = &imageInfos[start];
				writes.push_back(write);
			} else if (vkType == VK_DESCRIPTOR_TYPE_SAMPLER) {
				const size_t start = imageInfos.size();
				for (uint32_t i = 0; i < descriptorCount; ++i) {
					VkDescriptorImageInfo info{};
					info.sampler = chosenTexture->sampler;
					imageInfos.push_back(info);
				}

				VkWriteDescriptorSet write{};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = sets[setIndex];
				write.dstBinding = binding.binding;
				write.descriptorType = vkType;
				write.descriptorCount = descriptorCount;
				write.pImageInfo = &imageInfos[start];
				writes.push_back(write);
			} else if (vkType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || vkType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				const size_t start = bufferInfos.size();
				for (uint32_t i = 0; i < descriptorCount; ++i) {
					VkDescriptorBufferInfo bufferInfo{};
					bufferInfo.buffer = VK_NULL_HANDLE;
					bufferInfo.offset = 0;
					bufferInfo.range = VK_WHOLE_SIZE;
					bufferInfos.push_back(bufferInfo);
				}

				VkWriteDescriptorSet write{};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = sets[setIndex];
				write.dstBinding = binding.binding;
				write.descriptorType = vkType;
				write.descriptorCount = descriptorCount;
				write.pBufferInfo = &bufferInfos[start];
				writes.push_back(write);
			} else {
				TERRAENGINE_LOG(LogLevel::Warning, "Unsupported descriptor type in material binding; leaving binding " << binding.binding << " unset");
			}
		}
	}

	if (!writes.empty()) {
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}

	cacheForPipeline[materialId] = sets;
	return cacheForPipeline[materialId];
}

void VulkanContext::Impl::EnsureMesh(TerraEngine::UUID meshId) {
	if (meshCache.contains(meshId)) return;
	
	Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
	if (!cpuMesh) return;

	VkDeviceSize vertexSize = cpuMesh->vertexData.size();
	VkDeviceSize indexSize = cpuMesh->indexData.size();

	VulkanBuffer stagingVertex = CreateBuffer(vertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	void* data;
	vmaMapMemory(allocator, stagingVertex.allocation, &data);
	memcpy(data, cpuMesh->vertexData.data(), vertexSize);
	vmaUnmapMemory(allocator, stagingVertex.allocation);

	VulkanBuffer stagingIndex = CreateBuffer(indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	vmaMapMemory(allocator, stagingIndex.allocation, &data);
	memcpy(data, cpuMesh->indexData.data(), indexSize);
	vmaUnmapMemory(allocator, stagingIndex.allocation);

	VulkanMesh vMesh;
	vMesh.vertexBuffer = CreateBuffer(vertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.indexBuffer = CreateBuffer(indexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	vMesh.indexCount = cpuMesh->header.indexCount;
	vMesh.indexType = (cpuMesh->header.indexFormat == static_cast<uint32_t>(IndexFormat::UInt16)) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

	CopyBuffer(stagingVertex.buffer, vMesh.vertexBuffer.buffer, vertexSize);
	CopyBuffer(stagingIndex.buffer, vMesh.indexBuffer.buffer, indexSize);

	vmaDestroyBuffer(allocator, stagingVertex.buffer, stagingVertex.allocation);
	vmaDestroyBuffer(allocator, stagingIndex.buffer, stagingIndex.allocation);

	meshCache[meshId] = vMesh;
}

bool VulkanContext::Impl::BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants) {
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
			if (pc.offsetBytes == 0 && pc.sizeBytes >= sizeof(PushConstants) && (vkStages & VK_SHADER_STAGE_VERTEX_BIT)) {
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
		fallback.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pushConstants.push_back(fallback);
		supportsDefaultPushConstants = true;
	}

	for (const auto& [setIndex, bindingMap] : setBindings) {
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
			for (VkDescriptorSetLayout createdLayout : setLayouts) {
				vkDestroyDescriptorSetLayout(device, createdLayout, nullptr);
			}
			return false;
		}

		setLayouts.push_back(layout);
		bindingsPerSet.push_back(std::move(storedBindings));
	}

	return true;
}

void VulkanContext::Impl::EnsurePipeline(TerraEngine::UUID shaderCollectionId, VkRenderPass renderPass) {
	if (pipelineCache.contains(renderPass) && pipelineCache[renderPass].contains(shaderCollectionId)) return;
	
	ShaderCollection* collection = ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
	if (!collection) return;

	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
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
			if (stage.stage == ShaderStage::Vertex) stageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
			else if (stage.stage == ShaderStage::Fragment) stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			
			stageInfo.module = shaderModule;
			stageInfo.pName = shader->stringTable.data() + shader->binary.entryPointOffset;
			shaderStages.push_back(stageInfo);
		}
	}

	if (shaderStages.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "No shader stages available to build pipeline");
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

	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = 0;
	bindingDescription.stride = 12;
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
	
	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].format = VK_FORMAT_A2B10G10R10_SNORM_PACK32;
	attributeDescriptions[0].offset = 0;

	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].format = VK_FORMAT_A2B10G10R10_SNORM_PACK32;
	attributeDescriptions[1].offset = 4;

	attributeDescriptions[2].binding = 0;
	attributeDescriptions[2].location = 2;
	attributeDescriptions[2].format = VK_FORMAT_R16G16_UNORM;
	attributeDescriptions[2].offset = 8;

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

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
	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
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
		for (auto& layout : setLayouts) {
			vkDestroyDescriptorSetLayout(device, layout, nullptr);
		}
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		throw std::runtime_error("failed to create pipeline layout!");
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

	VkPipeline graphicsPipeline;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
		for (auto& layout : setLayouts) {
			vkDestroyDescriptorSetLayout(device, layout, nullptr);
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		throw std::runtime_error("failed to create graphics pipeline!");
	}

	for (auto& stage : shaderStages) {
		vkDestroyShaderModule(device, stage.module, nullptr);
	}

	VulkanPipeline pipeline{};
	pipeline.pipeline = graphicsPipeline;
	pipeline.layout = pipelineLayout;
	pipeline.setLayouts = std::move(setLayouts);
	pipeline.bindingsPerSet = std::move(bindingsPerSet);
	pipeline.pushConstants = std::move(pushConstants);
	pipeline.supportsDefaultPushConstants = supportsDefaultPushConstants;

	pipelineCache[renderPass][shaderCollectionId] = std::move(pipeline);
}
