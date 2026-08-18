// VulkanGlobalResources.cpp — Global descriptor-backed buffers and environment resources
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"

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
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 8: Point-shadow sampler can change when shadow resources are resized between editor views.
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 9: Bindless Buffers
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 10: Hi-Z Texture
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 11: Hi-Z sampler can be republished while multi-view command buffers are still executable.
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
		if (commandBuffer != VK_NULL_HANDLE)
		{
			VkBufferCopy copyRegion{};
			copyRegion.srcOffset = 0u;
			copyRegion.dstOffset = 0u;
			copyRegion.size = bufferSize;
			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
				vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBOs[i].buffer, 1, &copyRegion);
			}
			EndSingleTimeCommands(commandBuffer, "fallback material upload");
		}
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
	const VkDeviceSize indirectBufferSize =
		static_cast<VkDeviceSize>(INITIAL_INDIRECT_DRAW_COMMAND_CAPACITY)
		* sizeof(VkDrawMeshTasksIndirectCommandEXT);
	indirectDrawBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		indirectDrawBuffers[i] = CreateBuffer(indirectBufferSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU,
			"Frame Indirect Draw Command Buffer");
		indirectDrawCommandCounts[static_cast<std::size_t>(i)] = 0u;
	}
}

bool VulkanContext::Impl::ReserveIndirectDrawCommands(
	std::size_t commandCount,
	std::uint32_t& commandOffset)
{
	commandOffset = 0u;
	if (currentFrame >= indirectDrawBuffers.size()
		|| currentFrame >= indirectDrawCommandCounts.size())
	{
		return false;
	}

	VulkanBuffer& currentBuffer = indirectDrawBuffers[currentFrame];
	const std::size_t currentCapacity = currentBuffer.size
		/ sizeof(VkDrawMeshTasksIndirectCommandEXT);
	const TerraEngine::Renderer::Vulkan::IndirectDrawBufferCapacityPlan plan =
		TerraEngine::Renderer::Vulkan::PlanIndirectDrawBufferCapacity(
			currentCapacity,
			indirectDrawCommandCounts[currentFrame],
			commandCount);
	if (!plan.valid
		|| plan.commandOffset > std::numeric_limits<std::uint32_t>::max()
		|| plan.requiredCommandCount > std::numeric_limits<std::uint32_t>::max()
		|| plan.targetCommandCapacity
			> std::numeric_limits<VkDeviceSize>::max()
				/ sizeof(VkDrawMeshTasksIndirectCommandEXT))
	{
		return false;
	}

	const VkDeviceSize targetBytes = static_cast<VkDeviceSize>(
		plan.targetCommandCapacity)
		* sizeof(VkDrawMeshTasksIndirectCommandEXT);
	if (currentBuffer.buffer == VK_NULL_HANDLE
		|| currentBuffer.size < targetBytes)
	{
		// A later Scene/Game view can grow this slot after an earlier view already
		// recorded commands. Keep the old allocation alive through the slot fence;
		// command buffers retain their VkBuffer handle and must never be redirected.
		VulkanBuffer replacement = CreateBuffer(
			targetBytes,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU,
			"Frame Indirect Draw Command Buffer");
		if (replacement.buffer == VK_NULL_HANDLE)
		{
			return false;
		}
		RetireBuffer(currentBuffer);
		currentBuffer = replacement;
	}

	indirectDrawCommandCounts[currentFrame] = plan.requiredCommandCount;
	commandOffset = static_cast<std::uint32_t>(plan.commandOffset);
	return true;
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
		if (clearCommandBuffer != VK_NULL_HANDLE)
		{
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
			EndSingleTimeCommands(clearCommandBuffer, "ambient occlusion initialization clear");
		}
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
		if (transitionCommandBuffer == VK_NULL_HANDLE)
		{
			return;
		}
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
		EndSingleTimeCommands(transitionCommandBuffer, "ambient occlusion depth-history transition");
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

