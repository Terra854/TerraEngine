// Native CMAA2 resources and command recording for one retained view. The graph
// owner decides capability publication and retains this pass until GPU completion.
#include "VulkanPostProcessCmaa2.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct Constants
		{
			std::uint32_t width, height, capacity, maxSearch;
			float threshold, localContrast, armRatio;
			std::uint32_t reserved;
		};
		static_assert(sizeof(Constants) == 32 && offsetof(Constants, threshold) == 16);
		static_assert(offsetof(Constants, reserved) == 28 && sizeof(VkDispatchIndirectCommand) == 12);
		struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VmaAllocation allocation = VK_NULL_HANDLE; VkDeviceSize bytes = 0; };
		constexpr std::array<const char*, 6> kEntries{"DetectEdges", "CountRows", "PrefixRows", "CompactRows", "Classify", "Resolve"};
		constexpr std::array<VkDescriptorType, 9> kTypes{
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		void Require(bool condition, const char* reason) { if(!condition) throw std::runtime_error(reason); }
		bool QualifyImage(VkPhysicalDevice physical, const PostProcessTargetImage& image, VkFormatFeatureFlags features, const char*& reason) noexcept
		{
			const auto Check = [&](bool valid, const char* message) noexcept { if(!valid) reason = message; return valid; };
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			if(!Check((format.optimalTilingFeatures & features) == features, "CMAA2 image format lacks required optimal-tiling features.")) return false;
			VkImageFormatProperties limits{};
			if(!Check(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
				image.usage, 0, &limits) == VK_SUCCESS, "CMAA2 exact image-usage combination is unsupported.")) return false;
			if(!Check(image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height
				&& limits.maxExtent.depth >= 1 && limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1
				&& (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "CMAA2 extent/sample count exceeds exact format limits.")) return false;
			return true;
		}
		struct PhysicalLayout
		{
			std::uint32_t width = 0, height = 0, capacity = 0;
			std::array<VkDeviceSize,6> bytes{};
		};
		// Capability discovery and actual preparation share these stack-only
		// physical queries. No GPU/CPU heap allocation or shader IO occurs here.
		bool QualifyPhysicalView(VkPhysicalDevice physical, const PostProcessTargetImage& source,
			const PostProcessTargetImage& destination, PhysicalLayout& result, const char*& reason) noexcept
		{
			const auto Check = [&](bool valid, const char* message) noexcept { if(!valid) reason = message; return valid; };
			if(!Check(physical && source.extent.width && source.extent.height
				&& source.extent.width == destination.extent.width && source.extent.height == destination.extent.height,
				"CMAA2 physical query needs matching nonempty extents and an actual physical device.")) return false;
			const auto supportedFormat = [](VkFormat format) { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; };
			if(!Check(supportedFormat(source.format) && source.format == destination.format, "CMAA2 exact alpha preservation requires matching linear float RGBA variants.")) return false;
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
			if(!Check(properties.apiVersion >= VK_API_VERSION_1_3, "CMAA2 compiled modules require Vulkan1.3 physical support.")) return false;
			const auto& limits = properties.limits;
			const auto width = source.extent.width, height = source.extent.height;
			if(!Check(width <= std::min(16384u, limits.maxImageDimension2D) && height <= std::min(16384u, limits.maxImageDimension2D), "CMAA2 extent exceeds bounded shader/device dimensions.")) return false;
			if(!Check(limits.maxComputeWorkGroupInvocations >= 64 && limits.maxComputeWorkGroupSize[0] >= 64 && limits.maxComputeWorkGroupSize[1] >= 8
				&& limits.maxComputeWorkGroupSize[2] >= 1 && limits.maxPushConstantsSize >= sizeof(Constants)
				&& limits.maxComputeSharedMemorySize >= 65u*sizeof(std::uint32_t), "CMAA2 compute workgroup/push limits are insufficient.")) return false;
			if(!Check(limits.maxComputeWorkGroupCount[0] >= 65535 && limits.maxComputeWorkGroupCount[1] >= (height+7)/8
				&& limits.maxComputeWorkGroupCount[2] >= 1, "CMAA2 dispatch dimensions exceed physical limits.")) return false;
			if(!Check(limits.maxPerStageDescriptorStorageBuffers >= 6 && limits.maxDescriptorSetStorageBuffers >= 6
				&& limits.maxPerStageDescriptorStorageImages >= 2 && limits.maxDescriptorSetStorageImages >= 2
				&& limits.maxPerStageDescriptorSampledImages >= 1 && limits.maxDescriptorSetSampledImages >= 1
				&& limits.maxPerStageResources >= 9, "CMAA2 descriptor limits are insufficient.")) return false;
			if(!QualifyImage(physical, source, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, reason)) return false;
			if(!QualifyImage(physical, destination, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, reason)) return false;
			PostProcessTargetImage edgeDescription; edgeDescription.extent = source.extent; edgeDescription.format = VK_FORMAT_R32_UINT;
			edgeDescription.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if(!QualifyImage(physical, edgeDescription, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT, reason)) return false;
			const VkDeviceSize pixels = VkDeviceSize(width)*height;
			if(!Check(pixels <= std::numeric_limits<std::uint32_t>::max()-63u, "CMAA2 candidate counter/indirect arithmetic would overflow.")) return false;
			if(!Check(pixels <= std::numeric_limits<VkDeviceSize>::max()/20u, "CMAA2 private buffer byte count overflow.")) return false;
			const std::array<VkDeviceSize, 6> bytes{pixels*4, VkDeviceSize(height)*4, VkDeviceSize(height)*4,16,12,pixels*20};
			VkPhysicalDeviceMaintenance3Properties maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
			VkPhysicalDeviceProperties2 detailed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; detailed.pNext = &maintenance;
			vkGetPhysicalDeviceProperties2(physical, &detailed);
			VkDeviceSize total = pixels*4;
			for(const auto size : bytes)
			{
				if(!Check(size <= limits.maxStorageBufferRange && size <= maintenance.maxMemoryAllocationSize, "CMAA2 full-capacity buffer exceeds physical storage/allocation limits.")) return false;
				if(!Check(total <= std::numeric_limits<VkDeviceSize>::max()-size, "CMAA2 total private allocation overflow.")) return false; total += size;
			}
			VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
			VkDeviceSize largestHeap = 0;
			for(std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) if(memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) largestHeap = std::max(largestHeap, memory.memoryHeaps[i].size);
			if(!Check(total <= largestHeap, "CMAA2 full private working set exceeds the physical local heap.")) return false;
			const VkDeviceSize groups = (pixels+63)/64;
			if(!Check((groups+65534)/65535 <= limits.maxComputeWorkGroupCount[1], "CMAA2 dense indirect grid exceeds physical Y limit.")) return false;
			result = {width,height,static_cast<std::uint32_t>(pixels),bytes};
			return true;
		}
		void ImageBarrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
			VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess, VkPipelineStageFlags destinationStage, VkAccessFlags destinationAccess)
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = destinationAccess;
			barrier.oldLayout = oldLayout; barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		void ShaderDependency(VkCommandBuffer command, bool indirect = false)
		{
			// All resources in this private pass are exclusive to the serialized
			// view. A memory dependency covers its actual SSBO/image writes; layouts
			// stay GENERAL between compute stages. Include indirect command reads
			// after PrefixRows, whose args are generated by a shader on the GPU.
			VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
				| (indirect ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : 0);
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | (indirect ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT : 0),
				0, 1, &barrier, 0, nullptr, 0, nullptr);
		}
	}

	struct Cmaa2Pass
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		const PostProcessTargetImage* source = nullptr; // borrowed stable records
		PostProcessTargetImage* destination = nullptr;
		VkImage sourceImage = VK_NULL_HANDLE, destinationImage = VK_NULL_HANDLE;
		VkImageView sourceView = VK_NULL_HANDLE, destinationView = VK_NULL_HANDLE;
		PostProcessTargetImage edges;
		std::array<Buffer, 6> buffers; // candidates, row counts/offsets, counters, args, coefficients
		VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		std::array<VkPipeline, 6> pipelines{};
		Constants constants{};
		~Cmaa2Pass()
		{
			// The completed bundle owner proves no borrowing GPU work remains.
			// Descriptors and pipelines disappear before the allocations they borrow.
			if(!device) return;
			for(auto pipeline : pipelines) if(pipeline) vkDestroyPipeline(device, pipeline, nullptr);
			if(layout) vkDestroyPipelineLayout(device, layout, nullptr);
			if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			if(descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
			if(edges.view) vkDestroyImageView(device, edges.view, nullptr);
			if(edges.image && allocator) vmaDestroyImage(allocator, edges.image, edges.allocation);
			for(const auto& buffer : buffers) if(buffer.buffer && allocator) vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
		}
	};

	namespace
	{
		void AllocateBuffer(Cmaa2Pass& pass, Buffer& target, VkDeviceSize bytes, bool indirect)
		{
			target.bytes = bytes;
			VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			info.size = bytes; info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
				| (indirect ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0); info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			Require(vmaCreateBuffer(pass.allocator, &info, &allocation, &target.buffer, &target.allocation, nullptr) == VK_SUCCESS,
				"CMAA2 device-local buffer allocation failed; full candidate capacity is retained.");
		}
		void AllocateEdges(Cmaa2Pass& pass)
		{
			auto& target = pass.edges;
			target.extent = pass.source->extent; target.format = VK_FORMAT_R32_UINT;
			target.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D; info.format = target.format; info.extent = {target.extent.width, target.extent.height, 1};
			info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = target.usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			Require(vmaCreateImage(pass.allocator, &info, &allocation, &target.image, &target.allocation, nullptr) == VK_SUCCESS,
				"CMAA2 edge-image allocation failed.");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = target.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = target.format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			Require(vkCreateImageView(pass.device, &view, nullptr, &target.view) == VK_SUCCESS, "CMAA2 edge view creation failed.");
		}
		void CreateDescriptors(Cmaa2Pass& pass)
		{
			std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
			for(std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, kTypes[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			info.bindingCount = static_cast<std::uint32_t>(bindings.size()); info.pBindings = bindings.data();
			Require(vkCreateDescriptorSetLayout(pass.device, &info, nullptr, &pass.descriptors) == VK_SUCCESS, "CMAA2 descriptor layout creation failed.");
			const std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool.maxSets = 1; pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size()); pool.pPoolSizes = sizes.data();
			Require(vkCreateDescriptorPool(pass.device, &pool, nullptr, &pass.pool) == VK_SUCCESS, "CMAA2 descriptor pool creation failed.");
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocation.descriptorPool = pass.pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &pass.descriptors;
			Require(vkAllocateDescriptorSets(pass.device, &allocation, &pass.set) == VK_SUCCESS, "CMAA2 descriptor allocation failed.");
			const std::array<VkDescriptorImageInfo, 3> images{{
				{VK_NULL_HANDLE, pass.source->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				{VK_NULL_HANDLE, pass.edges.view, VK_IMAGE_LAYOUT_GENERAL},
				{VK_NULL_HANDLE, pass.destination->view, VK_IMAGE_LAYOUT_GENERAL}}};
			std::array<VkDescriptorBufferInfo, 6> buffers{};
			std::array<VkWriteDescriptorSet, 9> writes{};
			for(std::uint32_t i = 0; i < writes.size(); ++i)
			{
				auto& write = writes[i]; write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				write.dstSet = pass.set; write.dstBinding = i; write.descriptorCount = 1; write.descriptorType = kTypes[i];
				if(i == 0) write.pImageInfo = &images[0];
				else if(i == 1) write.pImageInfo = &images[1];
				else if(i == 8) write.pImageInfo = &images[2];
				else { buffers[i-2] = {pass.buffers[i-2].buffer, 0, pass.buffers[i-2].bytes}; write.pBufferInfo = &buffers[i-2]; }
			}
			vkUpdateDescriptorSets(pass.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
			VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants)};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout.setLayoutCount = 1; layout.pSetLayouts = &pass.descriptors; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
			Require(vkCreatePipelineLayout(pass.device, &layout, nullptr, &pass.layout) == VK_SUCCESS, "CMAA2 pipeline layout creation failed.");
		}
		void CreatePipeline(Cmaa2Pass& pass, std::size_t index)
		{
			const std::string suffix = pass.destination->format == VK_FORMAT_R16G16B16A16_SFLOAT ? "16.comp.spv" : "32.comp.spv";
			const std::string path = std::string("Shaders/PostProcess/CMAA2") + kEntries[index] + suffix;
			auto& resources = ResourceManager::Instance();
			const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			Require(!bytes.empty() && bytes.size() % 4 == 0, "CMAA2 exact installed shader variant is unavailable.");
			std::vector<std::uint32_t> words(bytes.size()/4); std::memcpy(words.data(), bytes.data(), bytes.size());
			Require(words.size() >= 5 && words[0] == 0x07230203u, "CMAA2 SPIR-V header is invalid.");
			// These modules need only core Shader capability. Reject a changed module
			// requiring unenabled device features before attempting its pipeline.
			std::unordered_map<std::uint32_t,std::uint32_t> variables,pointers,imageFormats;
			std::uint32_t outputVariable = 0;
			bool localSizeMatches = false;
			for(std::size_t offset = 5; offset < words.size();)
			{
				const auto count = words[offset] >> 16u, opcode = words[offset] & 65535u;
				Require(count && count <= words.size()-offset, "CMAA2 SPIR-V instruction bound is invalid.");
				if(opcode == 17u) Require(count == 2 && words[offset+1] == 1u, "CMAA2 shader needs an unqualified optional capability.");
				if(opcode == 71u && count == 4 && words[offset+2] == 33u)
				{
					Require(words[offset+3] < 9u, "CMAA2 shader descriptor binding exceeds prepared ABI.");
					if(words[offset+3] == 8u) outputVariable = words[offset+1];
				}
				if(opcode == 71u && count == 4 && words[offset+2] == 34u) Require(words[offset+3] == 0, "CMAA2 shader uses an unprepared descriptor set.");
				if(opcode == 59u && count >= 4) variables[words[offset+2]] = words[offset+1];
				if(opcode == 32u && count == 4) pointers[words[offset+1]] = words[offset+3];
				if(opcode == 25u && count >= 9) imageFormats[words[offset+1]] = words[offset+8];
				if(opcode == 16u && count == 6 && words[offset+2] == 17u)
				{
					const std::array<std::uint32_t,3> expected = index == 0 || index == 5
						? std::array<std::uint32_t,3>{8,8,1} : std::array<std::uint32_t,3>{64,1,1};
					localSizeMatches = words[offset+3] == expected[0] && words[offset+4] == expected[1] && words[offset+5] == expected[2];
				}
				offset += count;
			}
			Require(localSizeMatches, "CMAA2 module workgroup size differs from qualified dispatch ABI.");
			if(index == 5)
			{
				Require(outputVariable && variables.contains(outputVariable) && pointers.contains(variables.at(outputVariable)), "CMAA2 resolve output image binding is missing.");
				const auto imageType = pointers.at(variables.at(outputVariable));
				const auto expected = pass.destination->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 2u : 1u;
				Require(imageFormats.contains(imageType) && imageFormats.at(imageType) == expected, "CMAA2 resolve storage image format differs from exact destination variant.");
			}
			VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			moduleInfo.codeSize = bytes.size(); moduleInfo.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			Require(vkCreateShaderModule(pass.device, &moduleInfo, nullptr, &module) == VK_SUCCESS, "CMAA2 shader module creation failed.");
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			pipeline.stage.module = module; pipeline.stage.pName = kEntries[index]; pipeline.layout = pass.layout;
			const auto result = vkCreateComputePipelines(pass.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pass.pipelines[index]);
			vkDestroyShaderModule(pass.device, module, nullptr);
			Require(result == VK_SUCCESS, "CMAA2 compute pipeline creation failed.");
		}
	}

	bool SupportsCmaa2View(VkPhysicalDevice physical, VkExtent2D extent, VkFormat format) noexcept
	{
		// These are the standard finished-image target usages. Prepare separately
		// reuses the helper with the exact borrowed images' actual usage masks.
		PostProcessTargetImage source; source.extent = extent; source.format = format;
		source.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		const auto destination = source;
		PhysicalLayout qualified;
		const char* reason = nullptr;
		return QualifyPhysicalView(physical,source,destination,qualified,reason);
	}

	std::shared_ptr<Cmaa2Pass> PrepareCmaa2Pass(VkPhysicalDevice physical, PostProcessTargetBundle& targets,
		const PostProcessTargetImage& source, PostProcessTargetImage& destination, std::string& reason)
	{
		reason.clear();
		try
		{
			Require(physical && targets.device && targets.allocator && source.image && source.view && destination.image && destination.view,
				"CMAA2 prepare requires actual device/allocator and view resources.");
			Require(source.image != destination.image && source.view != destination.view, "CMAA2 source and destination must not alias.");
			Require(source.extent.width && source.extent.height && source.extent.width == destination.extent.width && source.extent.height == destination.extent.height,
				"CMAA2 requires matching nonempty native input/output extents.");
			Require((source.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (destination.usage & VK_IMAGE_USAGE_STORAGE_BIT) && (destination.usage & VK_IMAGE_USAGE_SAMPLED_BIT),
				"CMAA2 requires sampled source and distinct sampled/storage output.");
			PhysicalLayout qualified;
			const char* physicalReason = nullptr;
			const bool supported = QualifyPhysicalView(physical,source,destination,qualified,physicalReason);
			Require(supported,physicalReason ? physicalReason : "CMAA2 physical requirements are unsupported.");
			const auto width = qualified.width, height = qualified.height;
			const auto& bytes = qualified.bytes;
			auto pass = std::make_shared<Cmaa2Pass>();
			pass->device = targets.device; pass->allocator = targets.allocator; pass->source = &source; pass->destination = &destination;
			pass->sourceImage = source.image; pass->destinationImage = destination.image; pass->sourceView = source.view; pass->destinationView = destination.view;
			pass->constants = {width,height,qualified.capacity,64,0.05f,0.5f,1.5f,0};
			for(std::size_t i = 0; i < bytes.size(); ++i) AllocateBuffer(*pass, pass->buffers[i], bytes[i], i == 4);
			AllocateEdges(*pass); CreateDescriptors(*pass);
			for(std::size_t i = 0; i < pass->pipelines.size(); ++i) CreatePipeline(*pass,i);
			return pass;
		}
		catch(const std::exception& error) { reason = error.what(); return {}; }
	}

	void RecordCmaa2Pass(VkCommandBuffer command, Cmaa2Pass& pass)
	{
		Require(command && pass.source && pass.destination && pass.source->image == pass.sourceImage && pass.destination->image == pass.destinationImage
			&& pass.source->view == pass.sourceView && pass.destination->view == pass.destinationView, "CMAA2 record borrowed resource identity changed.");
		Require(pass.source->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "CMAA2 source is not shader-readable at record boundary.");
		Require(pass.source->extent.width == pass.constants.width && pass.source->extent.height == pass.constants.height
			&& pass.destination->extent.width == pass.constants.width && pass.destination->extent.height == pass.constants.height, "CMAA2 record extent changed without preparation.");
		ImageBarrier(command, pass.sourceImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		const auto old = pass.destination->layout;
		ImageBarrier(command, pass.destinationImage, old, VK_IMAGE_LAYOUT_GENERAL,
			old == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
		// Edge contents are deliberately discarded on every recording. UNDEFINED
		// is legal for discard regardless of the previous layout; execution still
		// waits for earlier queue users. No accepted-layout guess is needed after
		// an abandoned recording that never reached a GPU submission.
		ImageBarrier(command, pass.edges.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		// Private scratch is reused only on the same serialized view queue. Order prior
		// readers/writers before filling all private buffers; never rely on stale
		// counters, queue tails or coefficient values from a preceding frame.
		VkMemoryBarrier beforeClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		beforeClear.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; beforeClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,1,&beforeClear,0,nullptr,0,nullptr);
		for(const auto& buffer : pass.buffers) vkCmdFillBuffer(command,buffer.buffer,0,buffer.bytes,0);
		const VkClearColorValue clear{}; const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
		vkCmdClearColorImage(command,pass.edges.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&color);
		VkMemoryBarrier afterClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		afterClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; afterClear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&afterClear,0,nullptr,0,nullptr);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pass.layout,0,1,&pass.set,0,nullptr);
		vkCmdPushConstants(command,pass.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Constants),&pass.constants);
		const auto dispatch = [&](std::size_t pipeline,std::uint32_t x,std::uint32_t y) { vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipelines[pipeline]); vkCmdDispatch(command,x,y,1); };
		dispatch(0,(pass.constants.width+7)/8,(pass.constants.height+7)/8); ShaderDependency(command);
		dispatch(1,pass.constants.height,1); ShaderDependency(command);
		dispatch(2,1,1); ShaderDependency(command,true);
		dispatch(3,pass.constants.height,1); ShaderDependency(command,true);
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipelines[4]);
		vkCmdDispatchIndirect(command,pass.buffers[4].buffer,0); ShaderDependency(command);
		dispatch(5,(pass.constants.width+7)/8,(pass.constants.height+7)/8);
		ImageBarrier(command,pass.destinationImage,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
		pass.destination->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; pass.edges.layout = VK_IMAGE_LAYOUT_GENERAL;
	}
}
