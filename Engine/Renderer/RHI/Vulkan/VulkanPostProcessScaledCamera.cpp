#include "VulkanPostProcessScaledCamera.h"

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		bool MakeImage(ScaledCameraTargets& targets, VkPhysicalDevice physicalDevice,
			PostProcessTargetImage& image, VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage)
		{
			VkImageFormatProperties support{};
			if(vkGetPhysicalDeviceImageFormatProperties(physicalDevice, format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, usage, 0, &support) != VK_SUCCESS
				|| (support.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0u
				|| targets.extent.width > support.maxExtent.width || targets.extent.height > support.maxExtent.height) return false;
			image.extent = targets.extent;
			image.usage = usage;
			image.format = format;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = format;
			info.extent = {image.extent.width, image.extent.height, 1u};
			info.arrayLayers = info.mipLevels = 1u;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.usage = usage;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{};
			allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			if(vmaCreateImage(targets.allocator, &info, &allocation, &image.image, &image.allocation, nullptr) != VK_SUCCESS)
				return false;
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = format;
			view.subresourceRange = {aspect, 0u, 1u, 0u, 1u};
			return vkCreateImageView(targets.device, &view, nullptr, &image.view) == VK_SUCCESS;
		}
	}

	ScaledCameraTargets::~ScaledCameraTargets()
	{
		if(framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
		for(auto* image : {&color, &identity, &depth})
		{
			if(image->view) vkDestroyImageView(device, image->view, nullptr);
			if(image->image) vmaDestroyImage(allocator, image->image, image->allocation);
		}
	}

	std::shared_ptr<ScaledCameraTargets> PrepareScaledCameraTargets(VkPhysicalDevice physicalDevice,
		VkDevice device, VmaAllocator allocator, VkRenderPass renderPass, VkFormat colorFormat, VkFormat depthFormat,
		VkExtent2D internalExtent, std::string& reason)
	{
		if(!physicalDevice || !device || !allocator || !renderPass || !internalExtent.width || !internalExtent.height
			|| (colorFormat != VK_FORMAT_R16G16B16A16_SFLOAT && colorFormat != VK_FORMAT_R32G32B32A32_SFLOAT)
			|| (depthFormat != VK_FORMAT_D32_SFLOAT && depthFormat != VK_FORMAT_D16_UNORM))
		{
			reason = "The renderer cannot provide an internal HDR, identity and depth raster surface.";
			return {};
		}
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		if(internalExtent.width > properties.limits.maxFramebufferWidth || internalExtent.height > properties.limits.maxFramebufferHeight)
		{
			reason = "The internal camera extent exceeds the framebuffer limits.";
			return {};
		}
		auto targets = std::make_shared<ScaledCameraTargets>();
		targets->device = device;
		targets->allocator = allocator;
		targets->extent = internalExtent;
		constexpr auto colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		constexpr auto identityUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		constexpr auto depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if(!MakeImage(*targets, physicalDevice, targets->color, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, colorUsage)
			|| !MakeImage(*targets, physicalDevice, targets->identity, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, identityUsage)
			|| !MakeImage(*targets, physicalDevice, targets->depth, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, depthUsage))
		{
			reason = "Could not allocate the exact internal raster attachments.";
			return {};
		}
		const std::array views{targets->color.view, targets->identity.view, targets->depth.view};
		VkFramebufferCreateInfo create{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		create.renderPass = renderPass;
		create.attachmentCount = static_cast<std::uint32_t>(views.size());
		create.pAttachments = views.data();
		create.width = internalExtent.width;
		create.height = internalExtent.height;
		create.layers = 1u;
		if(vkCreateFramebuffer(device, &create, nullptr, &targets->framebuffer) != VK_SUCCESS)
		{
			reason = "The internal framebuffer does not match the accepted camera pass.";
			return {};
		}
		reason.clear();
		return targets;
	}

	void RecordScaledCameraInitialization(VkCommandBuffer command, const ScaledCameraTargets& targets) noexcept
	{
		if(targets.initialized) return;
		// The normal offscreen pass expects its persistent layouts on entry.
		// Seed depth to far so first-frame cluster building sees valid depth.
		const auto transition = [command](VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
			VkAccessFlags sourceAccess, VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStage, VkPipelineStageFlags targetStage) {
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = sourceAccess;
			barrier.dstAccessMask = destinationAccess;
			barrier.oldLayout = from;
			barrier.newLayout = to;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = {aspect, 0u, 1u, 0u, 1u};
			vkCmdPipelineBarrier(command, sourceStage, targetStage, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
		};
		transition(targets.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			0u, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		transition(targets.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		const VkClearDepthStencilValue farDepth{1.0f, 0u};
		const VkImageSubresourceRange depth{VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
		vkCmdClearDepthStencilImage(command, targets.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &farDepth, 1u, &depth);
		transition(targets.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
	}
}
