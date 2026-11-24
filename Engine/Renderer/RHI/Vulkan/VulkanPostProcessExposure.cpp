// VulkanPostProcessExposure.cpp - Bounded histogram, adaptation and application.
#include "VulkanPostProcessExposure.h"
#include "VulkanPostProcessTargets.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Resource/ResourceManager.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct Buffer
		{
			VkBuffer handle = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VkDeviceSize size = 0;
		};
		struct HistogramConstants
		{
			std::uint32_t width, height;
			float minimumLog, maximumLog;
		};
		struct ResolveConstants
		{
			float minimumLog, maximumLog, minimumEV, maximumEV;
			float lowPercentile, highPercentile, middleGrey, deltaSeconds;
			float brightenRate, darkenRate;
			std::uint32_t automaticEnabled, reset;
		};
		struct ApplicationConstants
		{
			std::uint32_t width, height;
			float compensationEV;
			std::uint32_t automaticEnabled;
		};
		static_assert(sizeof(HistogramConstants) == 16 && sizeof(ResolveConstants) == 48 && sizeof(ApplicationConstants) == 16);
		constexpr VkDeviceSize histogramBytes = 256u * sizeof(std::uint32_t);
		constexpr VkDeviceSize stateBytes = 16u; // Three floats and one uint valid.

		bool CreateBuffer(VmaAllocator allocator, Buffer& buffer, VkDeviceSize bytes, bool initializeState)
		{
			VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			create.size = bytes;
			create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{};
			allocation.usage = initializeState ? VMA_MEMORY_USAGE_CPU_TO_GPU : VMA_MEMORY_USAGE_GPU_ONLY;
			if(vmaCreateBuffer(allocator, &create, &allocation, &buffer.handle, &buffer.allocation, nullptr) != VK_SUCCESS) return false;
			buffer.size = bytes;
			if(!initializeState) return true;
			// Initialize pristine history once before publication. No initialization
			// submission or speculative CPU frame flag is needed. Later mutation is
			// exclusively GPU resolve/copy work in accepted command order.
			void* mapped = nullptr;
			if(vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS) return false;
			if(!mapped) { vmaUnmapMemory(allocator, buffer.allocation); return false; }
			std::memset(mapped, 0, static_cast<std::size_t>(bytes));
			const VkResult flushed = vmaFlushAllocation(allocator, buffer.allocation, 0, bytes);
			vmaUnmapMemory(allocator, buffer.allocation);
			return flushed == VK_SUCCESS;
		}

		bool CreateComputePipeline(VkDevice device, VkPipelineLayout layout, const char* path, const char* entry, VkPipeline& pipeline)
		{
			const auto bytes = ResourceManager::Instance().ReadFileBytes(ResourceManager::Instance().ResolveEngineAssetPath(path));
			if(bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) return false;
			std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
			std::memcpy(words.data(), bytes.data(), bytes.size());
			VkShaderModuleCreateInfo moduleCreate{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			moduleCreate.codeSize = bytes.size(); moduleCreate.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			if(vkCreateShaderModule(device, &moduleCreate, nullptr, &module) != VK_SUCCESS) return false;
			VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			create.layout = layout;
			create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; create.stage.module = module; create.stage.pName = entry;
			const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline);
			vkDestroyShaderModule(device, module, nullptr);
			return result == VK_SUCCESS;
		}

		void BufferBarrier(VkCommandBuffer command, const Buffer& buffer, VkAccessFlags sourceAccess,
			VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage)
		{
			VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
			barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = destinationAccess;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = buffer.handle; barrier.offset = 0; barrier.size = buffer.size;
			vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
		}
		void BindCompute(VkCommandBuffer command, VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet descriptor,
			const void* constants, std::uint32_t bytes)
		{
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &descriptor, 0, nullptr);
			vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, constants);
		}
	}

	struct AutoExposurePass
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkExtent2D extent{};
		VkImage output = VK_NULL_HANDLE;
		Buffer histogram, previousState, nextState;
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline histogramPipeline = VK_NULL_HANDLE, resolvePipeline = VK_NULL_HANDLE, applicationPipeline = VK_NULL_HANDLE;
		~AutoExposurePass()
		{
			// The enclosing target ledger proves completion before releasing this
			// owner. Pipelines/descriptors disappear before their private buffers.
			if(histogramPipeline) vkDestroyPipeline(device, histogramPipeline, nullptr);
			if(resolvePipeline) vkDestroyPipeline(device, resolvePipeline, nullptr);
			if(applicationPipeline) vkDestroyPipeline(device, applicationPipeline, nullptr);
			if(pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
			if(descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
			if(descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
			for(auto* buffer : {&histogram, &previousState, &nextState})
				if(buffer->handle) vmaDestroyBuffer(allocator, buffer->handle, buffer->allocation);
		}
	};

	std::shared_ptr<AutoExposurePass> PrepareAutoExposurePass(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& source, const PostProcessTargetImage& output, std::string& reason)
	{
		try
		{
			const std::uint64_t pixelCount = std::uint64_t(source.extent.width) * source.extent.height;
			if(!targets.device || !targets.allocator || !source.image || !source.view || !output.view || !output.image || targets.processingImageCount == 0
				|| source.image == output.image || pixelCount == 0 || pixelCount > std::numeric_limits<std::uint32_t>::max()
				|| source.extent.width != output.extent.width || source.extent.height != output.extent.height
				|| (source.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 || (output.usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0
				|| (source.format != VK_FORMAT_R16G16B16A16_SFLOAT && source.format != VK_FORMAT_R32G32B32A32_SFLOAT)
				|| (output.format != VK_FORMAT_R16G16B16A16_SFLOAT && output.format != VK_FORMAT_R32G32B32A32_SFLOAT))
			{
				reason = "Automatic exposure requires distinct qualified native-scale linear HDR sampled/storage targets.";
				return {};
			}
			auto pass = std::make_shared<AutoExposurePass>();
			pass->device = targets.device; pass->allocator = targets.allocator;
			pass->extent = source.extent; pass->output = output.image;
			if(!CreateBuffer(pass->allocator, pass->histogram, histogramBytes, false)
				|| !CreateBuffer(pass->allocator, pass->previousState, stateBytes, true)
				|| !CreateBuffer(pass->allocator, pass->nextState, stateBytes, true))
			{ reason = "Automatic exposure private buffer preparation failed."; return {}; }
			const std::array<VkDescriptorType, 5> types{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
			std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
			for(std::uint32_t index = 0; index < bindings.size(); ++index)
			{ bindings[index].binding = index; bindings[index].descriptorType = types[index]; bindings[index].descriptorCount = 1; bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
			VkDescriptorSetLayoutCreateInfo layoutCreate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			layoutCreate.bindingCount = static_cast<std::uint32_t>(bindings.size()); layoutCreate.pBindings = bindings.data();
			if(vkCreateDescriptorSetLayout(pass->device, &layoutCreate, nullptr, &pass->descriptorLayout) != VK_SUCCESS)
			{ reason = "Automatic exposure descriptor layout preparation failed."; return {}; }
			const std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
			VkDescriptorPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			poolCreate.maxSets = 1; poolCreate.poolSizeCount = static_cast<std::uint32_t>(sizes.size()); poolCreate.pPoolSizes = sizes.data();
			if(vkCreateDescriptorPool(pass->device, &poolCreate, nullptr, &pass->descriptorPool) != VK_SUCCESS)
			{ reason = "Automatic exposure descriptor pool preparation failed."; return {}; }
			VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocate.descriptorPool = pass->descriptorPool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &pass->descriptorLayout;
			if(vkAllocateDescriptorSets(pass->device, &allocate, &pass->descriptor) != VK_SUCCESS)
			{ reason = "Automatic exposure descriptor allocation failed."; return {}; }
			const VkDescriptorImageInfo sourceInfo{VK_NULL_HANDLE,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			const VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL};
			const std::array<VkDescriptorBufferInfo,3> bufferInfo{{{pass->histogram.handle,0,histogramBytes},{pass->previousState.handle,0,stateBytes},{pass->nextState.handle,0,stateBytes}}};
			std::array<VkWriteDescriptorSet,5> writes{};
			for(std::uint32_t index = 0; index < writes.size(); ++index)
			{
				writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[index].dstSet = pass->descriptor;
				writes[index].dstBinding = index; writes[index].descriptorType = types[index]; writes[index].descriptorCount = 1;
				if(index == 0) writes[index].pImageInfo = &sourceInfo;
				else if(index == 4) writes[index].pImageInfo = &outputInfo;
				else writes[index].pBufferInfo = &bufferInfo[index - 1u];
			}
			vkUpdateDescriptorSets(pass->device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
			const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ResolveConstants)};
			VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &pass->descriptorLayout;
			pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &push;
			if(vkCreatePipelineLayout(pass->device, &pipelineLayout, nullptr, &pass->pipelineLayout) != VK_SUCCESS
				|| !CreateComputePipeline(pass->device,pass->pipelineLayout,"Shaders/PostProcess/ExposureHistogram.comp.spv","BuildHistogram",pass->histogramPipeline)
				|| !CreateComputePipeline(pass->device,pass->pipelineLayout,"Shaders/PostProcess/ExposureResolve.comp.spv","ResolveExposure",pass->resolvePipeline)
				|| !CreateComputePipeline(pass->device,pass->pipelineLayout,output.format == VK_FORMAT_R32G32B32A32_SFLOAT
					? "Shaders/PostProcess/ExposureApply32.comp.spv" : "Shaders/PostProcess/ExposureApply16.comp.spv","ApplyExposure",pass->applicationPipeline))
			{ reason = "Installed automatic exposure compute pipelines are unavailable."; return {}; }
			reason.clear();
			return pass;
		}
		catch(const std::exception& exception)
		{ reason = std::string("Automatic exposure preparation failed: ") + exception.what(); return {}; }
	}

	void RecordAutoExposurePass(VkCommandBuffer command, AutoExposurePass& pass,
		const GraphicsSettings::PostProcessColorSettings& settings, float deltaSeconds, bool reset)
	{
		const bool automatic = settings.exposure == GraphicsSettings::ExposureMode::Automatic;
		constexpr VkAccessFlags priorAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		constexpr VkPipelineStageFlags priorStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT;
		// These barriers also order previous-frame readers before buffer reuse.
		// No per-recording CPU history index or initialization flag is modified.
		BufferBarrier(command,pass.previousState,priorAccess,VK_ACCESS_SHADER_READ_BIT,priorStages,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		if(automatic)
		{
			BufferBarrier(command,pass.histogram,priorAccess,VK_ACCESS_TRANSFER_WRITE_BIT,priorStages,VK_PIPELINE_STAGE_TRANSFER_BIT);
			vkCmdFillBuffer(command,pass.histogram.handle,0,histogramBytes,0);
			BufferBarrier(command,pass.histogram,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			const HistogramConstants histogram{pass.extent.width,pass.extent.height,-12.0f,16.0f};
			BindCompute(command,pass.histogramPipeline,pass.pipelineLayout,pass.descriptor,&histogram,sizeof(histogram));
			vkCmdDispatch(command,(pass.extent.width+15u)/16u,(pass.extent.height+15u)/16u,1);
			BufferBarrier(command,pass.histogram,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			BufferBarrier(command,pass.nextState,priorAccess,VK_ACCESS_SHADER_WRITE_BIT,priorStages,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			const ResolveConstants resolve{-12.0f,16.0f,settings.minEV,settings.maxEV,0.02f,0.98f,0.18f,deltaSeconds,
				settings.brightenSpeed,settings.darkenSpeed,1u,reset ? 1u : 0u};
			BindCompute(command,pass.resolvePipeline,pass.pipelineLayout,pass.descriptor,&resolve,sizeof(resolve));
			vkCmdDispatch(command,1,1,1);
			BufferBarrier(command,pass.nextState,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT);
		}
		else
		{
			// If the adapter is used for a fixed transition, invalidate both state
			// buffers on the GPU. Metering itself stays fully opt-in.
			for(const auto* buffer : {&pass.previousState,&pass.nextState})
			{
				BufferBarrier(command,*buffer,priorAccess,VK_ACCESS_TRANSFER_WRITE_BIT,priorStages,VK_PIPELINE_STAGE_TRANSFER_BIT);
				vkCmdFillBuffer(command,buffer->handle,0,stateBytes,0);
				BufferBarrier(command,*buffer,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			}
		}
		// Every pixel is overwritten. Discard the prior output layout rather than
		// speculatively tracking a layout for a command that may never submit.
		VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		image.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image.image = pass.output; image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
		const ApplicationConstants application{pass.extent.width,pass.extent.height,settings.compensationEV,automatic ? 1u : 0u};
		BindCompute(command,pass.applicationPipeline,pass.pipelineLayout,pass.descriptor,&application,sizeof(application));
		vkCmdDispatch(command,(pass.extent.width+7u)/8u,(pass.extent.height+7u)/8u,1);
		image.oldLayout = VK_IMAGE_LAYOUT_GENERAL; image.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; image.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0,0,nullptr,0,nullptr,1,&image);
		if(automatic)
		{
			// Resolve has finished reading old history before this copy overwrites
			// it. Advancing history is therefore accepted GPU work, never CPU intent.
			BufferBarrier(command,pass.previousState,priorAccess,VK_ACCESS_TRANSFER_WRITE_BIT,priorStages,VK_PIPELINE_STAGE_TRANSFER_BIT);
			const VkBufferCopy copy{0,0,stateBytes};
			vkCmdCopyBuffer(command,pass.nextState.handle,pass.previousState.handle,1,&copy);
			BufferBarrier(command,pass.previousState,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
	}
}
