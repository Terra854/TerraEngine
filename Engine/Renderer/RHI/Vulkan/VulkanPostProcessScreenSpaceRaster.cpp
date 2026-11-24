#include "VulkanPostProcessScreenSpaceRaster.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace TerraEngine::Renderer::Vulkan
{
	ScreenSpaceRasterResources::~ScreenSpaceRasterResources()
	{
		// The parent retirement boundary establishes real GPU completion first.
		// Framebuffers borrow every view, so they must disappear before images.
		if(device && framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if(device && renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
		for(std::size_t index = 0; index < auxiliaryAllocations.size(); ++index)
		{
			auto& image = colors[index + 2];
			if(device && image.view) vkDestroyImageView(device, image.view, nullptr);
			if(allocator && image.image && auxiliaryAllocations[index])
				vmaDestroyImage(allocator, image.image, auxiliaryAllocations[index]);
		}
	}
	namespace
	{
		void Require(bool valid, const char* reason) { if(!valid) throw std::runtime_error(reason); }
		void Check(VkResult status, const char* reason) { Require(status == VK_SUCCESS, reason); }
		bool Hdr(VkFormat format) noexcept
		{
			return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT;
		}
		bool SameExtent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool Qualifies(VkPhysicalDevice physical, const ScreenSpaceInputImage& image)
		{
			VkFormatFeatureFlags features = 0;
			if(image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if(image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if(image.usage & VK_IMAGE_USAGE_STORAGE_BIT) features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if(image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
			if(image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) features |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			if((format.optimalTilingFeatures & features) != features) return false;
			VkImageFormatProperties limits{};
			if(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, image.usage, 0, &limits) != VK_SUCCESS) return false;
			const std::uint64_t bytesPerPixel = image.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u
				: image.format == VK_FORMAT_D16_UNORM ? 2u : image.format == VK_FORMAT_D32_SFLOAT ? 4u : 16u;
			const auto pixels = std::uint64_t{image.extent.width} * image.extent.height;
			if(pixels > std::numeric_limits<std::uint64_t>::max() / bytesPerPixel
				|| pixels * bytesPerPixel > limits.maxResourceSize) return false;
			return image.extent.width && image.extent.height && image.extent.width <= limits.maxExtent.width
				&& image.extent.height <= limits.maxExtent.height && limits.maxExtent.depth >= 1
				&& limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1
				&& (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
		}
		VkAttachmentDescription ColorAttachment(VkFormat format, VkImageLayout finalLayout)
		{
			VkAttachmentDescription attachment{};
			attachment.format = format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = finalLayout;
			return attachment;
		}
	}

	ScreenSpaceRasterPreparation PrepareScreenSpaceRasterResources(const ScreenSpaceRasterCreateInfo& info)
	{
		ScreenSpaceRasterPreparation result;
		if(!info.demand.Any()) { result.state = ScreenSpaceRasterPreparationState::Disabled; return result; }
		try
		{
			Require(info.physicalDevice && info.device && info.allocator && info.extent.width && info.extent.height,
				"Screen-space raster requires actual devices, allocator and a nonempty view extent.");
			Require(info.owner.lifecycle.rendererSessionGeneration && info.owner.lifecycle.backendResourceGeneration
				&& info.owner.view.lifetimeGeneration && info.owner.sceneTargetGeneration,
				"Screen-space raster requires a retained native view lifetime and scene target generation.");
			Require(Hdr(info.sceneColor.format) && info.identity.format == VK_FORMAT_R32G32B32A32_UINT,
				"Screen-space raster must borrow the exact HDR scene and uint4 identity format pair.");
			Require(info.depth.format == VK_FORMAT_D32_SFLOAT || info.depth.format == VK_FORMAT_D16_UNORM,
				"Screen-space raster needs a qualified standard-Z depth aspect; stencil depth routes are unsupported.");
			const std::array<const ScreenSpaceInputImage*, 3> borrowed{&info.sceneColor, &info.identity, &info.depth};
			for(const auto* image : borrowed)
			{
				Require(image->image && image->view && image->owner == info.owner && SameExtent(image->extent, info.extent)
					&& image->samples == VK_SAMPLE_COUNT_1_BIT && image->mipLevels == 1,
					"Screen-space raster attachments must match the exact view, extent and single-sample mip.");
				Require(Qualifies(info.physicalDevice, *image), "A borrowed screen-space attachment's exact format/usage/extent is unsupported.");
			}
			Require((info.sceneColor.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
				== (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
				&& (info.identity.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
				&& (info.depth.usage & (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
				== (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
				"Borrowed HDR/depth must be readable and every raster attachment must carry its real attachment usage.");
			for(std::size_t a = 0; a < borrowed.size(); ++a) for(std::size_t b = a + 1; b < borrowed.size(); ++b)
				Require(borrowed[a]->image != borrowed[b]->image && borrowed[a]->view != borrowed[b]->view,
					"Independent HDR, identity and depth raster attachments cannot alias.");
			for(float channel : info.sceneClear.float32)
				Require(std::isfinite(channel) && (info.sceneColor.format != VK_FORMAT_R16G16B16A16_SFLOAT || std::abs(channel) <= 65504.0f),
					"Screen-space raster scene clear must be finite in the actual HDR attachment format.");
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(info.physicalDevice, &properties);
			Require(properties.limits.maxColorAttachments >= 7 && properties.limits.maxFragmentOutputAttachments >= 7
				&& info.extent.width <= properties.limits.maxFramebufferWidth && info.extent.height <= properties.limits.maxFramebufferHeight
				&& properties.limits.maxFramebufferLayers >= 1,
				"The fixed seven-location screen-space raster ABI exceeds physical framebuffer/output limits.");
			auto candidate = std::make_shared<ScreenSpaceRasterResources>();
			candidate->device = info.device; candidate->allocator = info.allocator; candidate->owner = info.owner;
			candidate->demand = info.demand; candidate->extent = info.extent;
			candidate->colors[0] = info.sceneColor; candidate->colors[1] = info.identity; candidate->depth = info.depth;
			for(auto& reference : candidate->colorReferences) reference = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			const bool environment = info.demand.reflections || info.demand.ambientOcclusion;
			const bool diffuse = info.demand.indirectLighting || info.demand.ambientOcclusion;
			const std::array<bool, 5> needed{true, environment, diffuse, environment, diffuse};
			for(std::size_t index = 0; index < needed.size(); ++index)
			{
				if(!needed[index]) continue;
				auto& image = candidate->colors[index + 2];
				image.owner = info.owner; image.extent = info.extent;
				image.format = index == 0 ? VK_FORMAT_R16G16B16A16_SFLOAT : info.sceneColor.format;
				image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				// Allocation alone never certifies this frame's real surface producer.
				image.producedFrameId = 0;
				Require(Qualifies(info.physicalDevice, image), "A demanded screen-space auxiliary attachment is unsupported at the actual usage/extent.");
				VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
				create.imageType = VK_IMAGE_TYPE_2D; create.format = image.format; create.extent = {info.extent.width, info.extent.height, 1};
				create.mipLevels = create.arrayLayers = 1; create.samples = VK_SAMPLE_COUNT_1_BIT;
				create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = image.usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
				VmaAllocationInfo allocated{};
				Check(vmaCreateImage(info.allocator, &create, &allocation, &image.image, &candidate->auxiliaryAllocations[index], &allocated),
					"Demanded screen-space auxiliary image allocation failed.");
				Require(allocated.size <= std::numeric_limits<std::uint64_t>::max() - candidate->allocatedBytes,
					"Screen-space raster allocation accounting overflowed.");
				candidate->allocatedBytes += allocated.size;
				vmaSetAllocationName(info.allocator, candidate->auxiliaryAllocations[index], "Current-view screen-space raster auxiliary");
				VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
				view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				Check(vkCreateImageView(info.device, &view, nullptr, &image.view), "Screen-space raster auxiliary image view failed.");
			}
			std::array<VkAttachmentDescription, 8> attachments{};
			std::array<VkImageView, 8> views{};
			for(std::size_t logical = 0; logical < candidate->colors.size(); ++logical)
			{
				const auto& image = candidate->colors[logical]; if(!image.image) continue;
				const auto physical = candidate->attachmentCount++;
				candidate->colorReferences[logical].attachment = physical;
				attachments[physical] = ColorAttachment(image.format, logical < 2 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				views[physical] = image.view;
				// Zero initializers are also typed uint4 identity and invalid normal
				// clears. Only the actual HDR clear is caller-authored.
				if(logical == 0) candidate->clearValues[physical].color = info.sceneClear;
			}
			candidate->depthAttachmentIndex = candidate->attachmentCount++;
			auto& depth = attachments[candidate->depthAttachmentIndex];
			depth = ColorAttachment(info.depth.format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			views[candidate->depthAttachmentIndex] = info.depth.view;
			candidate->clearValues[candidate->depthAttachmentIndex].depthStencil = {1.0f, 0};
			const VkAttachmentReference depthReference{candidate->depthAttachmentIndex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
			VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = static_cast<std::uint32_t>(candidate->colorReferences.size());
			subpass.pColorAttachments = candidate->colorReferences.data(); subpass.pDepthStencilAttachment = &depthReference;
			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = dependencies[0].dstStageMask;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
			VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
			pass.attachmentCount = candidate->attachmentCount; pass.pAttachments = attachments.data();
			pass.subpassCount = 1; pass.pSubpasses = &subpass;
			pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size()); pass.pDependencies = dependencies.data();
			Check(vkCreateRenderPass(info.device, &pass, nullptr, &candidate->renderPass), "Screen-space raster render pass creation failed.");
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = candidate->renderPass; framebuffer.attachmentCount = candidate->attachmentCount;
			framebuffer.pAttachments = views.data(); framebuffer.width = info.extent.width; framebuffer.height = info.extent.height; framebuffer.layers = 1;
			Check(vkCreateFramebuffer(info.device, &framebuffer, nullptr, &candidate->framebuffer), "Screen-space raster framebuffer creation failed.");
			result.resources = std::move(candidate); result.state = ScreenSpaceRasterPreparationState::PreparedResources;
		}
		catch(const std::exception& error) { result.reason = error.what(); }
		return result;
	}
}
