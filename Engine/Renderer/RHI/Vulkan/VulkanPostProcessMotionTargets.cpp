#include "VulkanPostProcessMotionTargets.h"

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		constexpr VkFormat kMotionFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
		constexpr VkImageUsageFlags kMotionUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		bool CreateMotionImage(MotionRasterTargets& owner, PostProcessTargetImage& image)
		{
			image.format = kMotionFormat;
			image.usage = kMotionUsage;
			image.extent = owner.extent;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = image.format;
			info.extent = {image.extent.width, image.extent.height, 1u};
			info.mipLevels = info.arrayLayers = 1u;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = image.usage;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{};
			allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			if(vmaCreateImage(owner.allocator, &info, &allocation, &image.image, &image.allocation, nullptr) != VK_SUCCESS)
				return false;
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = image.format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
			return vkCreateImageView(owner.device, &view, nullptr, &image.view) == VK_SUCCESS;
		}
	}

	MotionRasterTargets::~MotionRasterTargets()
	{
		if(framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if(renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
		for(auto* image : {&totalMotion, &cameraMotion})
		{
			if(image->view) vkDestroyImageView(device, image->view, nullptr);
			if(image->image) vmaDestroyImage(allocator, image->image, image->allocation);
		}
	}

	std::shared_ptr<MotionRasterTargets> PrepareMotionRasterTargets(
		VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
		VkImageView depthView, VkFormat depthFormat, VkExtent2D extent,
		bool cameraMotion, std::string& reason)
	{
		if(!physicalDevice || !device || !allocator || !depthView || extent.width == 0u || extent.height == 0u
			|| (depthFormat != VK_FORMAT_D32_SFLOAT && depthFormat != VK_FORMAT_D16_UNORM))
		{
			reason = "Motion raster requires one current single-sample depth-only surface.";
			return {};
		}
		VkPhysicalDeviceProperties limits{};
		vkGetPhysicalDeviceProperties(physicalDevice, &limits);
		VkImageFormatProperties imageSupport{};
		if(vkGetPhysicalDeviceImageFormatProperties(physicalDevice, kMotionFormat, VK_IMAGE_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL, kMotionUsage, 0, &imageSupport) != VK_SUCCESS
			|| extent.width > imageSupport.maxExtent.width || extent.height > imageSupport.maxExtent.height
			|| extent.width > limits.limits.maxFramebufferWidth || extent.height > limits.limits.maxFramebufferHeight
			|| (imageSupport.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0u)
		{
			reason = "The selected device cannot allocate sampled motion color attachments at this view extent.";
			return {};
		}
		auto result = std::make_shared<MotionRasterTargets>();
		result->device = device;
		result->allocator = allocator;
		result->extent = extent;
		result->sourceDepth = depthView;
		result->usesCameraMotion = cameraMotion;
		if(!CreateMotionImage(*result, result->totalMotion) || (cameraMotion && !CreateMotionImage(*result, result->cameraMotion)))
		{
			reason = "The required motion images or their views could not be allocated.";
			return {};
		}
		const std::uint32_t colorCount = cameraMotion ? 2u : 1u;
		std::array<VkAttachmentDescription, 3> attachments{};
		std::array<VkAttachmentReference, 2> colors{};
		for(std::uint32_t index = 0; index < colorCount; ++index)
		{
			attachments[index] = {0, kMotionFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			colors[index] = {index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		}
		// Opaque depth is loaded and read through the exact same sample count.
		// There are no depth writes from the velocity variant or its pipeline.
		attachments[colorCount] = {0, depthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		const VkAttachmentReference depth{colorCount, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = colorCount;
		subpass.pColorAttachments = colors.data();
		subpass.pDepthStencilAttachment = &depth;
		constexpr auto depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		constexpr auto motionStages = depthStages | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		constexpr auto sampledStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT;
		constexpr auto sampledAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		const std::array dependencies{
			VkSubpassDependency{VK_SUBPASS_EXTERNAL, 0u, motionStages | sampledStages, motionStages,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | sampledAccess,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0u},
			VkSubpassDependency{0u, VK_SUBPASS_EXTERNAL, motionStages, motionStages | sampledStages,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | sampledAccess, 0u}};
		VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass.attachmentCount = colorCount + 1u;
		pass.pAttachments = attachments.data();
		pass.subpassCount = 1u;
		pass.pSubpasses = &subpass;
		pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
		pass.pDependencies = dependencies.data();
		if(vkCreateRenderPass(device, &pass, nullptr, &result->renderPass) != VK_SUCCESS)
		{
			reason = "The depth-preserving motion render pass could not be prepared.";
			return {};
		}
		const std::array views{result->totalMotion.view, cameraMotion ? result->cameraMotion.view : depthView, depthView};
		VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebuffer.renderPass = result->renderPass;
		framebuffer.attachmentCount = colorCount + 1u;
		framebuffer.pAttachments = views.data();
		framebuffer.width = extent.width;
		framebuffer.height = extent.height;
		framebuffer.layers = 1u;
		if(vkCreateFramebuffer(device, &framebuffer, nullptr, &result->framebuffer) != VK_SUCCESS)
		{
			reason = "The motion framebuffer could not borrow the current opaque depth view.";
			return {};
		}
		reason.clear();
		return result;
	}

	void BeginMotionRasterPass(VkCommandBuffer command, const MotionRasterTargets& targets) noexcept
	{
		const std::array<VkClearValue, 2> clearInvalid{};
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = targets.renderPass;
		begin.framebuffer = targets.framebuffer;
		begin.renderArea.extent = targets.extent;
		begin.clearValueCount = targets.usesCameraMotion ? 2u : 1u;
		begin.pClearValues = clearInvalid.data();
		vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
	}
}
