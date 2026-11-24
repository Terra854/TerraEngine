// Image allocation/adoption only: frame graph and actual submission stay outside.
#include "VulkanPostProcessCustomImages.h"
#include <array>
#include <limits>
#include <stdexcept>

namespace TerraEngine::Renderer::Vulkan
{
	struct CustomPostProcessImageAllocation
	{
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=nullptr;
		PostProcessTargetImage image;
		VkDescriptorPool pool=VK_NULL_HANDLE;
		std::shared_ptr<const void> infrastructureOwner;
		~CustomPostProcessImageAllocation()
		{
			// Descriptor borrowers precede image teardown. The independent supplied
			// infrastructure owner outlives every native destruction below.
			if(pool&&device)vkDestroyDescriptorPool(device,pool,nullptr);
			if(image.view&&device)vkDestroyImageView(device,image.view,nullptr);
			if(image.image&&allocator)vmaDestroyImage(allocator,image.image,image.allocation);
		}
	};
	struct CustomPostProcessImageSet
	{
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=nullptr;
		std::array<PostProcessTargetImage,3> images{};
		std::uint32_t count=0;
		bool adopted=false;
		~CustomPostProcessImageSet()
		{
			if(!adopted)return;
			// Only image/view deletion moved here. The actual outer target owns
			// all descriptors and its explicit accepted-submission retirement gate.
			for(std::uint32_t i=0;i<count;++i)
			{
				if(images[i].view)vkDestroyImageView(device,images[i].view,nullptr);
				if(images[i].image)vmaDestroyImage(allocator,images[i].image,images[i].allocation);
			}
		}
	};
	namespace
	{
		void Require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
		bool Hdr(VkFormat format) noexcept
		{
			return format==VK_FORMAT_R16G16B16A16_SFLOAT||format==VK_FORMAT_R32G32B32A32_SFLOAT;
		}
		bool ColorLayout(VkImageLayout layout,VkImageUsageFlags usage) noexcept
		{
			switch(layout)
			{
			case VK_IMAGE_LAYOUT_UNDEFINED:return true;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:return usage&VK_IMAGE_USAGE_SAMPLED_BIT;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:return usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			case VK_IMAGE_LAYOUT_GENERAL:return usage&VK_IMAGE_USAGE_STORAGE_BIT;
			default:return false;
			}
		}
		std::optional<CustomPostProcessImage> Bind(const PostProcessTargetImage& image,std::shared_ptr<const void> owner,
			const PostProcessHistoryScope& scope,std::uint64_t frameId,std::uint64_t generation,VkImageLayout layout,std::string& reason)
		{
			Require(image.image&&image.view&&image.allocation&&owner&&image.extent.width&&image.extent.height&&Hdr(image.format)
				&&generation&&scope.target.view&&scope.target.view->lifetimeGeneration&&scope.lifecycle.rendererSessionGeneration
				&&scope.lifecycle.backendResourceGeneration&&ColorLayout(layout,image.usage),"Custom image binding lacks actual retained allocation, view lifetime or legal color layout.");
			CustomPostProcessImage binding;binding.image=image.image;binding.view=image.view;binding.extent=image.extent;
			binding.format=image.format;binding.usage=image.usage;binding.layout=layout;binding.scope=scope;
			binding.frameId=frameId;binding.targetGeneration=generation;binding.owner=std::move(owner);
			reason.clear();return binding;
		}
	}
	std::shared_ptr<const CustomPostProcessImageAllocation> PrepareCustomPostProcessImage(
		const CustomPostProcessImageCreateInfo& info,std::string& reason) noexcept
	{
		try
		{
			Require(info.device&&info.physicalDevice&&info.allocator&&info.extent.width&&info.extent.height&&Hdr(info.format)
				&&info.compositeLayout&&info.sampler&&info.infrastructureOwner,"Custom HDR image needs native allocator and actual retained fullscreen descriptor infrastructure.");
			Require(info.layoutKind==CustomCompositeDescriptorLayoutKind::SampledColor||info.layoutKind==CustomCompositeDescriptorLayoutKind::SampledColorAndStorage,
				"Custom fullscreen descriptor layout shape is unknown.");
			Require(info.samplingFilter==VK_FILTER_NEAREST||info.samplingFilter==VK_FILTER_LINEAR,"Custom fullscreen sampler filter is unsupported.");
			const bool storage=info.layoutKind==CustomCompositeDescriptorLayoutKind::SampledColorAndStorage;
			VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if(storage)usage|=VK_IMAGE_USAGE_STORAGE_BIT;
			VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			if(storage)required|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if(info.samplingFilter==VK_FILTER_LINEAR)required|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
			VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(info.physicalDevice,info.format,&format);
			Require((format.optimalTilingFeatures&required)==required,"Custom HDR image format lacks its exact render/sample/transfer/storage/filter features.");
			VkImageFormatProperties supported{};
			Require(vkGetPhysicalDeviceImageFormatProperties(info.physicalDevice,info.format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&supported)==VK_SUCCESS
				&&(supported.sampleCounts&VK_SAMPLE_COUNT_1_BIT)&&supported.maxMipLevels>=1&&supported.maxArrayLayers>=1
				&&supported.maxExtent.depth>=1&&info.extent.width<=supported.maxExtent.width&&info.extent.height<=supported.maxExtent.height,
				"Custom HDR image exact format/usage/extent is unsupported.");
			const auto bytes=info.format==VK_FORMAT_R16G16B16A16_SFLOAT?8u:16u;
			Require(std::uint64_t(info.extent.width)<=std::numeric_limits<std::uint64_t>::max()/info.extent.height/bytes
				&&std::uint64_t(info.extent.width)*info.extent.height*bytes<=supported.maxResourceSize,"Custom HDR image byte extent exceeds native limits.");
			VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(info.physicalDevice,&properties);
			Require(info.extent.width<=properties.limits.maxFramebufferWidth&&info.extent.height<=properties.limits.maxFramebufferHeight
				&&properties.limits.maxColorAttachments>=1,"Custom HDR image exceeds native framebuffer extent.");
			auto owner=std::make_shared<CustomPostProcessImageAllocation>();owner->device=info.device;owner->allocator=info.allocator;
			owner->infrastructureOwner=info.infrastructureOwner;owner->image.extent=info.extent;owner->image.format=info.format;owner->image.usage=usage;
			VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=info.format;
			image.extent={info.extent.width,info.extent.height,1};image.mipLevels=1;image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
			image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_GPU_ONLY;
			Require(vmaCreateImage(info.allocator,&image,&allocation,&owner->image.image,&owner->image.allocation,nullptr)==VK_SUCCESS,"Custom HDR image allocation failed.");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=owner->image.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=info.format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
			Require(vkCreateImageView(info.device,&view,nullptr,&owner->image.view)==VK_SUCCESS,"Custom HDR image view allocation failed.");
			const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=storage?2:1;pool.pPoolSizes=sizes.data();
			Require(vkCreateDescriptorPool(info.device,&pool,nullptr,&owner->pool)==VK_SUCCESS,"Custom HDR image descriptor pool failed.");
			VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set.descriptorPool=owner->pool;set.descriptorSetCount=1;set.pSetLayouts=&info.compositeLayout;
			Require(vkAllocateDescriptorSets(info.device,&set,&owner->image.descriptor)==VK_SUCCESS,"Custom HDR image descriptor allocation failed.");
			const std::array<VkDescriptorImageInfo,2> images{{{info.sampler,owner->image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,owner->image.view,VK_IMAGE_LAYOUT_GENERAL}}};
			std::array<VkWriteDescriptorSet,2> writes{};
			for(std::uint32_t i=0;i<(storage?2u:1u);++i)
			{
				writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=owner->image.descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;
				writes[i].descriptorType=sizes[i].type;writes[i].pImageInfo=&images[i];
			}
			vkUpdateDescriptorSets(info.device,storage?2:1,writes.data(),0,nullptr);
			reason.clear();return owner;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom HDR image preparation failed.";}catch(...){}return {};}
	}
	PostProcessTargetImage DescribeCustomPostProcessImage(const std::shared_ptr<const CustomPostProcessImageAllocation>& allocation) noexcept
	{
		return allocation?allocation->image:PostProcessTargetImage{};
	}
	std::shared_ptr<const CustomPostProcessImageSet> AdoptCustomPostProcessImageSet(VkDevice device,VmaAllocator allocator,
		std::span<const PostProcessTargetImage> images,std::string& reason) noexcept
	{
		try
		{
			Require(device&&allocator&&!images.empty()&&images.size()<=3,"Custom image adoption needs one to three actual native allocations.");
			for(std::size_t i=0;i<images.size();++i)
			{
				const auto& image=images[i];Require(image.image&&image.view&&image.allocation&&image.extent.width&&image.extent.height&&Hdr(image.format),"Custom adopted image is incomplete or is not native HDR color.");
				for(std::size_t j=0;j<i;++j)Require(image.image!=images[j].image&&image.view!=images[j].view&&image.allocation!=images[j].allocation,"Custom image adoption repeats an allocation or view.");
			}
			auto owner=std::make_shared<CustomPostProcessImageSet>();owner->device=device;owner->allocator=allocator;owner->count=static_cast<std::uint32_t>(images.size());
			for(std::size_t i=0;i<images.size();++i)owner->images[i]=images[i];
			// All throwing work (including diagnostics) precedes the single transfer
			// flag. Failure cleanup has never owned or freed the caller's images.
			reason.clear();owner->adopted=true;return owner;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom native image adoption failed.";}catch(...){}return {};}
	}
	std::optional<CustomPostProcessImage> BindCustomPostProcessImage(std::shared_ptr<const CustomPostProcessImageAllocation> allocation,
		const PostProcessHistoryScope& scope,std::uint64_t frameId,std::uint64_t generation,VkImageLayout layout,std::string& reason) noexcept
	{
		try {Require(bool(allocation),"Custom image allocation owner is missing.");return Bind(allocation->image,allocation,scope,frameId,generation,layout,reason);}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom image binding failed.";}catch(...){}return {};}
	}
	std::optional<CustomPostProcessImage> BindCustomPostProcessImageSetImage(std::shared_ptr<const CustomPostProcessImageSet> images,
		std::uint32_t index,const PostProcessHistoryScope& scope,std::uint64_t frameId,std::uint64_t generation,VkImageLayout layout,std::string& reason) noexcept
	{
		try {Require(images&&images->adopted&&index<images->count,"Custom borrowed image lacks a real adopted allocation owner or index.");return Bind(images->images[index],images,scope,frameId,generation,layout,reason);}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom adopted image binding failed.";}catch(...){}return {};}
	}
}
