// Static depth owners are constructed before recording. No temporary submission
// initializes them: the real primary owns the first UNDEFINED transition.
#include "VulkanResourceSource.h"
#include <limits>
#include <cmath>

namespace
{
	bool StaticDepthFormat(VkFormat format)
	{
		// Stencil needs a separate aspect/layout contract; retain combined shadows.
		return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM
			|| format == VK_FORMAT_X8_D24_UNORM_PACK32;
	}
	VkImageCreateInfo StaticDepthInfo(const TerraEngine::Renderer::ShadowViewKey& view)
	{
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = static_cast<VkFormat>(view.depthFormat);
		info.extent = {view.extent[0], view.extent[1], 1};
		info.mipLevels = 1;
		info.arrayLayers = view.resourceKind == ShadowResourceKind::PointCube ? 6U : 1U;
		info.flags = info.arrayLayers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		return info;
	}
}

VulkanContext::Impl::StaticShadowEntry::~StaticShadowEntry()
{
	// Borrowed color views are never destroyed here. Framebuffers must go first.
	for (auto framebuffer : framebuffers) if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
	for (auto view : faceViews) if (view) vkDestroyImageView(device, view, nullptr);
	if (depth.image && depth.allocation) vmaDestroyImage(allocator, depth.image, depth.allocation);
}

