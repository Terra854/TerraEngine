// Current-frame MAX pyramid ownership; no task-shader/history state is changed.
#include "VulkanPostProcessScreenSpaceHiZ.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	struct ScreenSpaceLightingHiZ
	{
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=nullptr;
		ScreenSpaceInputImage source,image;
		VmaAllocation allocation=nullptr;
		std::array<VkImageView,32> mipViews{};
		std::array<VkDescriptorSet,32> sets{};
		VkDescriptorPool pool=VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
		VkPipelineLayout layout=VK_NULL_HANDLE;
		VkPipeline pipeline=VK_NULL_HANDLE;
		~ScreenSpaceLightingHiZ()
		{
			// The enclosing pass's existing accepted-submit/fence gate proves idle.
			if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
			if(pool)vkDestroyDescriptorPool(device,pool,nullptr);
			if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
			if(descriptors)vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
			for(auto view:mipViews)if(view)vkDestroyImageView(device,view,nullptr);
			if(image.view)vkDestroyImageView(device,image.view,nullptr);
			if(image.image)vmaDestroyImage(allocator,image.image,allocation);
		}
	};
	namespace
	{
		struct Constants {std::uint32_t input[2],output[2],occlusionState,invalidReason,reserved[2];};
		static_assert(sizeof(Constants)==32&&offsetof(Constants,occlusionState)==16&&offsetof(Constants,reserved)==24);
		void Require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
		void Check(VkResult result,const char* reason){Require(result==VK_SUCCESS,reason);}
		VkExtent2D Extent(VkExtent2D extent,std::uint32_t mip)noexcept{return {std::max(1u,extent.width>>mip),std::max(1u,extent.height>>mip)};}
		bool SameDepth(const ScreenSpaceInputImage& a,const ScreenSpaceInputImage& b)noexcept
		{
			return a.image==b.image&&a.view==b.view&&a.format==b.format&&a.usage==b.usage&&a.samples==b.samples
				&&a.mipLevels==b.mipLevels&&a.extent.width==b.extent.width&&a.extent.height==b.extent.height&&a.owner==b.owner;
		}
		void Module(std::span<const std::uint32_t> words)
		{
			Require(words.size()>=5&&words[0]==0x07230203u,"Same-frame Hi-Z shader has no actual SPIR-V header.");
			std::map<std::uint32_t,std::uint32_t> bindings,sets,variables,storage,pointers,sampled,formats,floatWidths,imageScalars,constants;
			std::map<std::uint32_t,std::map<std::uint32_t,std::uint32_t>> offsets;
			std::map<std::uint32_t,std::array<std::uint32_t,3>> local,localIds;
			std::set<std::uint32_t> functions;std::uint32_t entry=0,push=0,open=0;
			for(std::size_t i=5;i<words.size();)
			{
				const auto count=words[i]>>16,op=words[i]&65535u;Require(count&&count<=words.size()-i,"Same-frame Hi-Z SPIR-V instruction is truncated.");
				if(op==15){Require(count>=5&&words[i+1]==5&&!entry,"Same-frame Hi-Z needs one compute entry.");const auto* name=reinterpret_cast<const char*>(&words[i+3]);Require(std::memchr(name,0,(count-3)*4)&&std::strcmp(name,"main")==0,"Same-frame Hi-Z entry is not main.");entry=words[i+2];}
				if(op==16&&count==6&&words[i+2]==17)local[words[i+1]]={words[i+3],words[i+4],words[i+5]};
				if(op==331&&count==6&&words[i+2]==38)localIds[words[i+1]]={words[i+3],words[i+4],words[i+5]};
				if(op==43&&count==4)constants[words[i+2]]=words[i+3];
				if(op==71&&count==4&&words[i+2]==33)bindings[words[i+1]]=words[i+3];
				if(op==71&&count==4&&words[i+2]==34)sets[words[i+1]]=words[i+3];
				if(op==72&&count==5&&words[i+3]==35)offsets[words[i+1]][words[i+2]]=words[i+4];
				if(op==59&&count>=4){variables[words[i+2]]=words[i+1];storage[words[i+2]]=words[i+3];if(words[i+3]==9){Require(!push,"Same-frame Hi-Z has multiple push blocks.");push=words[i+1];}}
				if(op==32&&count==4)pointers[words[i+1]]=words[i+3];
				if(op==22&&count==3)floatWidths[words[i+1]]=words[i+2];
				if(op==25&&count>=9){Require(words[i+3]==1&&!words[i+5]&&!words[i+6],"Same-frame Hi-Z image must be nonarray single-sample 2D.");imageScalars[words[i+1]]=words[i+2];sampled[words[i+1]]=words[i+7];formats[words[i+1]]=words[i+8];}
				if(op==54){Require(count==5&&!open,"Same-frame Hi-Z function is malformed.");open=1;functions.insert(words[i+2]);}
				if(op==56){Require(count==1&&open,"Same-frame Hi-Z function end is malformed.");open=0;}
				i+=count;
			}
			if(localIds.contains(entry)){std::array<std::uint32_t,3> size{};for(unsigned i=0;i<3;++i){Require(constants.contains(localIds.at(entry)[i]),"Same-frame Hi-Z workgroup specialization is unsupported.");size[i]=constants.at(localIds.at(entry)[i]);}local[entry]=size;}
			Require(entry&&!open&&functions.contains(entry)&&local.contains(entry)&&local.at(entry)==std::array<std::uint32_t,3>{8,8,1}
				&&bindings.size()==2&&pointers.contains(push),"Same-frame Hi-Z entry/workgroup/descriptor contract differs.");
			for(std::uint32_t binding=0;binding<2;++binding)
			{
				const auto variable=std::find_if(bindings.begin(),bindings.end(),[&](const auto& item){return item.second==binding;});
				Require(variable!=bindings.end()&&sets.contains(variable->first)&&sets.at(variable->first)==0&&variables.contains(variable->first)
					&&pointers.contains(variables.at(variable->first))&&storage.at(variable->first)==0,"Same-frame Hi-Z descriptor set/type is invalid.");
				const auto type=pointers.at(variables.at(variable->first));
				Require(sampled.contains(type)&&sampled.at(type)==(binding?2u:1u)&&imageScalars.contains(type)
					&&floatWidths.contains(imageScalars.at(type))&&floatWidths.at(imageScalars.at(type))==32
					&&(!binding||formats.at(type)==3),"Same-frame Hi-Z sampled/storage float/R32f ABI differs.");
			}
			const auto type=pointers.at(push);Require(offsets.contains(type)&&offsets.at(type).size()==5,"Same-frame Hi-Z push block member count differs.");
			constexpr std::array<std::uint32_t,5> wanted{0,8,16,20,24};for(std::uint32_t i=0;i<wanted.size();++i)Require(offsets.at(type).contains(i)&&offsets.at(type).at(i)==wanted[i],"Same-frame Hi-Z push offset differs.");
		}
	}
	std::shared_ptr<ScreenSpaceLightingHiZ> PrepareScreenSpaceLightingHiZ(const ScreenSpaceLightingHiZCreateInfo& info,std::string& reason)noexcept
	{
		try
		{
			const auto& depth=info.depth;Require(info.physicalDevice&&info.device&&info.allocator&&depth.image&&depth.view
				&&depth.extent.width&&depth.extent.height&&depth.samples==VK_SAMPLE_COUNT_1_BIT&&depth.mipLevels==1
				&&(depth.format==VK_FORMAT_D32_SFLOAT||depth.format==VK_FORMAT_D16_UNORM)&&(depth.usage&VK_IMAGE_USAGE_SAMPLED_BIT)
				&&depth.owner.lifecycle.rendererSessionGeneration&&depth.owner.lifecycle.backendResourceGeneration
				&&depth.owner.view.lifetimeGeneration&&depth.owner.sceneTargetGeneration,"Same-frame Hi-Z needs the exact retained standard-Z depth/view allocation.");
			Module(info.shader);
			auto owner=std::make_shared<ScreenSpaceLightingHiZ>();owner->device=info.device;owner->allocator=info.allocator;owner->source=depth;
			auto& image=owner->image;image.owner=depth.owner;image.extent=depth.extent;image.format=VK_FORMAT_R32_SFLOAT;image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
			for(auto edge=std::max(image.extent.width,image.extent.height);edge>1;edge>>=1)++image.mipLevels;
			Require(image.mipLevels<=owner->mipViews.size(),"Same-frame Hi-Z mip count exceeds its bounded descriptor owner.");
			VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(info.physicalDevice,image.format,&features);
			Require((features.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))==(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),"Same-frame Hi-Z R32F sampled/storage is unavailable.");
			VkImageFormatProperties formats{};Check(vkGetPhysicalDeviceImageFormatProperties(info.physicalDevice,image.format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,image.usage,0,&formats),"Same-frame Hi-Z exact format usage is unavailable.");
			Require(image.extent.width<=formats.maxExtent.width&&image.extent.height<=formats.maxExtent.height&&formats.maxExtent.depth>=1
				&&image.mipLevels<=formats.maxMipLevels&&formats.maxArrayLayers>=1&&(formats.sampleCounts&VK_SAMPLE_COUNT_1_BIT),"Same-frame Hi-Z full mip allocation exceeds native limits.");
			std::uint64_t bytes=0;for(std::uint32_t mip=0;mip<image.mipLevels;++mip){const auto extent=Extent(image.extent,mip);Require(std::uint64_t(extent.width)<=std::numeric_limits<std::uint64_t>::max()/extent.height/4,"Same-frame Hi-Z mip byte extent overflows.");const auto size=std::uint64_t(extent.width)*extent.height*4;Require(size<=std::numeric_limits<std::uint64_t>::max()-bytes,"Same-frame Hi-Z complete mip bytes overflow.");bytes+=size;}
			Require(bytes<=formats.maxResourceSize,"Same-frame Hi-Z complete mip byte size exceeds native limits.");
			VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(info.physicalDevice,&properties);const auto& limits=properties.limits;
			Require(limits.maxPushConstantsSize>=32&&limits.maxComputeWorkGroupInvocations>=64&&limits.maxComputeWorkGroupSize[0]>=8
				&&limits.maxComputeWorkGroupSize[1]>=8&&limits.maxComputeWorkGroupSize[2]>=1
				&&(std::uint64_t(image.extent.width)+7)/8<=limits.maxComputeWorkGroupCount[0]
				&&(std::uint64_t(image.extent.height)+7)/8<=limits.maxComputeWorkGroupCount[1]&&limits.maxComputeWorkGroupCount[2]>=1
				&&limits.maxPerStageDescriptorSampledImages>=1&&limits.maxDescriptorSetSampledImages>=1
				&&limits.maxPerStageDescriptorStorageImages>=1&&limits.maxDescriptorSetStorageImages>=1,"Same-frame Hi-Z compute limits are insufficient.");
			VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};create.imageType=VK_IMAGE_TYPE_2D;create.format=image.format;create.extent={image.extent.width,image.extent.height,1};create.mipLevels=image.mipLevels;create.arrayLayers=1;create.samples=VK_SAMPLE_COUNT_1_BIT;create.tiling=VK_IMAGE_TILING_OPTIMAL;create.usage=image.usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_GPU_ONLY;Check(vmaCreateImage(info.allocator,&create,&allocation,&image.image,&owner->allocation,nullptr),"Same-frame Hi-Z image allocation failed.");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,image.mipLevels,0,1};Check(vkCreateImageView(info.device,&view,nullptr,&image.view),"Same-frame Hi-Z full sampled view failed.");
			for(std::uint32_t mip=0;mip<image.mipLevels;++mip){view.subresourceRange.baseMipLevel=mip;view.subresourceRange.levelCount=1;Check(vkCreateImageView(info.device,&view,nullptr,&owner->mipViews[mip]),"Same-frame Hi-Z individual mip view failed.");}
			const std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
			VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptors.bindingCount=2;descriptors.pBindings=bindings.data();Check(vkCreateDescriptorSetLayout(info.device,&descriptors,nullptr,&owner->descriptors),"Same-frame Hi-Z descriptor layout failed.");
			VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,32};VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&owner->descriptors;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&range;Check(vkCreatePipelineLayout(info.device,&layout,nullptr,&owner->layout),"Same-frame Hi-Z pipeline layout failed.");
			VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=info.shader.size_bytes();shader.pCode=info.shader.data();VkShaderModule module=VK_NULL_HANDLE;Check(vkCreateShaderModule(info.device,&shader,nullptr,&module),"Same-frame Hi-Z shader module failed.");
			struct ModuleOwner{VkDevice device;VkShaderModule module;~ModuleOwner(){vkDestroyShaderModule(device,module,nullptr);}}moduleOwner{info.device,module};
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=owner->layout;pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=module;pipeline.stage.pName="main";Check(vkCreateComputePipelines(info.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&owner->pipeline),"Same-frame Hi-Z compute pipeline failed.");
			const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,image.mipLevels},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,image.mipLevels}}};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=image.mipLevels;pool.poolSizeCount=2;pool.pPoolSizes=sizes.data();Check(vkCreateDescriptorPool(info.device,&pool,nullptr,&owner->pool),"Same-frame Hi-Z descriptor pool failed.");
			std::array<VkDescriptorSetLayout,32> layouts{};layouts.fill(owner->descriptors);VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=owner->pool;allocate.descriptorSetCount=image.mipLevels;allocate.pSetLayouts=layouts.data();Check(vkAllocateDescriptorSets(info.device,&allocate,owner->sets.data()),"Same-frame Hi-Z mip descriptor allocation failed.");
			for(std::uint32_t mip=0;mip<image.mipLevels;++mip){const std::array<VkDescriptorImageInfo,2> images{{{VK_NULL_HANDLE,mip?owner->mipViews[mip-1]:depth.view,mip?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,owner->mipViews[mip],VK_IMAGE_LAYOUT_GENERAL}}};std::array<VkWriteDescriptorSet,2> writes{};for(std::uint32_t i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=owner->sets[mip];writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&images[i];}vkUpdateDescriptorSets(info.device,2,writes.data(),0,nullptr);}
			reason.clear();return owner;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Optional same-frame Hi-Z preparation failed.";}catch(...){}return {};}
	}
	const ScreenSpaceInputImage& DescribeScreenSpaceLightingHiZ(const ScreenSpaceLightingHiZ& owner)noexcept{return owner.image;}
	bool CanRecordScreenSpaceLightingHiZ(VkCommandBuffer command,const ScreenSpaceLightingHiZ& owner,const ScreenSpaceGeometryRecordedUse& geometry)noexcept
	{
		if(!command||geometry.command!=command||!geometry.frameId||geometry.owner!=owner.source.owner
			||geometry.samples!=VK_SAMPLE_COUNT_1_BIT||geometry.variableRateShading||!SameDepth(owner.source,geometry.depth)
			||!owner.image.image||!owner.image.view||!owner.pipeline||!owner.layout||!owner.image.mipLevels)return false;
		for(std::uint32_t mip=0;mip<owner.image.mipLevels;++mip)if(!owner.sets[mip]||!owner.mipViews[mip])return false;
		return true;
	}
	bool RecordScreenSpaceLightingHiZ(VkCommandBuffer command,const ScreenSpaceLightingHiZ& owner,const ScreenSpaceGeometryRecordedUse& geometry,
		ScreenSpaceHiZRecordedUse& recorded,std::uint32_t& dispatches)noexcept
	{
		if(!CanRecordScreenSpaceLightingHiZ(command,owner,geometry))return false;
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=owner.image.image;barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,owner.image.mipLevels,0,1};
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,owner.pipeline);
		for(std::uint32_t mip=0;mip<owner.image.mipLevels;++mip)
		{
			const auto source=mip?Extent(owner.image.extent,mip-1):owner.source.extent;const auto destination=Extent(owner.image.extent,mip);
			// Occlusion-history fields remain zero: current lighting acceleration
			// does not qualify any previous-frame task shader visibility state.
			const Constants constants{{source.width,source.height},{destination.width,destination.height},0,0,{0,0}};
			vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,owner.layout,0,1,&owner.sets[mip],0,nullptr);
			vkCmdPushConstants(command,owner.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),&constants);
			vkCmdDispatch(command,(destination.width+7)/8,(destination.height+7)/8,1);
			barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.subresourceRange.baseMipLevel=mip;barrier.subresourceRange.levelCount=1;
			vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		}
		ScreenSpaceHiZRecordedUse proof;proof.command=command;proof.frameId=geometry.frameId;proof.image=owner.image;proof.image.producedFrameId=geometry.frameId;
		proof.state={VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT};recorded=proof;dispatches=owner.image.mipLevels;return true;
	}
}
