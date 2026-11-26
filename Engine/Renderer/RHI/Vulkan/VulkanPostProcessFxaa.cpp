// Native-extent FXAA resources; recording consumes copied per-view ownership.
#include "VulkanPostProcessFxaa.h"
#include "Resource/ResourceManager.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct FxaaConstants
		{
			std::array<std::uint32_t, 2> sourceSize{};
			float relativeThreshold = 0.125f;
			float minimumThreshold = 0.03125f;
			float subpixelQuality = 0.5f;
			std::uint32_t endpointSteps = 8;
			std::uint32_t enabled = 1;
			std::uint32_t reserved = 0;
		};
		static_assert(sizeof(FxaaConstants) == 32);
		static_assert(offsetof(FxaaConstants, relativeThreshold) == 8 && offsetof(FxaaConstants, minimumThreshold) == 12);
		static_assert(offsetof(FxaaConstants, subpixelQuality) == 16 && offsetof(FxaaConstants, endpointSteps) == 20);
		static_assert(offsetof(FxaaConstants, enabled) == 24 && offsetof(FxaaConstants, reserved) == 28);

		bool LinearFormat(VkFormat format) noexcept
		{
			return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT
				|| format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
		}
		bool SupportsImage(VkPhysicalDevice physicalDevice, const PostProcessTargetImage& image, VkFormatFeatureFlags features)
		{
			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, image.format, &properties);
			if ((properties.optimalTilingFeatures & features) != features) return false;
			VkImageFormatProperties limits{};
			if (vkGetPhysicalDeviceImageFormatProperties(physicalDevice, image.format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, image.usage, 0, &limits) != VK_SUCCESS) return false;
			return image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height
				&& limits.maxExtent.depth >= 1 && limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1
				&& (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
		}
		struct ShaderModules
		{
			VkDevice device;
			VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
			~ShaderModules()
			{
				if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
				if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
			}
		};
		VkShaderModule LoadModule(VkDevice device, const char* path)
		{
			auto& resources = ResourceManager::Instance();
			const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			if (bytes.empty() || bytes.size() % sizeof(std::uint32_t)) return VK_NULL_HANDLE;
			std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
			std::memcpy(words.data(), bytes.data(), bytes.size());
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = bytes.size(); info.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
		}
	}

	struct FxaaPass
	{
		VkDevice device = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkRenderPass renderPass = VK_NULL_HANDLE; // borrowed compatible pass
		VkFramebuffer framebuffer = VK_NULL_HANDLE; // borrowed destination view
		PostProcessTargetImage* destination = nullptr;
		FxaaConstants constants;
		~FxaaPass()
		{
			// The enclosing target owner must establish actual GPU completion.
			// Descriptor/pipeline borrowers disappear before their sampler/images.
			if (!device) return;
			if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
			if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
			if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
			if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
			if (sampler) vkDestroySampler(device, sampler, nullptr);
		}
	};

	namespace
	{
		bool CreatePipeline(FxaaPass& pass)
		{
			ShaderModules shaders{pass.device};
			shaders.vertex = LoadModule(pass.device, "Shaders/PostProcess/PostProcessFullscreen.mesh.spv");
			shaders.fragment = LoadModule(pass.device, "Shaders/PostProcess/FXAA.frag.spv");
			if (!shaders.vertex || !shaders.fragment) return false;
			std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
			for (auto& stage : stages) stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT; stages[0].module = shaders.vertex; stages[0].pName = "main";
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = shaders.fragment; stages[1].pName = "PSMain";
			VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
			viewport.viewportCount = 1; viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
			raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
			raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
			samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
			VkPipelineColorBlendAttachmentState attachment{};
			// The shader filters only RGB and copies center alpha. No fixed-function
			// blending, depth operation or second transfer modifies that result.
			attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
			blend.attachmentCount = 1; blend.pAttachments = &attachment;
			const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
			VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
			dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states.data();
			VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			info.stageCount = 2; info.pStages = stages.data();
			info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &samples;
			info.pDepthStencilState = &depth; info.pColorBlendState = &blend; info.pDynamicState = &dynamic;
			info.layout = pass.layout; info.renderPass = pass.renderPass;
			return vkCreateGraphicsPipelines(pass.device, VK_NULL_HANDLE, 1, &info, nullptr, &pass.pipeline) == VK_SUCCESS;
		}
	}

	std::shared_ptr<FxaaPass> PrepareFxaaPass(VkPhysicalDevice physicalDevice,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& source,
		PostProcessTargetImage& destination, VkRenderPass processingPass,
		VkFramebuffer destinationFramebuffer, std::string& reason)
	{
		reason.clear();
		if (!physicalDevice || !targets.device || !source.image || !source.view || !destination.image || !destination.view
			|| source.image == destination.image || !processingPass || !destinationFramebuffer
			|| !source.extent.width || !source.extent.height || source.extent.width != destination.extent.width
			|| source.extent.height != destination.extent.height || source.format != destination.format
			|| !(source.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(destination.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
			|| !(destination.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !LinearFormat(source.format)) {
			reason = "FXAA needs distinct equally sized linear-format sampled/color attachments and a compatible framebuffer.";
			return {};
		}
		if (!SupportsImage(physicalDevice, source, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
			|| !SupportsImage(physicalDevice, destination, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
			reason = "The actual FXAA source/destination format, usage or extent is unsupported.";
			return {};
		}
		try {
			auto pass = std::make_shared<FxaaPass>();
			pass->device = targets.device; pass->destination = &destination;
			pass->renderPass = processingPass; pass->framebuffer = destinationFramebuffer;
			pass->constants.sourceSize = {source.extent.width, source.extent.height};
			// This dedicated sampler guarantees the shader's real filtering contract
			// without depending on an unrelated target sampler's future settings.
			VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler.minFilter = sampler.magFilter = VK_FILTER_LINEAR;
			sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			if (vkCreateSampler(pass->device, &sampler, nullptr, &pass->sampler) != VK_SUCCESS) throw std::runtime_error("FXAA sampler creation failed.");
			const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			descriptors.bindingCount = 1; descriptors.pBindings = &binding;
			if (vkCreateDescriptorSetLayout(pass->device, &descriptors, nullptr, &pass->descriptorLayout) != VK_SUCCESS) throw std::runtime_error("FXAA descriptor layout creation failed.");
			const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
			if (vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->descriptorPool) != VK_SUCCESS) throw std::runtime_error("FXAA descriptor pool creation failed.");
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocation.descriptorPool = pass->descriptorPool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &pass->descriptorLayout;
			if (vkAllocateDescriptorSets(pass->device, &allocation, &pass->descriptor) != VK_SUCCESS) throw std::runtime_error("FXAA descriptor allocation failed.");
			const VkDescriptorImageInfo image{pass->sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = pass->descriptor; write.dstBinding = 0; write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
			vkUpdateDescriptorSets(pass->device, 1, &write, 0, nullptr);
			const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FxaaConstants)};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout.setLayoutCount = 1; layout.pSetLayouts = &pass->descriptorLayout;
			layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
			if (vkCreatePipelineLayout(pass->device, &layout, nullptr, &pass->layout) != VK_SUCCESS || !CreatePipeline(*pass))
				throw std::runtime_error("Installed FXAA shader or compatible pipeline creation failed.");
			return pass;
		} catch (const std::exception& error) { reason = error.what(); return {}; }
	}

	void RecordFxaaPass(VkCommandBuffer command, FxaaPass& pass)
	{
		if (!command || !pass.pipeline || !pass.destination) return;
		auto& destination = *pass.destination;
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		// Full overwrite is independent of speculative layout bookkeeping in a
		// discarded command. ALL_COMMANDS orders prior readbacks/readers too.
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = destination.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = pass.renderPass; begin.framebuffer = pass.framebuffer; begin.renderArea.extent = destination.extent;
		vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
		const VkViewport viewport{0, 0, float(destination.extent.width), float(destination.extent.height), 0, 1};
		const VkRect2D scissor{{0, 0}, destination.extent};
		vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 1, &pass.descriptor, 0, nullptr);
		vkCmdPushConstants(command, pass.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pass.constants), &pass.constants);
		vkCmdDrawMeshTasksEXT(command, 1, 1, 1);
		vkCmdEndRenderPass(command);
		// The borrowed compatible pass ends in SHADER_READ. Publish its write
		// to subsequent image consumers, including explicit native capture copies.
		barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		destination.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
}
