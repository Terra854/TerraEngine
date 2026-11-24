#include "VulkanPostProcessMsaa.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		bool UsesSupportedAttachments(const IndexedMsaa4Attachments& attachments) noexcept
		{
			using A = IndexedMsaa4Attachments;
			const auto color = attachments.formats[A::Color];
			const auto depth = attachments.formats[A::Depth];
			// The initial variant consumes linear HDR and depth-only sampled images.
			// Stencil resolve needs an independent policy before it can be admitted.
			if ((color != VK_FORMAT_R16G16B16A16_SFLOAT && color != VK_FORMAT_R32G32B32A32_SFLOAT)
				|| (depth != VK_FORMAT_D32_SFLOAT && depth != VK_FORMAT_D16_UNORM)) return false;
			const auto expected = DescribeIndexedMsaa4Attachments(color, depth);
			return attachments.formats == expected.formats && attachments.usages == expected.usages
				&& attachments.samples == expected.samples;
		}

		VkFormatFeatureFlags RequiredFeatures(VkImageUsageFlags usage) noexcept
		{
			VkFormatFeatureFlags result = 0;
			if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) result |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) result |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) result |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if (usage & VK_IMAGE_USAGE_STORAGE_BIT) result |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) result |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
			if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) result |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			return result;
		}
		std::uint64_t PixelBytes(VkFormat format) noexcept
		{
			switch (format) {
			case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
			case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
			case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_R32_SFLOAT: return 4;
			case VK_FORMAT_D16_UNORM: return 2;
			default: return 0;
			}
		}
		bool QualifiesImage(VkPhysicalDevice physical, VkFormat format, VkExtent2D extent,
			VkImageUsageFlags usage, VkSampleCountFlagBits samples) noexcept
		{
			if (!physical || !extent.width || !extent.height || !PixelBytes(format)) return false;
			VkFormatProperties features{}; vkGetPhysicalDeviceFormatProperties(physical, format, &features);
			const auto required = RequiredFeatures(usage);
			if ((features.optimalTilingFeatures & required) != required) return false;
			VkImageFormatProperties image{};
			if (vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, usage, 0, &image) != VK_SUCCESS || !(image.sampleCounts & samples)
				|| extent.width > image.maxExtent.width || extent.height > image.maxExtent.height
				|| image.maxExtent.depth < 1 || image.maxMipLevels < 1 || image.maxArrayLayers < 1) return false;
			const auto bytes = PixelBytes(format) * static_cast<std::uint32_t>(samples);
			const auto limit = std::numeric_limits<std::uint64_t>::max();
			return std::uint64_t(extent.width) <= limit / extent.height / bytes
				&& std::uint64_t(extent.width) * extent.height * bytes <= image.maxResourceSize;
		}
		bool SameExtent(VkExtent2D left, VkExtent2D right) noexcept { return left.width == right.width && left.height == right.height; }
		void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
		void Check(VkResult result, const char* reason) { Require(result == VK_SUCCESS, reason); }

		// The supplied installed shaders have fixed 8x8x1 workgroups. Validate
		// that local interface before calculating dispatch counts; this is not a
		// replacement for compiler/SPIR-V validation or native pipeline creation.
		bool ComputeShader8x8(std::span<const std::uint32_t> words)
		{
			if (words.size() < 5 || words[0] != 0x07230203U || words[1] < 0x00010000U
				|| words[1] > 0x00010600U || words[3] == 0 || words[4] != 0) return false;
			std::uint32_t entry = 0, entryCount = 0;
			std::unordered_map<std::uint32_t, std::uint32_t> constants;
			for (std::size_t i = 5; i < words.size();)
			{
				const auto count = words[i] >> 16U, opcode = words[i] & 0xffffU;
				if (count == 0 || count > words.size() - i) return false;
				// OpEntryPoint GLCompute, literal "main\0"; OpConstant/OpSpecConstant.
				if (opcode == 15 && count >= 5 && words[i + 1] == 5 && words[i + 3] == 0x6e69616dU && words[i + 4] == 0)
				{ entry = words[i + 2]; ++entryCount; }
				if ((opcode == 43 || opcode == 50) && count == 4) constants[words[i + 2]] = words[i + 3];
				i += count;
			}
			if (entryCount != 1) return false;
			std::uint32_t modes = 0;
			for (std::size_t i = 5; i < words.size(); i += words[i] >> 16U)
			{
				const auto count = words[i] >> 16U, opcode = words[i] & 0xffffU;
				if (count < 6 || words[i + 1] != entry) continue;
				if (opcode == 16 && words[i + 2] == 17)
				{
					if (words[i + 3] != 8 || words[i + 4] != 8 || words[i + 5] != 1) return false;
					++modes;
				}
				if (opcode == 331 && words[i + 2] == 38)
				{
					const auto x = constants.find(words[i + 3]), y = constants.find(words[i + 4]), z = constants.find(words[i + 5]);
					if (x == constants.end() || y == constants.end() || z == constants.end()
						|| x->second != 8 || y->second != 8 || z->second != 1) return false;
					++modes;
				}
			}
			return modes == 1;
		}
		struct alignas(16) DepthSeedConstants
		{
			std::array<std::uint32_t, 2> inputSize{}, outputSize{};
			std::uint32_t occlusionState = 0, invalidReason = 0;
			std::array<std::uint32_t, 2> reserved{};
		};
		static_assert(sizeof(DepthSeedConstants) == 32);
		static_assert(offsetof(DepthSeedConstants, occlusionState) == 16);
		static_assert(offsetof(DepthSeedConstants, reserved) == 24);
	}

	IndexedMsaa4Attachments DescribeIndexedMsaa4Attachments(VkFormat color, VkFormat depth) noexcept
	{
		const auto identity = VK_FORMAT_R32G32B32A32_UINT;
		constexpr auto colorAttachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		constexpr auto depthAttachment = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		constexpr auto sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
		constexpr auto transferSource = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		return {
			{color, identity, depth, color, depth, identity, VK_FORMAT_R32_SFLOAT},
			{colorAttachment, colorAttachment | sampled, depthAttachment | sampled,
				colorAttachment | sampled | transferSource, depthAttachment | sampled | transferSource,
				VK_IMAGE_USAGE_STORAGE_BIT | transferSource,
				VK_IMAGE_USAGE_STORAGE_BIT | sampled | transferSource},
			{VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_4_BIT,
				VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT}
		};
	}

	bool SupportsIndexedMsaa4Attachments(VkPhysicalDevice physicalDevice, VkExtent2D extent,
		const IndexedMsaa4Attachments& attachments) noexcept
	{
		using A = IndexedMsaa4Attachments;
		if (!physicalDevice || extent.width == 0 || extent.height == 0 || !UsesSupportedAttachments(attachments)) return false;
		VkPhysicalDeviceVulkan12Properties v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
		VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		// Vulkan 1.2 aggregates the depth/identity limits. Chaining both structs
		// would query the same properties twice and violate the pNext contract.
		properties.pNext = &v12;
		vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
		const auto& limits = properties.properties.limits;
		const auto commonSamples = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts
			& v12.framebufferIntegerColorSampleCounts & limits.sampledImageDepthSampleCounts
			& limits.sampledImageIntegerSampleCounts;
		if (limits.maxColorAttachments < 2 || extent.width > limits.maxFramebufferWidth || extent.height > limits.maxFramebufferHeight
			|| (commonSamples & VK_SAMPLE_COUNT_4_BIT) == 0
			|| (v12.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) == 0) return false;

		// The generic limit is necessary, but Vulkan may still reject any exact
		// format/usage pair or extent. Check all seven exact attachment/intermediate combinations.
		for (std::size_t i = 0; i < A::Count; ++i)
		{
			if (!QualifiesImage(physicalDevice, attachments.formats[i], extent, attachments.usages[i], attachments.samples[i])) return false;
		}
		return true;
	}

	VkResult CreateIndexedMsaa4RenderPass(VkDevice device, const IndexedMsaa4Attachments& attachments,
		VkRenderPass& renderPass) noexcept
	{
		using A = IndexedMsaa4Attachments;
		if (!device || renderPass) return VK_ERROR_INITIALIZATION_FAILED;
		if (!UsesSupportedAttachments(attachments)) return VK_ERROR_FORMAT_NOT_SUPPORTED;

		constexpr std::array<VkImageLayout, A::RasterAttachmentCount> layouts{
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		std::array<VkAttachmentDescription2, A::RasterAttachmentCount> descriptions{};
		for (std::size_t i = 0; i < descriptions.size(); ++i)
		{
			auto& description = descriptions[i];
			description.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			description.format = attachments.formats[i];
			description.samples = attachments.samples[i];
			description.loadOp = i < A::ResolvedColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			// Retain identity for the exact sample-zero compute resolve and
			// depth for Hi-Z. HDR color is resolved inside the raster pass.
			description.storeOp = i == A::Color ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
			description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			description.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			description.finalLayout = layouts[i];
		}
		const auto ref = [](A::Index i, VkImageLayout layout, VkImageAspectFlags aspect) {
			return VkAttachmentReference2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, static_cast<uint32_t>(i), layout, aspect};
		};
		const std::array color{ref(A::Color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT),
			ref(A::Identity, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT)};
		const std::array resolved{ref(A::ResolvedColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT),
			VkAttachmentReference2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, VK_ATTACHMENT_UNUSED,
				VK_IMAGE_LAYOUT_UNDEFINED, 0}};
		const auto depth = ref(A::Depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
		const auto resolvedDepth = ref(A::ResolvedDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

		// Vulkan averages float HDR color. Resolve depth and integer identity
		// explicitly from sample zero so their visibility provenance agrees.
		VkSubpassDescriptionDepthStencilResolve depthResolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
		depthResolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
		depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
		depthResolve.pDepthStencilResolveAttachment = &resolvedDepth;
		VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
		subpass.pNext = &depthResolve;
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = static_cast<uint32_t>(color.size());
		subpass.pColorAttachments = color.data();
		subpass.pResolveAttachments = resolved.data();
		subpass.pDepthStencilAttachment = &depth;

		// Resolve completion must be visible before post-process samples, picking
		// copies or overlays. The reverse dependency protects reuse of this view.
		constexpr VkPipelineStageFlags uses = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		constexpr VkPipelineStageFlags writes = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		constexpr VkAccessFlags renderAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		constexpr VkAccessFlags useAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT
			| VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | renderAccess;
		const std::array dependencies{
			VkSubpassDependency2{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, nullptr, VK_SUBPASS_EXTERNAL, 0,
				uses, writes, useAccess, renderAccess, 0, 0},
			VkSubpassDependency2{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, nullptr, 0, VK_SUBPASS_EXTERNAL,
				writes, uses, renderAccess, useAccess, 0, 0}};
		VkRenderPassCreateInfo2 create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
		create.attachmentCount = static_cast<uint32_t>(descriptions.size());
		create.pAttachments = descriptions.data();
		create.subpassCount = 1;
		create.pSubpasses = &subpass;
		create.dependencyCount = static_cast<uint32_t>(dependencies.size());
		create.pDependencies = dependencies.data();
		VkRenderPass candidate = VK_NULL_HANDLE;
		const auto result = vkCreateRenderPass2(device, &create, nullptr, &candidate);
		if (result == VK_SUCCESS) renderPass = candidate;
		return result;
	}

	IndexedMsaa4Resources::~IndexedMsaa4Resources()
	{
		// Parent retirement proves every native borrower complete before release.
		// Descriptor/framebuffer borrowers die before their image views and VMA data.
		if (!device) return;
		for (const auto pipeline : pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
		if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
		if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (ownsRenderPass && renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
		for (auto& image : images)
		{
			if (!image.owned) continue;
			if (image.view) vkDestroyImageView(device, image.view, nullptr);
			if (image.image && image.allocation && allocator) vmaDestroyImage(allocator, image.image, image.allocation);
		}
	}

	namespace
	{
		void CreatePrivateImage(IndexedMsaa4Resources& owner, IndexedMsaa4Attachments::Index index,
			const IndexedMsaa4Attachments& specification)
		{
			auto& image = owner.images[index];
			image.format = specification.formats[index]; image.usage = specification.usages[index];
			image.samples = specification.samples[index]; image.extent = owner.extent;
			VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			create.imageType = VK_IMAGE_TYPE_2D; create.format = image.format;
			create.extent = {owner.extent.width, owner.extent.height, 1};
			create.mipLevels = create.arrayLayers = 1; create.samples = image.samples;
			create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = image.usage;
			create.sharingMode = VK_SHARING_MODE_EXCLUSIVE; create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			Check(vmaCreateImage(owner.allocator, &create, &allocation, &image.image, &image.allocation, nullptr), "MSAA image allocation failed.");
			image.owned = true;
			vmaSetAllocationName(owner.allocator, image.allocation, "Indexed MSAA4 view resources");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
			view.subresourceRange = {static_cast<VkImageAspectFlags>(index == IndexedMsaa4Attachments::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
			Check(vkCreateImageView(owner.device, &view, nullptr, &image.view), "MSAA private image view creation failed.");
		}
		void CreateComputePipeline(IndexedMsaa4Resources& owner, std::span<const std::uint32_t> words, VkPipeline& pipeline)
		{
			VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			shader.codeSize = words.size_bytes(); shader.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			Check(vkCreateShaderModule(owner.device, &shader, nullptr, &module), "MSAA compute shader module creation failed.");
			VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; create.stage.module = module; create.stage.pName = "main";
			create.layout = owner.pipelineLayout;
			const auto result = vkCreateComputePipelines(owner.device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline);
			vkDestroyShaderModule(owner.device, module, nullptr);
			Check(result, "MSAA compute pipeline creation failed.");
		}
		void CreateResolveDescriptors(IndexedMsaa4Resources& owner)
		{
			const std::array bindings{
				VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
				VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
			VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			layout.bindingCount = static_cast<std::uint32_t>(bindings.size()); layout.pBindings = bindings.data();
			Check(vkCreateDescriptorSetLayout(owner.device, &layout, nullptr, &owner.descriptorLayout), "MSAA compute descriptor layout creation failed.");
			const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool.maxSets = 2; pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size()); pool.pPoolSizes = sizes.data();
			Check(vkCreateDescriptorPool(owner.device, &pool, nullptr, &owner.descriptorPool), "MSAA compute descriptor pool creation failed.");
			const std::array layouts{owner.descriptorLayout, owner.descriptorLayout};
			VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocate.descriptorPool = owner.descriptorPool; allocate.descriptorSetCount = 2; allocate.pSetLayouts = layouts.data();
			Check(vkAllocateDescriptorSets(owner.device, &allocate, owner.descriptors.data()), "MSAA compute descriptor allocation failed.");
			using A = IndexedMsaa4Attachments;
			const std::array<VkDescriptorImageInfo, 4> images{{
				{VK_NULL_HANDLE, owner.images[A::Identity].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				{VK_NULL_HANDLE, owner.images[A::ResolvedIdentity].view, VK_IMAGE_LAYOUT_GENERAL},
				{VK_NULL_HANDLE, owner.images[A::Depth].view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
				{VK_NULL_HANDLE, owner.images[A::HiZDepthSeed].view, VK_IMAGE_LAYOUT_GENERAL}}};
			std::array<VkWriteDescriptorSet, 4> writes{};
			for (std::uint32_t index = 0; index < writes.size(); ++index)
			{
				auto& write = writes[index]; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = owner.descriptors[index / 2]; write.dstBinding = index % 2;
				write.descriptorCount = 1;
				write.descriptorType = index % 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				write.pImageInfo = &images[index];
			}
			vkUpdateDescriptorSets(owner.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
			VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DepthSeedConstants)};
			VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &owner.descriptorLayout;
			pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &constants;
			Check(vkCreatePipelineLayout(owner.device, &pipelineLayout, nullptr, &owner.pipelineLayout), "MSAA compute pipeline layout creation failed.");
		}
		VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
			VkAccessFlags source, VkAccessFlags destination) noexcept
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.oldLayout = oldLayout; barrier.newLayout = newLayout;
			barrier.srcAccessMask = source; barrier.dstAccessMask = destination;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			return barrier;
		}
		std::uint32_t DispatchCount(std::uint32_t extent) noexcept { return extent / 8U + (extent % 8U != 0); }
	}

	std::shared_ptr<IndexedMsaa4Resources> CreateIndexedMsaa4Resources(const IndexedMsaa4CreateInfo& info, std::string& reason)
	{
		using A = IndexedMsaa4Attachments;
		try
		{
			Require(info.physicalDevice && info.device && info.allocator && info.extent.width && info.extent.height, "MSAA creation requires a live device/allocator and nonempty extent.");
			const auto specification = DescribeIndexedMsaa4Attachments(info.resolvedColor.format, info.resolvedDepth.format);
			Require(SupportsIndexedMsaa4Attachments(info.physicalDevice, info.extent, specification), "The exact MSAA4 image formats, usages, extent or depth sample-zero resolve are unsupported.");
			const auto borrowed = [&](const IndexedMsaa4BorrowedImage& image, VkFormat format, VkImageUsageFlags required) {
				return image.image && image.view && image.format == format && SameExtent(image.extent, info.extent)
					&& image.samples == VK_SAMPLE_COUNT_1_BIT && (image.usage & required) == required
					&& QualifiesImage(info.physicalDevice, image.format, image.extent, image.usage, image.samples);
			};
			Require(borrowed(info.resolvedColor, specification.formats[A::ResolvedColor], specification.usages[A::ResolvedColor])
				&& borrowed(info.resolvedDepth, specification.formats[A::ResolvedDepth], specification.usages[A::ResolvedDepth]), "MSAA hardware resolves require proven matching single-sample color/depth targets.");
			constexpr auto identityUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			Require(borrowed(info.destinationIdentity, VK_FORMAT_R32G32B32A32_UINT, identityUsage), "MSAA identity destination lacks exact extent, format or real transfer-destination usage.");
			Require(info.resolvedColor.image != info.resolvedDepth.image && info.resolvedColor.image != info.destinationIdentity.image
				&& info.resolvedDepth.image != info.destinationIdentity.image, "MSAA borrowed color/depth/identity images must be distinct.");
			Require(info.identityFinalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || info.identityFinalLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "MSAA identity final layout must match transfer-read or color-attachment consumers.");
			Require(ComputeShader8x8(info.identityShader) && ComputeShader8x8(info.hiZSeedShader), "MSAA resolve shaders require valid main compute entries with fixed 8x8x1 groups.");
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(info.physicalDevice, &properties);
			Require(properties.limits.maxComputeWorkGroupInvocations >= 64 && properties.limits.maxComputeWorkGroupSize[0] >= 8
				&& properties.limits.maxComputeWorkGroupSize[1] >= 8 && properties.limits.maxComputeWorkGroupSize[2] >= 1
				&& properties.limits.maxComputeWorkGroupCount[0] >= DispatchCount(info.extent.width)
				&& properties.limits.maxComputeWorkGroupCount[1] >= DispatchCount(info.extent.height), "MSAA compute extent exceeds native dispatch limits.");
			auto candidate = std::make_shared<IndexedMsaa4Resources>();
			candidate->device = info.device; candidate->allocator = info.allocator; candidate->extent = info.extent;
			candidate->identityDestination = info.destinationIdentity; candidate->identityFinalLayout = info.identityFinalLayout;
			static_cast<IndexedMsaa4BorrowedImage&>(candidate->images[A::ResolvedColor]) = info.resolvedColor;
			static_cast<IndexedMsaa4BorrowedImage&>(candidate->images[A::ResolvedDepth]) = info.resolvedDepth;
			for (const auto index : {A::Color, A::Identity, A::Depth, A::ResolvedIdentity, A::HiZDepthSeed}) CreatePrivateImage(*candidate, index, specification);
			candidate->renderPass = info.compatibleRenderPass;
			if (!candidate->renderPass)
			{
				Check(CreateIndexedMsaa4RenderPass(info.device, specification, candidate->renderPass), "MSAA4 raster render pass creation failed.");
				candidate->ownsRenderPass = true;
			}
			std::array<VkImageView, A::RasterAttachmentCount> views{};
			for (std::size_t index = 0; index < views.size(); ++index) views[index] = candidate->images[index].view;
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = candidate->renderPass; framebuffer.attachmentCount = static_cast<std::uint32_t>(views.size()); framebuffer.pAttachments = views.data();
			framebuffer.width = info.extent.width; framebuffer.height = info.extent.height; framebuffer.layers = 1;
			Check(vkCreateFramebuffer(info.device, &framebuffer, nullptr, &candidate->framebuffer), "MSAA4 raster framebuffer creation failed.");
			CreateResolveDescriptors(*candidate);
			CreateComputePipeline(*candidate, info.identityShader, candidate->pipelines[0]);
			CreateComputePipeline(*candidate, info.hiZSeedShader, candidate->pipelines[1]);
			if (info.compatibleRenderPass && info.takeRenderPassOwnership) candidate->ownsRenderPass = true;
			reason.clear();
			return candidate;
		}
		catch (const std::exception& error) { reason = error.what(); return {}; }
	}

	bool RecordIdentityResolve(VkCommandBuffer command, const IndexedMsaa4Resources& resources) noexcept
	{
		using A = IndexedMsaa4Attachments;
		if (!command || !resources.pipelineLayout || !resources.pipelines[0] || !resources.descriptors[0]
			|| !resources.images[A::ResolvedIdentity].image || !resources.identityDestination.image) return false;
		const auto temporary = resources.images[A::ResolvedIdentity].image;
		const auto destination = resources.identityDestination.image;
		// Both images are completely overwritten. UNDEFINED discards contents but
		// ALL_COMMANDS still orders previous reads/writes before this reuse; this
		// also remains correct if an earlier recorded command was never submitted.
		const std::array begin{
			ImageBarrier(temporary, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT),
			ImageBarrier(destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
			static_cast<std::uint32_t>(begin.size()), begin.data());
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipelines[0]);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipelineLayout, 0, 1, &resources.descriptors[0], 0, nullptr);
		vkCmdDispatch(command, DispatchCount(resources.extent.width), DispatchCount(resources.extent.height), 1);
		const auto computed = ImageBarrier(temporary, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &computed);
		VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.extent = {resources.extent.width, resources.extent.height, 1};
		vkCmdCopyImage(command, temporary, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		const auto finalAccess = resources.identityFinalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT
			: (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		const auto finalStage = resources.identityFinalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		const auto finished = ImageBarrier(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, resources.identityFinalLayout, VK_ACCESS_TRANSFER_WRITE_BIT, finalAccess);
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, finalStage, 0, 0, nullptr, 0, nullptr, 1, &finished);
		return true;
	}

	bool RecordHiZDepthSeed(VkCommandBuffer command, const IndexedMsaa4Resources& resources,
		std::uint32_t occlusionState, std::uint32_t invalidReason) noexcept
	{
		using A = IndexedMsaa4Attachments;
		if (!command || !resources.pipelineLayout || !resources.pipelines[1] || !resources.descriptors[1]
			|| !resources.images[A::HiZDepthSeed].image) return false;
		const auto image = resources.images[A::HiZDepthSeed].image;
		const auto begin = ImageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &begin);
		const DepthSeedConstants constants{{resources.extent.width, resources.extent.height}, {resources.extent.width, resources.extent.height}, occlusionState, invalidReason, {}};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipelines[1]);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipelineLayout, 0, 1, &resources.descriptors[1], 0, nullptr);
		vkCmdPushConstants(command, resources.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
		vkCmdDispatch(command, DispatchCount(resources.extent.width), DispatchCount(resources.extent.height), 1);
		const auto finished = ImageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &finished);
		return true;
	}

}
