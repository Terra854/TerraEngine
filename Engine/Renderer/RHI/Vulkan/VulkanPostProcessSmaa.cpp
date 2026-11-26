// SMAA 1x: transactional native preparation and immutable edge/area/resolve recording.
#include "VulkanPostProcessSmaa.h"
#include "Resource/ResourceManager.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		// These copied values match SMAACommon.hlsli's explicit push layout.
		// Searches are bounded, and exact coverage integration needs no LUT asset.
		struct alignas(16) SmaaConstants
		{
			std::array<std::uint32_t, 2> extent{};
			std::uint32_t maxSearchSteps = 32;
			std::uint32_t maxDiagonalSteps = 16;
			float threshold = 0.1f;
			float localContrastFactor = 2.0f;
			float cornerRounding = 0.0f;
			std::uint32_t flags = 0;
		};
		static_assert(sizeof(SmaaConstants) == 32 && alignof(SmaaConstants) == 16);
		static_assert(offsetof(SmaaConstants, extent) == 0 && offsetof(SmaaConstants, maxSearchSteps) == 8);
		static_assert(offsetof(SmaaConstants, maxDiagonalSteps) == 12 && offsetof(SmaaConstants, threshold) == 16);
		static_assert(offsetof(SmaaConstants, localContrastFactor) == 20 && offsetof(SmaaConstants, cornerRounding) == 24);
		static_assert(offsetof(SmaaConstants, flags) == 28);
	}
	struct SmaaPass
	{
		struct Stage
		{
			VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
			VkDescriptorSet descriptor = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkRenderPass renderPass = VK_NULL_HANDLE;
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
		};
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		std::array<PostProcessTargetImage, 2> intermediates;
		std::array<Stage, 3> stages;
		const PostProcessTargetImage* source = nullptr;
		PostProcessTargetImage* destination = nullptr;
		SmaaConstants constants;
		~SmaaPass()
		{
			// The enclosing owner proves actual GPU completion before release.
			// Shader/pipeline borrowers die before views, images and allocation data.
			if(!device) return;
			for(auto& stage : stages)
			{
				if(stage.pipeline) vkDestroyPipeline(device, stage.pipeline, nullptr);
				if(stage.layout) vkDestroyPipelineLayout(device, stage.layout, nullptr);
			}
			if(descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
			for(auto& stage : stages)
				if(stage.descriptorLayout) vkDestroyDescriptorSetLayout(device, stage.descriptorLayout, nullptr);
			for(std::size_t index = 0; index < intermediates.size(); ++index)
			{
				// The final resolve pass/framebuffer are borrowed, never destroyed.
				if(stages[index].framebuffer) vkDestroyFramebuffer(device, stages[index].framebuffer, nullptr);
				if(stages[index].renderPass) vkDestroyRenderPass(device, stages[index].renderPass, nullptr);
			}
			if(sampler) vkDestroySampler(device, sampler, nullptr);
			for(auto& image : intermediates)
			{
				if(image.view) vkDestroyImageView(device, image.view, nullptr);
				if(image.image && allocator) vmaDestroyImage(allocator, image.image, image.allocation);
			}
		}
	};

	namespace
	{
		constexpr VkImageUsageFlags intermediateUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		constexpr VkFilter samplingFilter = VK_FILTER_NEAREST;
		constexpr bool blendingEnabled = false;
		void Require(bool condition, const char* reason) { if(!condition) throw std::runtime_error(reason); }
		void Check(VkResult result, const char* reason) { Require(result == VK_SUCCESS, reason); }
		bool SameExtent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		std::uint32_t RgbaBytes(VkFormat format) noexcept
		{
			switch(format)
			{
			case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: return 4;
			case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
			case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
			default: return 0;
			}
		}
		bool SupportsImage(VkPhysicalDevice physical, VkFormat format, VkExtent2D extent,
			VkImageUsageFlags usage, std::uint32_t pixelBytes)
		{
			if(!pixelBytes || !extent.width || !extent.height
				|| extent.width > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
				|| extent.height > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) return false;
			VkFormatProperties formatProperties{};
			vkGetPhysicalDeviceFormatProperties(physical, format, &formatProperties);
			VkFormatFeatureFlags required = 0;
			if(usage & VK_IMAGE_USAGE_SAMPLED_BIT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if(usage & VK_IMAGE_USAGE_STORAGE_BIT) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
			if(usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			// Check actual state requirements. Nearest sampling needs no linear
			// filter feature, and disabled blending needs no attachment-blend bit.
			if(samplingFilter == VK_FILTER_LINEAR && (usage & VK_IMAGE_USAGE_SAMPLED_BIT)) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
			if(blendingEnabled && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
			if((formatProperties.optimalTilingFeatures & required) != required) return false;
			VkImageFormatProperties properties{};
			if(vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, usage, 0, &properties) != VK_SUCCESS) return false;
			if(extent.width > properties.maxExtent.width || extent.height > properties.maxExtent.height
				|| properties.maxExtent.depth < 1 || properties.maxArrayLayers < 1 || properties.maxMipLevels < 1
				|| !(properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) return false;
			const auto limit = (std::numeric_limits<std::uint64_t>::max)();
			if(std::uint64_t(extent.width) > limit / extent.height / pixelBytes) return false;
			return std::uint64_t(extent.width) * extent.height * pixelBytes <= properties.maxResourceSize;
		}
		void CreateImage(SmaaPass& pass, PostProcessTargetImage& image, VkFormat format, VkExtent2D extent, const char* label)
		{
			image.extent = extent; image.format = format; image.usage = intermediateUsage;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {extent.width, extent.height, 1};
			info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = image.usage;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			Check(vmaCreateImage(pass.allocator, &info, &allocation, &image.image, &image.allocation, nullptr), "SMAA intermediate allocation failed.");
			vmaSetAllocationName(pass.allocator, image.allocation, label);
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			Check(vkCreateImageView(pass.device, &view, nullptr, &image.view), "SMAA intermediate view creation failed.");
		}
		void CreateIntermediateRenderPass(SmaaPass& owner, std::size_t index)
		{
			auto& stage = owner.stages[index]; const auto& image = owner.intermediates[index];
			VkAttachmentDescription attachment{};
			attachment.format = image.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
			info.attachmentCount = 1; info.pAttachments = &attachment;
			info.subpassCount = 1; info.pSubpasses = &subpass;
			info.dependencyCount = static_cast<std::uint32_t>(dependencies.size()); info.pDependencies = dependencies.data();
			Check(vkCreateRenderPass(owner.device, &info, nullptr, &stage.renderPass), "SMAA private render-pass creation failed.");
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = stage.renderPass; framebuffer.attachmentCount = 1; framebuffer.pAttachments = &image.view;
			framebuffer.width = image.extent.width; framebuffer.height = image.extent.height; framebuffer.layers = 1;
			Check(vkCreateFramebuffer(owner.device, &framebuffer, nullptr, &stage.framebuffer), "SMAA private framebuffer creation failed.");
		}
		struct ShaderModules
		{
			VkDevice device;
			VkShaderModule vertex = VK_NULL_HANDLE;
			std::array<VkShaderModule, 3> fragments{};
			~ShaderModules() { if(vertex) vkDestroyShaderModule(device, vertex, nullptr); for(auto shader : fragments) if(shader) vkDestroyShaderModule(device, shader, nullptr); }
		};
		VkShaderModule LoadShader(VkDevice device, const char* logicalPath)
		{
			auto& resources = ResourceManager::Instance();
			const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(logicalPath));
			Require(bytes.size() >= 5 * sizeof(std::uint32_t) && bytes.size() % sizeof(std::uint32_t) == 0, "Installed built-in SMAA shader is unavailable or malformed.");
			std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
			std::memcpy(words.data(), bytes.data(), bytes.size());
			Require(words.front() == 0x07230203u, "Built-in SMAA shader does not contain SPIR-V.");
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = bytes.size(); info.pCode = words.data();
			VkShaderModule shader = VK_NULL_HANDLE;
			Check(vkCreateShaderModule(device, &info, nullptr, &shader), "SMAA shader module creation failed."); return shader;
		}
		void CreatePipeline(SmaaPass& owner, std::size_t index, const ShaderModules& shaders)
		{
			auto& stage = owner.stages[index];
			std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
			for(auto& item : shaderStages) item.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT; shaderStages[0].module = shaders.vertex; shaderStages[0].pName = "main";
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; shaderStages[1].module = shaders.fragments[index]; shaderStages[1].pName = "PSMain";
			VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = 1; viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
			VkPipelineColorBlendAttachmentState attachment{}; attachment.blendEnable = blendingEnabled;
			attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
			const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
			VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()); dynamic.pDynamicStates = dynamicStates.data();
			VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			info.stageCount = 2; info.pStages = shaderStages.data();
			info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &samples;
			info.pDepthStencilState = &depth; info.pColorBlendState = &blend; info.pDynamicState = &dynamic;
			info.layout = stage.layout; info.renderPass = stage.renderPass;
			Check(vkCreateGraphicsPipelines(owner.device, VK_NULL_HANDLE, 1, &info, nullptr, &stage.pipeline), "SMAA graphics pipeline creation failed.");
		}
		void CreateDescriptors(SmaaPass& owner)
		{
			VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler.magFilter = sampler.minFilter = samplingFilter; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sampler.minLod = sampler.maxLod = 0.0f; sampler.maxAnisotropy = 1.0f;
			Check(vkCreateSampler(owner.device, &sampler, nullptr, &owner.sampler), "SMAA nearest sampler creation failed.");
			VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 3; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
			Check(vkCreateDescriptorPool(owner.device, &pool, nullptr, &owner.descriptorPool), "SMAA descriptor pool creation failed.");
			const std::array<std::array<std::uint32_t, 2>, 3> bindings{{{0, 0}, {1, 0}, {0, 2}}};
			const std::array<std::uint32_t, 3> counts{1, 1, 2};
			const std::array views{owner.source->view, owner.intermediates[0].view, owner.intermediates[1].view};
			for(std::size_t index = 0; index < owner.stages.size(); ++index)
			{
				auto& stage = owner.stages[index];
				std::array<VkDescriptorSetLayoutBinding, 2> layoutBindings{};
				for(std::uint32_t slot = 0; slot < counts[index]; ++slot) layoutBindings[slot] = {bindings[index][slot], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
				VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layout.bindingCount = counts[index]; layout.pBindings = layoutBindings.data();
				Check(vkCreateDescriptorSetLayout(owner.device, &layout, nullptr, &stage.descriptorLayout), "SMAA descriptor layout creation failed.");
				VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = owner.descriptorPool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &stage.descriptorLayout;
				Check(vkAllocateDescriptorSets(owner.device, &allocate, &stage.descriptor), "SMAA immutable descriptor allocation failed.");
				std::array<VkDescriptorImageInfo, 2> images{}; std::array<VkWriteDescriptorSet, 2> writes{};
				for(std::uint32_t slot = 0; slot < counts[index]; ++slot)
				{
					images[slot] = {owner.sampler, views[bindings[index][slot]], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
					writes[slot] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[slot].dstSet = stage.descriptor; writes[slot].dstBinding = bindings[index][slot];
					writes[slot].descriptorCount = 1; writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[slot].pImageInfo = &images[slot];
				}
				vkUpdateDescriptorSets(owner.device, counts[index], writes.data(), 0, nullptr);
				const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SmaaConstants)};
				VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pipeline.setLayoutCount = 1; pipeline.pSetLayouts = &stage.descriptorLayout; pipeline.pushConstantRangeCount = 1; pipeline.pPushConstantRanges = &push;
				Check(vkCreatePipelineLayout(owner.device, &pipeline, nullptr, &stage.layout), "SMAA pipeline layout creation failed.");
			}
		}
	}

	std::shared_ptr<SmaaPass> PrepareSmaaPass(VkPhysicalDevice physicalDevice, PostProcessTargetBundle& targets,
		const PostProcessTargetImage& source, PostProcessTargetImage& destination, VkRenderPass processingPass,
		VkFramebuffer destinationFramebuffer, std::string& reason)
	{
		reason.clear();
		try
		{
			Require(physicalDevice && targets.device && targets.allocator && processingPass && destinationFramebuffer,
				"SMAA needs a live physical/device allocator and compatible resolve pass/framebuffer.");
			Require(source.image && source.view && destination.image && destination.view && source.image != destination.image
				&& source.view != destination.view && SameExtent(source.extent, destination.extent)
				&& SameExtent(source.extent, targets.outputExtent) && SameExtent(targets.internalExtent, targets.outputExtent)
				&& source.format == destination.format && RgbaBytes(source.format)
				&& (source.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (destination.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
				&& (destination.usage & VK_IMAGE_USAGE_SAMPLED_BIT),
				"SMAA requires native matched extents and distinct same-format RGBA sampled source/color destination.");
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physicalDevice, &properties);
			const auto& limits = properties.limits;
			Require(source.extent.width <= limits.maxFramebufferWidth && source.extent.height <= limits.maxFramebufferHeight
				&& source.extent.width <= limits.maxViewportDimensions[0] && source.extent.height <= limits.maxViewportDimensions[1]
				&& limits.viewportBoundsRange[0] <= 0.0f && float(source.extent.width) <= limits.viewportBoundsRange[1]
				&& float(source.extent.height) <= limits.viewportBoundsRange[1] && limits.maxColorAttachments >= 1
				&& limits.maxPushConstantsSize >= sizeof(SmaaConstants) && limits.maxBoundDescriptorSets >= 1
				&& limits.maxPerStageDescriptorSamplers >= 2 && limits.maxDescriptorSetSamplers >= 2
				&& limits.maxPerStageDescriptorSampledImages >= 2 && limits.maxDescriptorSetSampledImages >= 2
				&& limits.maxPerStageResources >= 2,
				"SMAA extent, push constants or immutable sampled descriptors exceed native framebuffer/viewport/device limits.");
			Require(SupportsImage(physicalDevice, source.format, source.extent, source.usage, RgbaBytes(source.format))
				&& SupportsImage(physicalDevice, destination.format, destination.extent, destination.usage, RgbaBytes(destination.format))
				&& SupportsImage(physicalDevice, VK_FORMAT_R8G8_UNORM, source.extent, intermediateUsage, 2)
				&& SupportsImage(physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, source.extent, intermediateUsage, 8),
				"Device cannot provide complete sampled/attachment/usage support for the native SMAA formats and extents.");
			auto pass = std::make_shared<SmaaPass>(); pass->device = targets.device; pass->allocator = targets.allocator;
			pass->source = &source; pass->destination = &destination; pass->constants.extent = {source.extent.width, source.extent.height};
			pass->stages[2].renderPass = processingPass; pass->stages[2].framebuffer = destinationFramebuffer;
			CreateImage(*pass, pass->intermediates[0], VK_FORMAT_R8G8_UNORM, source.extent, "SMAA immutable edges");
			CreateImage(*pass, pass->intermediates[1], VK_FORMAT_R16G16B16A16_SFLOAT, source.extent, "SMAA immutable weights");
			CreateIntermediateRenderPass(*pass, 0); CreateIntermediateRenderPass(*pass, 1); CreateDescriptors(*pass);
			ShaderModules shaders{pass->device};
			shaders.vertex = LoadShader(pass->device, "Shaders/PostProcess/PostProcessFullscreen.mesh.spv");
			const std::array logicalShaders{"Shaders/PostProcess/SMAAEdges.frag.spv", "Shaders/PostProcess/SMAAWeights.frag.spv", "Shaders/PostProcess/SMAAResolve.frag.spv"};
			for(std::size_t index = 0; index < pass->stages.size(); ++index)
			{ shaders.fragments[index] = LoadShader(pass->device, logicalShaders[index]); CreatePipeline(*pass, index, shaders); }
			return pass;
		}
		catch(const std::exception& failure) { reason = failure.what(); return {}; }
	}

	void RecordSmaaPass(VkCommandBuffer command, SmaaPass& pass)
	{
		if(!command || !pass.source || !pass.destination) return;
		// The caller's preceding camera pass already transitions the source to
		// shader-read layout. This dependency covers real color/compute/copy writes.
		VkImageMemoryBarrier sourceReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		sourceReady.oldLayout = sourceReady.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		sourceReady.srcQueueFamilyIndex = sourceReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		sourceReady.image = pass.source->image; sourceReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		sourceReady.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &sourceReady);
		for(std::size_t index = 0; index < pass.stages.size(); ++index)
		{
			auto& stage = pass.stages[index];
			auto& output = index < pass.intermediates.size() ? pass.intermediates[index] : *pass.destination;
			// Discard the old contents without trusting metadata from an abandoned
			// recording. Full queue dependencies still protect prior-frame readers.
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = output.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = stage.renderPass; begin.framebuffer = stage.framebuffer; begin.renderArea.extent = output.extent;
			vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
			VkViewport viewport{0, 0, float(output.extent.width), float(output.extent.height), 0, 1}; VkRect2D scissor{{0, 0}, output.extent};
			vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, stage.pipeline);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, stage.layout, 0, 1, &stage.descriptor, 0, nullptr);
			vkCmdPushConstants(command, stage.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pass.constants), &pass.constants);
			vkCmdDrawMeshTasksEXT(command, 1, 1, 1); vkCmdEndRenderPass(command);
			barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
	}
}