bool VulkanContext::Impl::QueryStaticShadowRequirements(const TerraEngine::Renderer::ShadowViewKey& view,
	TerraEngine::Renderer::StaticShadowAdmissionRequest& result, std::string& reason)
{
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	reason.clear();
	const auto reject = [&](const char* message) { reason = message; return false; };
	if (!initialized || deviceLost || !device || !physicalDevice || !staticShadowMaintenance4Enabled
		|| !vulkan13Features.maintenance4) return reject("Static image requirements capability unavailable.");
	auto query = reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(vkGetDeviceProcAddr(device, "vkGetDeviceImageMemoryRequirements"));
	if (!query) return reject("Static image requirements entry point unavailable.");
	if (view.rendererSessionGeneration != renderSessionGeneration || view.allocationGeneration != shadowAllocationGeneration
		|| !view.extent[0] || !view.extent[1] || !StaticDepthFormat(static_cast<VkFormat>(view.depthFormat))
		|| view.depthFormat != static_cast<std::uint32_t>(depthFormat)
		|| static_cast<std::uint32_t>(view.resourceKind) > 3U || view.faceIndex != 0)
		return reject("Static depth view is unsupported or stale.");
	auto info = StaticDepthInfo(view);
	VkFormatProperties format{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, info.format, &format);
	constexpr auto required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if ((format.optimalTilingFeatures & required) != required) return reject("Static depth transfer format unavailable.");
	VkImageFormatProperties properties{};
	// Check both complete usage combinations, not independent format bits alone.
	for (auto usage : {info.usage, VkImageUsageFlags(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)}) {
		if (vkGetPhysicalDeviceImageFormatProperties(physicalDevice, info.format, info.imageType, info.tiling, usage, info.flags, &properties) != VK_SUCCESS
			|| info.extent.width > properties.maxExtent.width || info.extent.height > properties.maxExtent.height
			|| info.arrayLayers > properties.maxArrayLayers || !(properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
			return reject("Static depth image usage or extent unavailable.");
	}
	VkDeviceImageMemoryRequirements request{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
	request.pCreateInfo = &info;
	VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	requirements.pNext = &dedicated;
	query(device, &request, &requirements);
	if (!requirements.memoryRequirements.size || !requirements.memoryRequirements.memoryTypeBits)
		return reject("Static depth requirements unavailable.");
	TerraEngine::Renderer::StaticShadowAdmissionRequest value{};
	value.view = view; value.queriedDepthBytes = requirements.memoryRequirements.size;
	value.requiredFaceMask = info.arrayLayers == 6 ? 0x3f : 1;
	result = value;
	return true;
}

bool VulkanContext::Impl::EnsureStaticShadowLoadRenderPass()
{
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	if (!initialized || deviceLost || !device || !shadowRenderPass) return false;
	if (staticShadowLoadRenderPass) return true;
	// Only load/store operations differ. Bind the original shadow pipeline:
	// its attachment formats, samples and subpass references remain compatible.
			VkAttachmentDescription colorAttachment{};
			colorAttachment.format = swapchainImageFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// Shadow images are explicitly initialized and persist across frames.
			// Preserve their real attachment layout instead of discarding it again.
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = depthFormat;
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// RecordShadowPass explicitly transitions the reused depth layer back to
			// attachment layout. Keeping both layouts equal prevents an implicit
			// begin-pass transition from racing a write in an earlier queue submit.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthAttachmentRef{};
			depthAttachmentRef.attachment = 1;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;

			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			// Shadow images persist across frames. Order the previous render-pass
			// writes and fragment sampling before clearing the same attachments again;
			// queue submission order alone is not a memory dependency for these accesses.
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
			VkRenderPassCreateInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();

			if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &staticShadowLoadRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Warning, "failed to create shadow render pass");
				staticShadowLoadRenderPass = VK_NULL_HANDLE;
				return false;
			}
	return true;
}

bool VulkanContext::Impl::CreateStaticShadowEntry(const TerraEngine::Renderer::StaticShadowAdmissionRequest& reservation,
	std::span<const VkImageView> colorViews, double priority, StaticShadowEntry*& result, std::string& reason)
{
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	result = nullptr; reason.clear();
	TerraEngine::Renderer::StaticShadowAdmissionRequest queried{};
	if (!QueryStaticShadowRequirements(reservation.view, queried, reason)) return false;
	const auto reject = [&](const char* message) { reason = message; return false; };
	if (staticShadowBudgetDraining || !std::isfinite(priority) || reservation.queriedDepthBytes != queried.queriedDepthBytes
		|| reservation.queriedAuxBytes || reservation.requiredFaceMask != queried.requiredFaceMask
		|| !nextStaticShadowAllocationGeneration || nextStaticShadowAllocationGeneration == std::numeric_limits<std::uint64_t>::max())
		return reject("Static reservation is stale or admission is draining.");
	auto info = StaticDepthInfo(reservation.view);
	if (colorViews.size() != info.arrayLayers) return reject("Static color cohort is incomplete.");
	for (auto color : colorViews) if (!color) return reject("Static color view unavailable.");
	const auto charged = staticShadowLifetime.ChargedBytes();
	if (reservation.queriedDepthBytes > std::numeric_limits<std::uint64_t>::max() - charged
		|| charged + reservation.queriedDepthBytes > staticShadowEffectiveBudget)
		return reject("Static reservation exceeds effective charged budget.");
	if (!EnsureStaticShadowLoadRenderPass()) return reject("Static compatible load pass unavailable.");
	try {
		// Reserve container capacity before tracker publication; insertion then cannot allocate.
		staticShadowEntries.reserve(staticShadowEntries.size() + 1);
		auto entry = std::make_unique<StaticShadowEntry>();
		entry->device = device; entry->allocator = allocator; entry->depth = {};
		entry->finalView = reservation.view; entry->staticView = reservation.view;
		entry->borrowedColorGeneration = shadowAllocationGeneration; entry->priority = priority;
		VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		VmaAllocationInfo actual{};
		if (vmaCreateImage(allocator, &info, &allocation, &entry->depth.image, &entry->depth.allocation, &actual) != VK_SUCCESS)
			return reject("Static depth allocation failed.");
		if (!TerraEngine::Renderer::ReconcileStaticShadowAllocation(reservation, actual.size, 0,
			charged + reservation.queriedDepthBytes, staticShadowEffectiveBudget, reason)) return false;
		entry->depth.size = actual.size;
		entry->owner = {renderSessionGeneration, backendResourceGeneration,
			reinterpret_cast<std::uint64_t>(entry->depth.allocation), nextStaticShadowAllocationGeneration,
			actual.size, reservation.view.depthFormat, info.arrayLayers, reservation.view.extent};
		entry->staticView.allocationGeneration = entry->owner.allocationGeneration;
		entry->staticView.regionId = entry->owner.allocationIdentity;
		for (std::uint32_t face = 0; face < info.arrayLayers; ++face) {
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = entry->depth.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = info.format;
			view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, face, 1};
			if (vkCreateImageView(device, &view, nullptr, &entry->faceViews[face]) != VK_SUCCESS)
				return reject("Static depth face creation failed.");
			std::array<VkImageView, 2> attachments{colorViews[face], entry->faceViews[face]};
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = shadowRenderPass; framebuffer.attachmentCount = 2;
			framebuffer.pAttachments = attachments.data(); framebuffer.width = info.extent.width;
			framebuffer.height = info.extent.height; framebuffer.layers = 1;
			if (vkCreateFramebuffer(device, &framebuffer, nullptr, &entry->framebuffers[face]) != VK_SUCCESS)
				return reject("Static depth framebuffer creation failed.");
		}
		// No GPU use exists yet. Every failed earlier step rolls back through RAII.
		if (!staticShadowLifetime.RegisterAllocation(entry->owner)) return reject("Static lifetime registration failed.");
		result = entry.get(); staticShadowEntries.push_back(std::move(entry));
		++nextStaticShadowAllocationGeneration;
		return true;
	} catch (const std::bad_alloc&) { return reject("Static resource preparation allocation failed."); }
}
