#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	// Framebuffers bind the first five images in this exact order. The first
	// three are rasterized at four samples. A compute pass selects sample zero
	// of Identity independently and writes the single-sample ResolvedIdentity.
	// Hi-Z reads Depth to retain coverage holes in the conservative pyramid.
	struct IndexedMsaa4Attachments
	{
		enum Index : std::size_t { Color, Identity, Depth, ResolvedColor, ResolvedDepth, ResolvedIdentity, HiZDepthSeed, Count };
		static constexpr std::size_t RasterAttachmentCount = ResolvedIdentity;
		std::array<VkFormat, Count> formats{};
		std::array<VkImageUsageFlags, Count> usages{};
		std::array<VkSampleCountFlagBits, Count> samples{};
	};

	// Describe the exact HDR color, integer identity and depth image uses. VRS,
	// geometry route and mode combinations are checked by the requesting view.
	IndexedMsaa4Attachments DescribeIndexedMsaa4Attachments(VkFormat color, VkFormat depth) noexcept;
	bool SupportsIndexedMsaa4Attachments(VkPhysicalDevice physicalDevice, VkExtent2D extent,
		const IndexedMsaa4Attachments& attachments) noexcept;

	// Build the raster subpass with color and depth resolves. The caller owns
	// the pass and records the identity resolve after rasterization.
	VkResult CreateIndexedMsaa4RenderPass(VkDevice device, const IndexedMsaa4Attachments& attachments,
		VkRenderPass& renderPass) noexcept;

	// These are copied facts from the existing protected single-sample image
	// owner. Vulkan cannot introspect a handle's creation usage/sample count.
	struct IndexedMsaa4BorrowedImage
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags usage = 0;
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		VkExtent2D extent{};
	};
	struct IndexedMsaa4Image : IndexedMsaa4BorrowedImage
	{
		VmaAllocation allocation = VK_NULL_HANDLE;
		bool owned = false;
	};
	struct IndexedMsaa4Resources
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkExtent2D extent{};
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		bool ownsRenderPass = false;
		std::array<IndexedMsaa4Image, IndexedMsaa4Attachments::Count> images{};
		IndexedMsaa4BorrowedImage identityDestination;
		VkImageLayout identityFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> descriptors{};
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		std::array<VkPipeline, 2> pipelines{};
		IndexedMsaa4Resources() = default;
		IndexedMsaa4Resources(const IndexedMsaa4Resources&) = delete;
		IndexedMsaa4Resources& operator=(const IndexedMsaa4Resources&) = delete;
		~IndexedMsaa4Resources();
	};
	struct IndexedMsaa4CreateInfo
	{
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkExtent2D extent{};
		IndexedMsaa4BorrowedImage resolvedColor, resolvedDepth, destinationIdentity;
		VkImageLayout identityFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		// A backend-owned compatible pass can be shared across view extents.
		// Ownership transfers only on SUCCESS when explicitly requested; failure
		// never destroys a supplied pass. The caller proves exact compatibility.
		VkRenderPass compatibleRenderPass = VK_NULL_HANDLE;
		bool takeRenderPassOwnership = false;
		std::span<const std::uint32_t> identityShader;
		std::span<const std::uint32_t> hiZSeedShader;
	};

	// Creates no submissions, waits or initialization commands. The caller must
	// retain the borrowed images/shared pass until actual GPU borrowers complete,
	// and destroy this owner before destroying those borrowed image views.
	std::shared_ptr<IndexedMsaa4Resources> CreateIndexedMsaa4Resources(
		const IndexedMsaa4CreateInfo& info, std::string& reason);
	// Record outside a render pass after this owner's 4x raster pass. Every
	// destination texel is overwritten, so discard transitions avoid publishing a
	// false CPU layout state when an earlier recorded command was abandoned.
	bool RecordIdentityResolve(VkCommandBuffer command, const IndexedMsaa4Resources& resources) noexcept;
	// Output is a COLOR-aspect R32_SFLOAT sampled image in SHADER_READ_ONLY.
	// Hi-Z must use that color-source path rather than a depth-aspect transition.
	bool RecordHiZDepthSeed(VkCommandBuffer command, const IndexedMsaa4Resources& resources,
		std::uint32_t occlusionState = 1, std::uint32_t invalidReason = 0) noexcept;
}
