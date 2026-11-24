// Optional current-view MRT resources for the real Default surface variant.
#pragma once
#include "VulkanPostProcessScreenSpaceInputs.h"
#include <array>

namespace TerraEngine::Renderer::Vulkan
{
	struct ScreenSpaceRasterCreateInfo
	{
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkExtent2D extent{};
		ScreenSpaceInputViewOwner owner;
		ScreenSpaceInputDemand demand;
		// These exact native scene attachments remain externally owned. Their
		// metadata must describe the same view lifetime, generation and extent.
		ScreenSpaceInputImage sceneColor, identity, depth;
		VkClearColorValue sceneClear{};
	};
	struct ScreenSpaceRasterResources
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		ScreenSpaceInputViewOwner owner;
		ScreenSpaceInputDemand demand;
		VkExtent2D extent{};
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		// Logical fragment locations never move: HDR0, uint identity1, normal2,
		// environment3, diffuse4, environment weight5, diffuse weight6. Missing
		// images have null handles and references set to VK_ATTACHMENT_UNUSED.
		std::array<ScreenSpaceInputImage, 7> colors{};
		ScreenSpaceInputImage depth;
		std::array<VkAttachmentReference, 7> colorReferences{};
		std::array<VmaAllocation, 5> auxiliaryAllocations{};
		std::array<VkClearValue, 8> clearValues{};
		std::uint32_t attachmentCount = 0;
		std::uint32_t depthAttachmentIndex = VK_ATTACHMENT_UNUSED;
		std::uint64_t allocatedBytes = 0;
		ScreenSpaceRasterResources() = default;
		ScreenSpaceRasterResources(const ScreenSpaceRasterResources&) = delete;
		ScreenSpaceRasterResources& operator=(const ScreenSpaceRasterResources&) = delete;
		~ScreenSpaceRasterResources();
	};
	enum class ScreenSpaceRasterPreparationState { Disabled, PreparedResources, Rejected };
	struct ScreenSpaceRasterPreparation
	{
		ScreenSpaceRasterPreparationState state = ScreenSpaceRasterPreparationState::Rejected;
		std::shared_ptr<ScreenSpaceRasterResources> resources;
		std::string reason;
	};
	// Allocation/preparation only; no command recording, pipeline creation,
	// submission or produced-frame stamp is manufactured here. Caller must use
	// the matching seven-location surface pipeline and preserve background clears.
	// Retain this owner AND the borrowed scene attachment owners through actual
	// submission completion before destroying or reusing their native resources.
	// Aux images end SHADER_READ_ONLY; borrowed HDR/identity end COLOR_ATTACHMENT,
	// depth ends DEPTH_STENCIL_ATTACHMENT. Caller orders their later layout changes.
	// All-disabled returns before querying a device or retaining any resources.
	ScreenSpaceRasterPreparation PrepareScreenSpaceRasterResources(const ScreenSpaceRasterCreateInfo& info);
}
