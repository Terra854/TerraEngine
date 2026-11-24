// VulkanPostProcessPasses.cpp - prepare and record the camera's finished image.
#include "VulkanContextSource.h"
#include "VulkanPostProcessPasses.h"
#include "VulkanPostProcessLensPresentation.h"
#include "VulkanPostProcessTemporalView.h"
#include "VulkanPostProcessScaledCamera.h"
#include "VulkanPostProcessDepthOfField.h"
#include "VulkanPostProcessMotionBlur.h"
#include "VulkanPostProcessScreenSpaceLighting.h"
#include "VulkanPostProcessCustomView.h"
#include "VulkanPostProcessExposure.h"
#include "VulkanPostProcessBloom.h"
#include "VulkanPostProcessTone.h"
#include "VulkanPostProcessLookups.h"
#include "VulkanPostProcessFxaa.h"
#include "VulkanPostProcessSmaa.h"
#include "VulkanPostProcessCmaa2.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/GraphicsSettings/PostProcessAuthoringFrame.h"
#include "File Formats/TerraEngineTextureFile.h"
#include <cmath>

namespace TerraEngine::Renderer::Vulkan
{
	// This owner borrows the target bundle's two sampled images and its output
	// framebuffer. The parent retains the bundle through queue completion.
	struct AtmospherePass
	{
		VkDevice device = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkImage depth = VK_NULL_HANDLE;
		std::uint32_t outputIndex = 0;
		~AtmospherePass()
		{
			if(pipeline) vkDestroyPipeline(device, pipeline, nullptr);
			if(layout) vkDestroyPipelineLayout(device, layout, nullptr);
			if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			if(descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
		}
	};

	namespace
	{
		VkShaderModule LoadModule(VkDevice device, const char* relativePath)
		{
			const auto path = ResourceManager::Instance().ResolveEngineAssetPath(relativePath);
			const auto bytes = ResourceManager::Instance().ReadFileBytes(path);
			if(bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) return VK_NULL_HANDLE;
			std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
			std::memcpy(words.data(), bytes.data(), bytes.size());
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = bytes.size();
			info.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
		}

		bool CreateLayout(VkDevice device, VkDescriptorSetLayout descriptor, std::uint32_t pushBytes,
			VkShaderStageFlags pushStages, VkPipelineLayout& layout)
		{
			VkPushConstantRange push{pushStages, 0, pushBytes};
			VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			info.setLayoutCount = descriptor != VK_NULL_HANDLE ? 1u : 0u;
			info.pSetLayouts = descriptor != VK_NULL_HANDLE ? &descriptor : nullptr;
			info.pushConstantRangeCount = 1;
			info.pPushConstantRanges = &push;
			return vkCreatePipelineLayout(device, &info, nullptr, &layout) == VK_SUCCESS;
		}

		bool CreatePipeline(VkDevice device, VkRenderPass renderPass, VkPipelineLayout layout,
			std::uint32_t colorCount, bool grid, VkPipeline& pipeline,
			const char* fullscreenFragment = "Shaders/PostProcess/PostProcessExposure.frag.spv")
		{
			const VkShaderModule vertex = LoadModule(device, grid
				? "Shaders/Editor/SceneViewGrid.vert.spv" : "Shaders/PostProcess/PostProcessFullscreen.vert.spv");
			const VkShaderModule fragment = LoadModule(device, grid
				? "Shaders/Editor/SceneViewGrid.frag.spv" : fullscreenFragment);
			if(!vertex || !fragment)
			{
				if(vertex) vkDestroyShaderModule(device, vertex, nullptr);
				if(fragment) vkDestroyShaderModule(device, fragment, nullptr);
				return false;
			}
			std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
			stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			stages[0].module = vertex;
			stages[0].pName = "VSMain";
			stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[1].module = fragment;
			stages[1].pName = grid ? "PSMainWithDepth" : "PSMain";
			VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
			VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
			assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
			viewport.viewportCount = 1;
			viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
			raster.polygonMode = VK_POLYGON_MODE_FILL;
			raster.cullMode = VK_CULL_MODE_NONE;
			raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			raster.lineWidth = 1.0f;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
			samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
			depth.depthTestEnable = grid ? VK_TRUE : VK_FALSE;
			// The overlay may test scene depth, but it never changes the geometry
			// depth used by picking, Hi-Z, and later reconstruction consumers.
			depth.depthWriteEnable = VK_FALSE;
			depth.depthCompareOp = VK_COMPARE_OP_LESS;
			std::array<VkPipelineColorBlendAttachmentState, 2> attachments{};
			attachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
				| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			attachments[0].blendEnable = grid ? VK_TRUE : VK_FALSE;
			attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			attachments[0].colorBlendOp = VK_BLEND_OP_ADD;
			attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			attachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
			// The second conventional attachment stores typed object identity.
			// Fullscreen color and overlays never write to that attachment.
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
			blend.attachmentCount = colorCount;
			blend.pAttachments = attachments.data();
			const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
			VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
			dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size());
			dynamic.pDynamicStates = states.data();
			VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			info.stageCount = static_cast<std::uint32_t>(stages.size());
			info.pStages = stages.data();
			info.pVertexInputState = &vertexInput;
			info.pInputAssemblyState = &assembly;
			info.pViewportState = &viewport;
			info.pRasterizationState = &raster;
			info.pMultisampleState = &samples;
			info.pDepthStencilState = &depth;
			info.pColorBlendState = &blend;
			info.pDynamicState = &dynamic;
			info.layout = layout;
			info.renderPass = renderPass;
			const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
			vkDestroyShaderModule(device, vertex, nullptr);
			vkDestroyShaderModule(device, fragment, nullptr);
			return result == VK_SUCCESS;
		}

		bool CreateOverlayPass(VkDevice device, const PostProcessPassConfiguration& configuration, VkRenderPass& pass)
		{
			std::array<VkAttachmentDescription2, 4> attachments{};
			for(auto& attachment : attachments)
			{
				attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
				attachment.samples = VK_SAMPLE_COUNT_1_BIT;
				attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			}
			attachments[0].format = configuration.destinationFormat;
			attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			attachments[1].format = VK_FORMAT_R32G32B32A32_UINT;
			attachments[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments[2].format = configuration.depthFormat;
			attachments[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			if(configuration.needsScaledCamera)
			{
				// A scaled Game camera rasterized into separate internal images.
				// Its full-size presentation starts with defined clear depth and
				// identity before compositing the reconstructed color and HUD.
				for(std::uint32_t index = 0u; index < 3u; ++index)
				{
					attachments[index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				}
			}
			attachments[3].format = VK_FORMAT_R8_UINT;
			attachments[3].initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
			attachments[3].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
			std::array<VkAttachmentReference2, 2> colors{};
			for(std::uint32_t index = 0; index < colors.size(); ++index)
			{
				colors[index] = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
				colors[index].attachment = index;
				colors[index].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
			VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
			depth.attachment = 2;
			depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			VkAttachmentReference2 rate{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
			rate.attachment = 3;
			rate.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
			VkFragmentShadingRateAttachmentInfoKHR shading{VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
			shading.pFragmentShadingRateAttachment = &rate;
			shading.shadingRateAttachmentTexelSize = configuration.vrsTexelSize;
			VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
			subpass.pColorAttachments = colors.data();
			subpass.pDepthStencilAttachment = &depth;
			subpass.pNext = configuration.usesVrs ? &shading : nullptr;
			// LOAD reads the preserved attachment after its implicit layout transition.
			// Include COLOR_ATTACHMENT_READ as well as writes in the dependency;
			// exact-pass HUD pipelines are prepared separately from legacy geometry.
			std::array<VkSubpassDependency2, 2> dependencies{};
			for(auto& dependency : dependencies) dependency.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			if(configuration.usesVrs)
			{
				dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
				dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
				dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			}
			else
			{
				dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
				dependencies[1].srcSubpass = 0;
				dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
				dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
				dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
				dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
			}
			VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
			info.attachmentCount = configuration.usesVrs ? 4u : 3u;
			info.pAttachments = attachments.data();
			info.subpassCount = 1;
			info.pSubpasses = &subpass;
			info.dependencyCount = configuration.usesVrs ? 1u : 2u;
			info.pDependencies = dependencies.data();
			return vkCreateRenderPass2(device, &info, nullptr, &pass) == VK_SUCCESS;
		}

		void Transition(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
			VkAccessFlags sourceAccess, VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStage,
			VkPipelineStageFlags destinationStage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.oldLayout = oldLayout;
			barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = {aspect, 0, 1, 0, 1};
			barrier.srcAccessMask = sourceAccess;
			barrier.dstAccessMask = destinationAccess;
			vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		void Viewport(VkCommandBuffer command, VkExtent2D extent)
		{
			VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
			VkRect2D scissor{{0, 0}, extent};
			vkCmdSetViewport(command, 0, 1, &viewport);
			vkCmdSetScissor(command, 0, 1, &scissor);
		}

		void Fullscreen(VkCommandBuffer command, const PostProcessPassResources& resources,
			VkPipeline pipeline, VkDescriptorSet source, float exposure)
		{
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resources.fullscreenLayout, 0, 1, &source, 0, nullptr);
			vkCmdPushConstants(command, resources.fullscreenLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(exposure), &exposure);
			vkCmdDraw(command, 3, 1, 0, 0);
		}

		struct AtmosphereConstants
		{
			glm::mat4 inverseProjection;
			glm::vec4 worldUpAndHeight, fog, scatterAndProjection, sun;
		};
		static_assert(sizeof(AtmosphereConstants) == 128);

		std::shared_ptr<AtmospherePass> PrepareAtmosphere(const PostProcessTargetBundle& targets,
			const PostProcessPassResources& resources, const PostProcessPassConfiguration& configuration,
			const PostProcessTargetImage& source, std::uint32_t outputIndex)
		{
			const auto& depth = configuration.sceneDepth;
			if(!configuration.physicalDevice || !depth.image || !depth.view || !source.view
				|| !(depth.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || depth.extent.width != targets.outputExtent.width
				|| depth.extent.height != targets.outputExtent.height
				|| (depth.format != VK_FORMAT_D32_SFLOAT && depth.format != VK_FORMAT_D16_UNORM)) return {};
			VkFormatProperties features{};
			vkGetPhysicalDeviceFormatProperties(configuration.physicalDevice, depth.format, &features);
			if(!(features.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return {};

			auto pass = std::make_shared<AtmospherePass>();
			pass->device = targets.device;
			pass->depth = depth.image;
			pass->outputIndex = outputIndex;
			const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
				{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
				{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
			VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
			layout.pBindings = bindings.data();
			if(vkCreateDescriptorSetLayout(pass->device, &layout, nullptr, &pass->descriptors) != VK_SUCCESS) return {};
			const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool.maxSets = pool.poolSizeCount = 1;
			pool.pPoolSizes = &size;
			if(vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool) != VK_SUCCESS) return {};
			VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			set.descriptorPool = pass->pool;
			set.descriptorSetCount = 1;
			set.pSetLayouts = &pass->descriptors;
			if(vkAllocateDescriptorSets(pass->device, &set, &pass->set) != VK_SUCCESS) return {};
			const std::array<VkDescriptorImageInfo, 2> images{{
				{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				{VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}}};
			std::array<VkWriteDescriptorSet, 2> writes{};
			for(std::uint32_t index = 0; index < writes.size(); ++index)
			{
				writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				writes[index].dstSet = pass->set;
				writes[index].dstBinding = index;
				writes[index].descriptorCount = 1;
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				writes[index].pImageInfo = &images[index];
			}
			vkUpdateDescriptorSets(pass->device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
			if(!CreateLayout(targets.device, pass->descriptors, sizeof(AtmosphereConstants), VK_SHADER_STAGE_FRAGMENT_BIT, pass->layout)
				|| !CreatePipeline(targets.device, resources.processingPass, pass->layout, 1, false, pass->pipeline,
					"Shaders/PostProcess/VolumetricFog.frag.spv")) return {};
			return pass;
		}

		void RecordAtmosphere(VkCommandBuffer command, PostProcessTargetBundle& targets,
			const AtmospherePass& pass, const RenderViewPresentationInput& view,
			const GraphicsSettings::AtmosphereControls& controls)
		{
			// The sealed camera origin and local eye position let the fog plane
			// stay fixed through world-origin rebases. Only the relative height
			// reaches the shader as a float.
			const auto& origin = view.referenceFrame.canonicalOrigin;
			const double height = static_cast<double>(origin.cell.y) * Scene::kWorldCellEdgeMetres
				+ (origin.offsetMetres.y + view.cameraLocalPosition.y - controls.baseHeight);
			const glm::vec3 viewUp = glm::mat3(view.viewMatrix) * glm::vec3(0.0f, 1.0f, 0.0f);
			const bool orthographic = std::abs(view.projectionMatrix[3][3]) > 0.5f;
			const glm::vec4 sunClip = view.projectionMatrix * glm::vec4(glm::mat3(view.viewMatrix) * view.sky.sunDirection, 0.0f);
			glm::vec4 sun(0.0f);
			if(controls.shaftsEnabled && view.sky.renderBackground && !view.sky.forceBlack && !orthographic && sunClip.w > 0.0001f)
			{
				const glm::vec2 uv = glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f;
				if(uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f)
					sun = glm::vec4(uv, 1.0f, 0.0f);
			}
			const AtmosphereConstants constants{glm::inverse(view.projectionMatrix), glm::vec4(viewUp, static_cast<float>(height)),
				glm::vec4(controls.fogEnabled ? controls.density : 0.0f, controls.heightFalloff, controls.maxDistance, controls.shaftsIntensity),
				glm::vec4(view.sky.horizonTint, orthographic ? 1.0f : 0.0f), sun};
			auto& output = targets.processing[pass.outputIndex];
			constexpr auto depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			constexpr auto depthAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			Transition(command, pass.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
				depthAccess, VK_ACCESS_SHADER_READ_BIT, depthStages, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
			Transition(command, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			begin.renderPass = targets.passes->processingPass;
			begin.framebuffer = targets.passes->processingFramebuffers[pass.outputIndex];
			begin.renderArea.extent = targets.outputExtent;
			vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
			Viewport(command, targets.outputExtent);
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 1, &pass.set, 0, nullptr);
			vkCmdPushConstants(command, pass.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
			vkCmdDraw(command, 3, 1, 0, 0);
			vkCmdEndRenderPass(command);
			output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			Transition(command, output.image, output.layout, output.layout, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			// Restore depth for focus, motion blur and the editor grid.
			Transition(command, pass.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_ACCESS_SHADER_READ_BIT, depthAccess, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, depthStages, VK_IMAGE_ASPECT_DEPTH_BIT);
		}
	}

	PostProcessPassResources::~PostProcessPassResources()
	{
		// Stage pipelines and descriptors borrow the shared pass and image
		// handles. Release those borrowers before tearing down the target.
		customView.reset();
		lensPresentation.reset();
		lensFlare.reset();
		lensDirt.reset();
		lensRadiance.reset();
		depthOfField.reset();
		atmosphere.reset();
		motionBlur.reset();
		temporalView.reset();
		temporalFrameState.reset();
		scaledCamera.reset();
		screenLighting.reset();
		if(screenRaster && retireScreenRaster) retireScreenRaster(screenRasterContext, screenRaster);
		screenRaster.reset();
		cmaa2.reset();
		smaa.reset();
		fxaa.reset();
		tone.reset();
		bloom.reset();
		autoExposure.reset();
		lookups.reset();
		if(staticUiPipeline) vkDestroyPipeline(device, staticUiPipeline, nullptr);
		if(sdfTextPipeline) vkDestroyPipeline(device, sdfTextPipeline, nullptr);
		if(gridPipeline) vkDestroyPipeline(device, gridPipeline, nullptr);
		if(gridLayout) vkDestroyPipelineLayout(device, gridLayout, nullptr);
		if(compositePipeline) vkDestroyPipeline(device, compositePipeline, nullptr);
		if(ditherPipeline) vkDestroyPipeline(device, ditherPipeline, nullptr);
		if(exposurePipeline) vkDestroyPipeline(device, exposurePipeline, nullptr);
		if(fullscreenLayout) vkDestroyPipelineLayout(device, fullscreenLayout, nullptr);
		if(overlayFramebuffer) vkDestroyFramebuffer(device, overlayFramebuffer, nullptr);
		for(const auto framebuffer : processingFramebuffers)
			if(framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if(ownsOverlayPass && overlayPass) vkDestroyRenderPass(device, overlayPass, nullptr);
		if(processingPass) vkDestroyRenderPass(device, processingPass, nullptr);
	}

	bool MatchesPostProcessCamera(const PostProcessTargetKey& key,
		const GraphicsSettings::GraphicsCameraTarget& camera) noexcept
	{
		return key.cameraId == camera.cameraId && key.cameraGenerations == std::array<std::uint64_t, 4>{
			camera.sceneGeneration, camera.objectLifetimeGeneration, camera.componentGeneration, camera.bindingGeneration};
	}

	PostProcessTargetKey MakePostProcessTargetKey(const RenderViewPresentationInput& presentation,
		std::uint64_t rendererGeneration) noexcept
	{
		PostProcessTargetKey key{static_cast<std::uint32_t>(presentation.output.kind) << 8u
			| static_cast<std::uint32_t>(presentation.output.diagnosticsView), presentation.output.outputId, rendererGeneration};
		if (presentation.graphicsCamera) {
			const auto& camera = *presentation.graphicsCamera;
			key.cameraId = camera.cameraId;
			key.cameraGenerations = {camera.sceneGeneration, camera.objectLifetimeGeneration,
				camera.componentGeneration, camera.bindingGeneration};
			if (presentation.graphicsView) key.viewLifetimeGeneration = presentation.graphicsView->lifetimeGeneration;
		}
		return key;
	}

	bool BuildPostProcessViewRequest(const RenderViewPresentationInput& presentation,
		const GraphicsSettings::GraphicsSettingsSnapshot& settings,
		const PreparedPostProcessViewResources& resources, PostProcessTargetRequest& request,
		PostProcessPassConfiguration& configuration, std::string& reason)
	{
		const auto plan = BuildPostProcessGraphSettings(settings);
		if(settings.lifecycle.rendererSessionGeneration != resources.rendererGeneration
			|| settings.lifecycle.backendResourceGeneration != resources.backendResourceGeneration)
		{
			reason = "The settings candidate belongs to an earlier backend lifetime.";
			return false;
		}
		if((!plan.hdrCamera && !plan.toneAndGrade && !plan.spatialAA && !plan.coverageAA && !plan.presentationEffects && !plan.temporalResolve && !plan.screenSpaceLighting && !plan.customStages && !plan.outputDithering)
			|| !SupportsPostProcessPlan(plan))
		{
			reason = "The active post-processing chain requires qualified native camera attachments.";
			return false;
		}
		if(resources.sourceExtent.width == 0u || resources.sourceExtent.height == 0u
			|| (resources.sourceFormat != VK_FORMAT_R16G16B16A16_SFLOAT && resources.sourceFormat != VK_FORMAT_R32G32B32A32_SFLOAT)
			|| resources.passes.destinationRenderPass == VK_NULL_HANDLE || resources.passes.depthFormat == VK_FORMAT_UNDEFINED)
		{
			reason = "The actual HDR source and compatible output render pass are not ready.";
			return false;
		}
		const auto matchesExtent = [&](glm::uvec2 extent, VkExtent2D expected) {
			return (extent.x == 0u && extent.y == 0u)
				|| (extent.x == expected.width && extent.y == expected.height);
		};
		GraphicsSettings::GraphicsAAConfiguration aa;
		if(GraphicsSettings::ResolveGraphicsAAConfiguration(settings.values, aa) != GraphicsSettings::GraphicsRequestReason::None)
		{ reason = "The copied camera settings have no exact native AA configuration."; return false; }
		const auto outputExtent = resources.sourceExtent;
		const VkExtent2D internalExtent{
			GraphicsSettings::ResolveGraphicsAAInternalDimension(outputExtent.width, aa),
			GraphicsSettings::ResolveGraphicsAAInternalDimension(outputExtent.height, aa)};
		const bool scaled = aa.upscaling == GraphicsSettings::GraphicsUpscalingMode::Temporal;
		if(scaled && (!resources.passes.cameraRasterPass || resources.passes.usesVrs
			|| presentation.output.diagnosticsView != RenderViewType::Game))
		{ reason = "Scaled indexed reconstruction requires a qualified Game camera without VRS."; return false; }
		// An inherited Game root describes its own main output dimensions, not
		// every Scene or dedicated camera surface that inherits its values.
		const bool boundView = settings.target.view && presentation.graphicsView
			&& *settings.target.view == *presentation.graphicsView;
		if(boundView && (!matchesExtent(plan.internalSize, internalExtent) || !matchesExtent(plan.outputSize, outputExtent)))
		{
			reason = "The candidate extent does not match the actual native source.";
			return false;
		}
		configuration = resources.passes;
		// Dedicated float captures have no display quantization boundary. The
		// editor camera composite uses the format of its eventual SDR swapchain.
		configuration.ditherCameraOutput = plan.outputDithering && resources.passes.ditherCameraOutput
			&& presentation.output.kind != RenderOutputKind::OffscreenTexture;
		configuration.color = GraphicsSettings::ResolvePostProcessColorSettings(settings.values);
		if(GraphicsSettings::ResolvePostProcessEffectSettings(settings.values, configuration.effects)
			!= GraphicsSettings::GraphicsRequestReason::None)
		{ reason = "The copied camera effects could not be resolved."; return false; }
		configuration.backendResourceGeneration = resources.backendResourceGeneration;
		configuration.viewLifetimeGeneration = presentation.graphicsView ? presentation.graphicsView->lifetimeGeneration : 0u;
		if(plan.customStages)
		{
			const auto& frame = presentation.graphicsAuthoring;
			if(!frame || !frame->HasEnabledEffects() || frame->lifecycle != settings.lifecycle
				|| frame->target.camera != presentation.graphicsCamera || frame->target.view != presentation.graphicsView)
			{ reason = "Custom effects have no current camera and native view binding."; return false; }
			configuration.custom = std::make_shared<PostProcessCustomViewConfiguration>(PostProcessCustomViewConfiguration{
				frame->effects, {frame->target, frame->lifecycle}, frame->preparedFromFrameId, frame->authoringRevision});
		}
		if(plan.screenSpaceLighting)
		{
			if(!presentation.graphicsView || !configuration.viewLifetimeGeneration || resources.passes.usesVrs
				|| aa.raster != GraphicsSettings::GraphicsGeometryRasterMode::Indexed || !resources.sourceTargetGeneration)
			{ reason = "Screen-space lighting requires a registered indexed native view with VRS off."; return false; }
			configuration.screenSpaceOwner = {settings.lifecycle, *presentation.graphicsView, resources.sourceTargetGeneration};
		}
		// Linear depth and HDR lens passes currently borrow single-sample,
		// native-size attachments. Their combinations are certified before use.
		if((configuration.effects.HasHdrCameraEffects() && (scaled || plan.coverageAA))
			// A reconstructed view and its jittered geometry depth no longer line
			// up pixel for pixel. Qualify that combination with an aligned input.
			|| (configuration.effects.atmosphere.Active() && plan.temporalResolve)
			|| (configuration.effects.motionBlur.Active() && (aa.raster != GraphicsSettings::GraphicsGeometryRasterMode::Indexed
				|| resources.passes.usesVrs || !configuration.viewLifetimeGeneration))
			|| (configuration.effects.depthOfField.Active() && !configuration.viewLifetimeGeneration))
		{ reason = "This camera effect requires a compatible live native view."; return false; }
		configuration.needsPresentationEffects = plan.presentationEffects;
		configuration.needsTemporal = plan.temporalResolve;
		configuration.needsScaledCamera = scaled;
		// Coverage has already been resolved by rasterization when these stages
		// run. Only one of the named post-AA algorithms may request a shader.
		configuration.antiAliasing = plan.spatialAA
			? static_cast<GraphicsSettings::GraphicsAntiAliasingMode>(plan.antiAliasingMode)
			: GraphicsSettings::GraphicsAntiAliasingMode::Off;
		configuration.needsGrid = presentation.output.diagnosticsView == RenderViewType::Scene && presentation.sceneViewGridVisible;
		request = {};
		request.enabled = true;
		request.key = MakePostProcessTargetKey(presentation, resources.rendererGeneration);
		if (presentation.graphicsCamera && (!presentation.graphicsView
			|| !request.key.viewLifetimeGeneration
			|| presentation.graphicsView->viewType != presentation.output.diagnosticsView
			|| presentation.graphicsView->outputKind != presentation.output.kind
			|| presentation.graphicsView->outputId != presentation.output.outputId)) {
			reason = "The authored camera has no matching stable native view lifetime.";
			return false;
		}
		request.internalExtent = internalExtent;
		request.outputExtent = outputExtent;
		const auto& color = configuration.color;
		const std::uint32_t stageCount = static_cast<std::uint32_t>(color.HasExposure())
			+ static_cast<std::uint32_t>(color.HasBloom()) + static_cast<std::uint32_t>(color.HasToneAndGrading())
			+ static_cast<std::uint32_t>(plan.spatialAA) + static_cast<std::uint32_t>(plan.presentationEffects)
			+ static_cast<std::uint32_t>(plan.temporalResolve)
			+ static_cast<std::uint32_t>(configuration.effects.atmosphere.Active())
			+ static_cast<std::uint32_t>(configuration.effects.depthOfField.Active())
			+ static_cast<std::uint32_t>(configuration.effects.motionBlur.Active())
			+ static_cast<std::uint32_t>(configuration.effects.motionBlur.Active() && !configuration.needsTemporal)
			+ static_cast<std::uint32_t>(plan.screenSpaceLighting)
			+ static_cast<std::uint32_t>(configuration.effects.lensDirt.Active())
			+ static_cast<std::uint32_t>(configuration.effects.lensFlare.Active());
		request.processingImageCount = std::clamp(stageCount, 1u, 2u);
		if(configuration.custom) request.processingImageCount = 2u;
		request.passCompatibilityId = reinterpret_cast<std::uint64_t>(configuration.destinationRenderPass)
			^ (configuration.needsGrid ? std::uint64_t{1} << 63u : 0u)
			^ (resources.backendResourceGeneration * 0x9e3779b97f4a7c15ull)
			^ (resources.sourceTargetGeneration * 0xbf58476d1ce4e5b9ull);
		// Reopening or resizing Scene View must replace effect-specific history.
		// The physical Scene key stays stable so replacement retires its old owner.
		if(presentation.graphicsView && (presentation.output.kind == RenderOutputKind::SceneView || configuration.effects.motionBlur.Active()))
			request.passCompatibilityId ^= presentation.graphicsView->lifetimeGeneration * 0xd6e8feb86659fd93ull;
		// Only structural choices rebuild the surrounding Vulkan owners. EV,
		// adaptation rates, grade parameters and bloom thresholds are uniforms.
		const std::uint64_t topology = (color.HasExposure() ? 1u : 0u)
			| (color.exposure == GraphicsSettings::ExposureMode::Automatic ? 2u : 0u)
			| (color.HasBloom() ? 4u : 0u) | (color.HasToneAndGrading() ? 8u : 0u)
			| (std::uint64_t{plan.antiAliasingMode} << 4u)
			| (plan.presentationEffects ? 128u : 0u) | (plan.temporalResolve ? 256u : 0u) | (scaled ? 512u : 0u)
			| (configuration.effects.depthOfField.Active() ? 1024u : 0u)
			| (configuration.effects.lensDirt.Active() ? 2048u : 0u)
			| (configuration.effects.lensFlare.Active() ? 4096u : 0u)
			| (configuration.effects.motionBlur.Active() ? 8192u : 0u);
		const std::uint64_t lightingTopology = (configuration.effects.gtao.Active() ? 1u : 0u)
			| (configuration.effects.reflections.Active() ? 2u : 0u) | (configuration.effects.indirect.Active() ? 4u : 0u);
		const std::uint64_t outputTopology = (configuration.effects.atmosphere.Active() ? 1u : 0u)
			| (configuration.ditherCameraOutput ? 2u : 0u);
		request.passCompatibilityId ^= (topology | (lightingTopology << 14u) | (outputTopology << 17u)) * 0x94d049bb133111ebull;
		if(configuration.custom)
			request.passCompatibilityId ^= configuration.custom->authoringRevision * 0x165667b19e3779f9ull;
		if(configuration.effects.lensDirt.Active())
		{
			// The sampled dirt image is part of the immutable descriptor set.
			const auto& id = configuration.effects.lensDirt.texture;
			configuration.dirtTexture.sourceOwner = ResourceManager::Instance().AcquireTextureSnapshot(id);
			if(!configuration.dirtTexture.sourceOwner || !configuration.dirtTexture.sourceOwner->publicationGeneration)
			{ reason = "The active lens dirt texture is not loaded."; return false; }
			request.passCompatibilityId ^= std::hash<TerraEngine::UUID>{}(id) * 0x8cb92baa3f3d8dd7ull;
			request.passCompatibilityId ^= configuration.dirtTexture.sourceOwner->publicationGeneration * 0xdb4f0b9175ae2165ull;
		}
		if(color.grading)
		{
			// Lookup texture identity is baked into private immutable descriptors.
			const auto hashAsset = [](const TerraEngine::UUID& id) {
				std::uint64_t hash = 1469598103934665603ull;
				for(const auto byte : id) hash = (hash ^ byte) * 1099511628211ull;
				return hash;
			};
			request.passCompatibilityId ^= hashAsset(color.curve) * 0x6eed0e9da4d94a4full;
			request.passCompatibilityId ^= hashAsset(color.lut) * 0xb5ad4eceda1ce2a9ull;
		}
		return true;
	}

	bool SupportsPostProcessPlan(const PostProcessGraphSettings& plan) noexcept
	{
		const bool singleSample = plan.sampleCount == 1u && !plan.coverageAA
			&& (plan.antiAliasingMode == 0u || (plan.spatialAA && plan.antiAliasingMode <= 3u));
		const bool coverage = plan.coverageAA && plan.sampleCount == 4u && plan.antiAliasingMode == 4u && !plan.spatialAA;
		const bool temporal = plan.temporalResolve && plan.sampleCount == 1u && !plan.spatialAA
			&& ((plan.antiAliasingMode == 5u && plan.internalResolutionScale == 1.0f)
				|| (plan.antiAliasingMode == 0u && plan.internalResolutionScale == 0.5f));
		return std::isfinite(plan.fixedExposureCompensation)
			&& (((singleSample || coverage) && plan.internalResolutionScale == 1.0f) || temporal)
			&& (!plan.screenSpaceLighting || (singleSample && plan.internalResolutionScale == 1.0f && !plan.motionBlur))
			&& (!plan.customStages || (singleSample && plan.internalResolutionScale == 1.0f));
	}

	bool PreparePostProcessPasses(PostProcessTargetBundle& targets,
		const PostProcessPassConfiguration& configuration, std::string& reason)
	{
		auto resources = std::make_shared<PostProcessPassResources>();
		// Both camera offscreen and MainRuntimeView subpasses expose color0 plus
		// integer visibility identity. The editor presentation-only pass is not
		// an eligible camera destination for this processing chain.
		resources->overlayColorAttachmentCount = 2u;
		resources->device = targets.device;
		VkAttachmentDescription color{};
		color.format = targets.processing[0].format;
		color.samples = VK_SAMPLE_COUNT_1_BIT;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &reference;
		VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass.attachmentCount = 1;
		pass.pAttachments = &color;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		if(vkCreateRenderPass(targets.device, &pass, nullptr, &resources->processingPass) != VK_SUCCESS)
		{
			reason = "The linear HDR color pass could not be created.";
			return false;
		}
		VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebuffer.renderPass = resources->processingPass;
		framebuffer.attachmentCount = 1;
		framebuffer.width = targets.outputExtent.width;
		framebuffer.height = targets.outputExtent.height;
		framebuffer.layers = 1;
		for(std::uint32_t index = 0; index < targets.processingImageCount; ++index)
		{
			framebuffer.pAttachments = &targets.processing[index].view;
			if(vkCreateFramebuffer(targets.device, &framebuffer, nullptr, &resources->processingFramebuffers[index]) != VK_SUCCESS)
			{
				reason = "A color stage framebuffer could not be prepared.";
				return false;
			}
		}
		if(!CreateLayout(targets.device, targets.descriptorLayout, sizeof(float), VK_SHADER_STAGE_FRAGMENT_BIT, resources->fullscreenLayout))
		{
			reason = "Exposure framebuffer or descriptor layout preparation failed.";
			return false;
		}
		resources->overlayPass = configuration.destinationRenderPass;
		if(!configuration.runtimeDestination)
		{
			if(!CreateOverlayPass(targets.device, configuration, resources->overlayPass))
			{
				reason = "The depth-preserving overlay load pass could not be created.";
				return false;
			}
			resources->ownsOverlayPass = true;
			VkFramebufferCreateInfo overlay{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			overlay.renderPass = resources->overlayPass;
			overlay.attachmentCount = configuration.usesVrs ? 4u : 3u;
			overlay.pAttachments = configuration.destinationViews.data();
			overlay.width = targets.outputExtent.width;
			overlay.height = targets.outputExtent.height;
			overlay.layers = 1;
			if(vkCreateFramebuffer(targets.device, &overlay, nullptr, &resources->overlayFramebuffer) != VK_SUCCESS)
			{
				reason = "The exact color/identity/depth overlay framebuffer could not be created.";
				return false;
			}
		}
		if(!CreatePipeline(targets.device, resources->overlayPass, resources->fullscreenLayout, 2, false, resources->compositePipeline))
		{
			reason = "The installed fullscreen composition shader is unavailable.";
			return false;
		}
		if(configuration.ditherCameraOutput && !CreatePipeline(targets.device, resources->overlayPass,
			resources->fullscreenLayout, 2, false, resources->ditherPipeline, "Shaders/PostProcess/OutputDither.frag.spv"))
		{
			reason = "The installed SDR dithering shader could not prepare for this camera output.";
			return false;
		}
		// Stage pipelines bind the exact input/output of this target. The sampled
		// source always differs from the written image, even with three stages.
		const auto& settings = configuration.color;
		const PostProcessTargetImage* current = &targets.sceneColor;
		std::uint32_t nextImage = 0;
		const auto advance = [&] {
			const auto index = nextImage++ % targets.processingImageCount;
			resources->finalProcessingImage = index;
			current = &targets.processing[index];
			return index;
		};
		CustomViewCreateInfo custom;
		std::vector<std::uint32_t> fullscreen;
		if(configuration.custom)
		{
			// Custom descriptors borrow an independent native image owner, not a
			// pointer back to this bundle. Every possible source stays retained.
			if(!targets.RetainCustomImageSet(reason) || targets.processingImageCount < 2u) return false;
			const auto path = ResourceManager::Instance().ResolveEngineAssetPath("Shaders/PostProcess/PostProcessFullscreen.vert.spv");
			const auto bytes = ResourceManager::Instance().ReadFileBytes(path);
			if(bytes.empty() || bytes.size() % sizeof(std::uint32_t)) return false;
			fullscreen.resize(bytes.size() / sizeof(std::uint32_t));
			std::memcpy(fullscreen.data(), bytes.data(), bytes.size());
			custom.physicalDevice = configuration.physicalDevice;
			custom.device = targets.device;
			custom.prepared = configuration.custom->prepared;
			custom.images = targets.customImages;
			custom.fullscreenVertex = fullscreen;
			custom.imageLayouts.fill(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			for(auto& indices : custom.pingIndices) indices = {1u, 2u};
		}
		const auto planCustom = [&](CustomPostProcessStage stage) {
			if(!configuration.custom) return true;
			const auto ordinal = static_cast<std::size_t>(stage);
			std::array<CustomPostProcessImage, 3> pool;
			for(std::uint32_t index = 0; index < pool.size(); ++index)
			{
				const auto shape = BindCustomPostProcessImageSetImage(targets.customImages, index,
					configuration.custom->scope, configuration.custom->planFrameId, targets.generation,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, reason);
				if(!shape) return false;
				pool[index] = *shape;
			}
			const auto source = current == &targets.sceneColor ? 0u : static_cast<std::uint32_t>(current - targets.processing.data()) + 1u;
			custom.stageSources[ordinal] = pool[source];
			resources->customSources[ordinal] = source;
			CustomViewStagePlan plan;
			if(!SelectCustomPostProcessViewStage(*custom.prepared, stage, pool[source], pool,
				custom.pingIndices[ordinal], plan, reason)) return false;
			if(plan.finalImageIndex)
			{
				const auto index = *plan.finalImageIndex - 1u;
				resources->finalProcessingImage = index;
				current = &targets.processing[index];
				nextImage = (index + 1u) % targets.processingImageCount;
			}
			return true;
		};
		const auto& effects = configuration.effects;
		if(effects.HasScreenSpace())
		{
			const auto image = [&](const PostProcessTargetImage& source) {
				ScreenSpaceInputImage result;
				result.image = source.image; result.view = source.view;
				result.format = source.format; result.usage = source.usage;
				result.extent = source.extent; result.owner = configuration.screenSpaceOwner;
				return result;
			};
			const ScreenSpaceInputDemand demand{effects.gtao.Active(), effects.reflections.Active(),
				effects.indirect.Active(), true, false};
			ScreenSpaceRasterCreateInfo raster;
			raster.physicalDevice = configuration.physicalDevice;
			raster.device = targets.device; raster.allocator = targets.allocator;
			raster.extent = targets.internalExtent;
			raster.owner = configuration.screenSpaceOwner;
			raster.demand = demand;
			raster.sceneColor = image(configuration.sceneColorSource);
			raster.identity = image(configuration.sceneIdentity);
			raster.depth = image(configuration.sceneDepth);
			raster.sceneClear = {{0.0f, 0.0f, 0.0f, 1.0f}};
			auto prepared = PrepareScreenSpaceRasterResources(raster);
			if(!prepared.resources) { reason = std::move(prepared.reason); return false; }
			resources->screenRaster = std::move(prepared.resources);
			resources->retireScreenRaster = configuration.retireScreenRaster;
			resources->screenRasterContext = configuration.screenRasterContext;
			const auto index = advance();
			ScreenSpaceLightingCreateInfo lighting;
			lighting.physicalDevice = configuration.physicalDevice;
			lighting.device = targets.device; lighting.allocator = targets.allocator;
			lighting.demand = demand; lighting.raster = resources->screenRaster;
			lighting.output = image(targets.processing[index]);
			resources->screenLighting = PrepareScreenSpaceLightingPass(lighting, reason);
			if(!resources->screenLighting) return false;
		}
		resources->temporalReconstruction = configuration.needsTemporal;
		if(configuration.needsTemporal || effects.motionBlur.Active())
		{
			const auto* source = current;
			const auto index = advance();
			auto depth = configuration.sceneDepth;
			if(configuration.needsScaledCamera)
			{
				resources->scaledCamera = PrepareScaledCameraTargets(configuration.physicalDevice,
					targets.device, targets.allocator, configuration.cameraRasterPass,
					targets.sceneColor.format, depth.format, targets.internalExtent, reason);
				if(!resources->scaledCamera) return false;
				depth = resources->scaledCamera->depth;
			}
			resources->temporalView = PrepareTemporalViewResources(configuration.physicalDevice, targets,
				*source, depth, targets.processing[index], effects.motionBlur.Active(), reason);
			if(!resources->temporalView) return false;
			resources->temporalView->resolveOutput = index;
			resources->temporalFrameState = resources->temporalView->frameState;
		}
		if(!planCustom(CustomPostProcessStage::HdrScene)) return false;
		// The participating medium contributes before exposure and bloom. Its
		// depth input is borrowed from the same view as the HDR color source.
		if(effects.atmosphere.Active())
		{
			const auto* source = current;
			const auto index = advance();
			resources->atmosphere = PrepareAtmosphere(targets, *resources, configuration, *source, index);
			if(!resources->atmosphere) { reason = "Volumetric fog needs the current sampled camera depth and HDR output."; return false; }
		}
		if(settings.HasExposure())
		{
			resources->exposureSource = current;
			resources->exposureOutput = advance();
			if(settings.exposure == GraphicsSettings::ExposureMode::Automatic)
			{
				resources->autoExposure = PrepareAutoExposurePass(targets, *resources->exposureSource,
					targets.processing[resources->exposureOutput], reason);
				if(!resources->autoExposure) return false;
			}
			else if(!CreatePipeline(targets.device, resources->processingPass, resources->fullscreenLayout,
				1, false, resources->exposurePipeline))
			{
				reason = "The installed fixed-exposure shader is unavailable.";
				return false;
			}
		}
		if(effects.motionBlur.Active())
		{
			const auto* source = current;
			const auto index = advance();
			const auto& view = resources->temporalView;
			// The depth-only history path runs at native size with zero jitter
			// and zero history blend. Its copied color is safe to borrow here.
			const MotionBlurInputs inputs{*source, configuration.sceneDepth, targets.processing[index],
				view->motion, view->resolve, MotionBlurColorGrid::UnjitteredOutput, VK_SAMPLE_COUNT_1_BIT,
				configuration.backendResourceGeneration, configuration.viewLifetimeGeneration};
			resources->motionBlur = PrepareMotionBlurPass(configuration.physicalDevice, targets, inputs, reason);
			if(!resources->motionBlur) return false;
			resources->motionBlurSource = static_cast<std::uint32_t>(source - targets.processing.data());
			resources->motionBlurOutput = index;
		}
		if(effects.depthOfField.Active())
		{
			const auto* source = current;
			const auto index = advance();
			const DepthOfFieldInputs inputs{*source, configuration.sceneDepth, targets.processing[index],
				VK_SAMPLE_COUNT_1_BIT, configuration.backendResourceGeneration, configuration.viewLifetimeGeneration};
			resources->depthOfField = PrepareDepthOfFieldPass(configuration.physicalDevice, targets, inputs, reason);
			if(!resources->depthOfField) return false;
		}
		if(effects.lensDirt.Active() || effects.lensFlare.Active())
		{
			// Build bright-source radiance from the exposed scene before bloom
			// composition so neither lens effect depends on the bloom toggle.
			resources->lensRadiance = PrepareBloomRadiance(targets, *current, reason);
			if(!resources->lensRadiance) return false;
		}
		if(settings.HasBloom())
		{
			const auto* source = current;
			const auto index = advance();
			resources->bloom = PrepareBloomPass(targets, *source, targets.processing[index], resources->processingPass,
				resources->processingFramebuffers[index], reason);
			if(!resources->bloom) return false;
		}
		if(effects.lensDirt.Active())
		{
			const auto* source = current;
			const auto index = advance();
			resources->lensDirt = PrepareLensDirtPass(configuration.physicalDevice, targets, *source,
				{ReadBloomRadianceTarget(*resources->lensRadiance), resources->lensRadiance}, configuration.dirtTexture,
				targets.processing[index], resources->processingPass, resources->processingFramebuffers[index], reason);
			if(!resources->lensDirt) return false;
		}
		if(effects.lensFlare.Active())
		{
			const auto* source = current;
			const auto index = advance();
			resources->lensFlare = PrepareLensFlarePass(configuration.physicalDevice, targets, *source,
				{ReadBloomRadianceTarget(*resources->lensRadiance), resources->lensRadiance},
				targets.processing[index], resources->processingPass, resources->processingFramebuffers[index], reason);
			if(!resources->lensFlare) return false;
		}
		if(settings.HasToneAndGrading())
		{
			const auto* source = current;
			const auto index = advance();
			resources->lookups = configuration.lookups;
			resources->tone = PrepareTonePass(targets, *source, targets.processing[index], resources->processingPass,
				resources->processingFramebuffers[index],
				resources->lookups ? resources->lookups->views : ToneLookupTextures{}, reason);
			if(!resources->tone) return false;
		}
		using Mode = GraphicsSettings::GraphicsAntiAliasingMode;
		if(!planCustom(CustomPostProcessStage::DisplayImage)) return false;
		if(configuration.antiAliasing != Mode::Off)
		{
			// The input is the same finished display-linear image for all modes.
			// The legacy tone operator remains an identity, including for AA-only.
			const auto* source = current;
			const auto index = advance();
			auto& destination = targets.processing[index];
			switch(configuration.antiAliasing)
			{
			case Mode::FXAA:
				resources->fxaa = PrepareFxaaPass(configuration.physicalDevice, targets, *source, destination,
					resources->processingPass, resources->processingFramebuffers[index], reason);
				if(!resources->fxaa) return false;
				break;
			case Mode::SMAA:
				resources->smaa = PrepareSmaaPass(configuration.physicalDevice, targets, *source, destination,
					resources->processingPass, resources->processingFramebuffers[index], reason);
				if(!resources->smaa) return false;
				break;
			case Mode::CMAA2:
				resources->cmaa2 = PrepareCmaa2Pass(configuration.physicalDevice, targets, *source, destination, reason);
				if(!resources->cmaa2) return false;
				break;
			default:
				reason = "The requested spatial AA pipeline is not implemented.";
				return false;
			}
		}
		if(configuration.needsGrid
			&& (!CreateLayout(targets.device, VK_NULL_HANDLE, sizeof(glm::mat4) * 4u, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, resources->gridLayout)
				|| !CreatePipeline(targets.device, resources->overlayPass, resources->gridLayout, 2, true, resources->gridPipeline)))
		{
			reason = "The non-depth-writing scene overlay pipeline is unavailable.";
			return false;
		}
		if(configuration.needsPresentationEffects)
		{
			const auto* source = current;
			const auto index = advance();
			resources->lensPresentation = PrepareLensPresentationPass(configuration.physicalDevice, targets, *source,
				targets.processing[index], resources->processingPass, resources->processingFramebuffers[index], reason);
			if(!resources->lensPresentation) return false;
		}
		if(!planCustom(CustomPostProcessStage::BeforeUI)) return false;
		if(configuration.custom)
		{
			resources->customView = PrepareCustomPostProcessView(custom, reason);
			if(!resources->customView) return false;
			resources->customConfiguration = configuration.custom;
		}
		targets.passes = std::move(resources);
		return true;
	}

	bool RecordPostProcessChain(VkCommandBuffer command, PostProcessTargetBundle& targets,
		VkImage source, const GraphicsSettings::PostProcessColorSettings& color, float deltaTimeSeconds,
		PostProcessStageMarker markStage, void* markerContext,
		const GraphicsSettings::PostProcessEffectSettings& effects, std::uint32_t seed,
		const TemporalChainRecording* temporal, const std::shared_ptr<const RenderFramePublication>& publication,
		std::uint32_t presentationIndex)
	{
		auto& raw = targets.sceneColor;
		auto& processed = targets.processing[0];
		// Screen inputs are stamped from the matching native geometry pass
		// after it has ended on this command buffer.
		if(targets.passes->screenLighting)
		{
			const auto& raster = *targets.passes->screenRaster;
			if(!publication || source != raster.colors[0].image) return false;
			ScreenSpaceLightingFrameInput frame;
			frame.publication = publication; frame.presentationIndex = presentationIndex;
			auto& produced = frame.geometry;
			produced.command = command; produced.frameId = publication->frameId;
			produced.owner = raster.owner; produced.samples = VK_SAMPLE_COUNT_1_BIT; produced.variableRateShading = false;
			produced.originalAo = raster.demand.ambientOcclusion ? ScreenSpaceOriginalAo::LegacyScreenAoOmitted
				: ScreenSpaceOriginalAo::LegacyScreenAoPresent;
			produced.colors = raster.colors;
			for(std::uint32_t index = 0; index < produced.colors.size(); ++index)
			{
				if(!produced.colors[index].image) continue;
				produced.colors[index].producedFrameId = produced.frameId;
				produced.colorStates[index] = {index < 2u ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
			}
			produced.depth = raster.depth;
			produced.depth.producedFrameId = produced.frameId;
			produced.depthState = {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
			frame.outputState = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u};
			struct Marker { PostProcessStageMarker record; void* context; } forward{markStage, markerContext};
			const auto marker = +[](void* opaque, ScreenSpaceLightingStage stage, bool begin) {
				const auto& forward = *static_cast<const Marker*>(opaque);
				const auto nativeStage = stage == ScreenSpaceLightingStage::DepthHierarchy ? PostProcessStage::ScreenDepthHierarchy
					: static_cast<PostProcessStage>(static_cast<std::uint32_t>(PostProcessStage::ScreenAO) + static_cast<std::uint32_t>(stage) - 1u);
				if(forward.record) forward.record(forward.context, nativeStage, begin);
			};
			ScreenSpaceLightingRecordedUse lighting;
			if(RecordScreenSpaceLightingPass(command, *targets.passes->screenLighting, effects, frame, lighting, {}, marker, &forward)
				!= ScreenSpaceLightingRecordResult::Recorded) return false;
		}
		const auto originalSourceLayout = targets.passes->screenLighting
			? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		Transition(command, source, originalSourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		// Both destinations are completely overwritten. Discard their previous
		// contents rather than depending on layouts from a primary that might
		// later be rejected. The execution dependency still waits for all earlier
		// readers on this queue, including native capture copies.
		Transition(command, raw.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = {targets.internalExtent.width, targets.internalExtent.height, 1};
		vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, raw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		Transition(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		Transition(command, raw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		raw.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		const auto mark = [&](PostProcessStage stage, bool begin) {
			if(markStage) markStage(markerContext, stage, begin);
		};
		std::shared_ptr<const CustomPostProcessViewFrame> customFrame;
		if(const auto& custom = targets.passes->customConfiguration)
		{
			if(!publication) return false;
			CustomViewFrameRequest frame;
			frame.frameId = publication->frameId;
			frame.authoringRevision = custom->authoringRevision;
			frame.scope = custom->scope;
			std::string reason;
			for(std::size_t stage = 0; stage < frame.stageSources.size(); ++stage)
			{
				const auto shape = BindCustomPostProcessImageSetImage(targets.customImages, targets.passes->customSources[stage],
					frame.scope, frame.frameId, targets.generation, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, reason);
				if(!shape) return false;
				frame.stageSources[stage] = *shape;
			}
			std::vector<CustomChainParameterSnapshot> parameters;
			parameters.reserve(custom->prepared->passes.size());
			for(const auto& pass : custom->prepared->passes)
				if(pass.enabled) parameters.push_back({pass.descriptor.assetId, pass.descriptor.revision, pass.values});
			frame.parameters = parameters;
			customFrame = PrepareCustomPostProcessViewFrame(targets.passes->customView, frame, reason);
			if(!customFrame)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Custom camera recording could not retain its pass inputs: " << reason);
				return false;
			}
		}
		const auto recordCustom = [&](CustomPostProcessStage insertion, PostProcessStage timing) {
			if(!customFrame) return true;
			const auto& prepared = targets.passes->customConfiguration->prepared->passes;
			if(std::none_of(prepared.begin(), prepared.end(), [insertion](const auto& pass) {
				return pass.enabled && pass.descriptor.stage == insertion;
			})) return true;
			CustomChainStageRecordedUse output;
			mark(timing, true);
			const auto result = RecordCustomPostProcessViewStage(command, *customFrame, insertion, output);
			mark(timing, false);
			return result == CustomChainRecordResult::Recorded && output.output.has_value();
		};
		if(const auto& view = targets.passes->temporalView)
		{
			if(!temporal || !temporal->motion || !temporal->frame.provenance.current) return false;
			const auto& plan = temporal->frame;
			const auto& jitter = temporal->motion->prepared.shader.jitterUV;
			TemporalRecordRequest request;
			request.recordingId = request.historyToken = plan.provenance.current->recordingId;
			request.frameId = plan.provenance.current->frameId;
			request.writeSlot = plan.writeSlot;
			request.readSlot = plan.readSlot;
			request.previous = plan.previous;
			request.history = plan.layouts;
			constexpr auto shaderRead = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			request.sceneColor = request.motion = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, shaderRead, VK_ACCESS_SHADER_READ_BIT};
			request.sceneDepth = {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
			// Reconstruct over the entire output, while preserving execution
			// ordering against earlier frame readers of the same image.
			request.output = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u};
			request.currentJitterUV = {jitter.x, jitter.y};
			request.previousJitterUV = {jitter.z, jitter.w};
			request.sharpenStrength = temporal->sharpening;
			if(!targets.passes->temporalReconstruction) request.historyWeight = 0.0f;
			TemporalRecordedUse recorded;
			mark(targets.passes->temporalReconstruction ? PostProcessStage::Temporal : PostProcessStage::MotionDepthHistory, true);
			if(!RecordTemporalPass(command, *view->resolve, request, recorded)
				|| !view->frameState->Record(command, recorded, temporal->motion))
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Temporal reconstruction could not record its exact image and history owners.");
				return false;
			}
			mark(targets.passes->temporalReconstruction ? PostProcessStage::Temporal : PostProcessStage::MotionDepthHistory, false);
			targets.processing[view->resolveOutput].layout = recorded.outputState.layout;
		}
		if(!recordCustom(CustomPostProcessStage::HdrScene, PostProcessStage::CustomHdr)) return false;
		if(const auto& atmosphere = targets.passes->atmosphere)
		{
			if(!publication || presentationIndex >= publication->presentationInputs.size()) return false;
			mark(PostProcessStage::Atmosphere, true);
			RecordAtmosphere(command, targets, *atmosphere, publication->presentationInputs[presentationIndex], effects.atmosphere);
			mark(PostProcessStage::Atmosphere, false);
		}
		if(targets.passes->autoExposure)
		{
			mark(PostProcessStage::Exposure, true);
			// A recreated target owns zeroed metering history. Recording a
			// discarded primary cannot advance the GPU-owned state.
			RecordAutoExposurePass(command, *targets.passes->autoExposure, color, deltaTimeSeconds, false);
			targets.processing[targets.passes->exposureOutput].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			mark(PostProcessStage::Exposure, false);
		}
		else if(targets.passes->exposurePipeline)
		{
			mark(PostProcessStage::Exposure, true);
			auto& output = targets.processing[targets.passes->exposureOutput];
			Transition(command, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			begin.renderPass = targets.passes->processingPass;
			begin.framebuffer = targets.passes->processingFramebuffers[targets.passes->exposureOutput];
			begin.renderArea.extent = targets.outputExtent;
			vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
			Viewport(command, targets.outputExtent);
			Fullscreen(command, *targets.passes, targets.passes->exposurePipeline, targets.passes->exposureSource->descriptor, color.compensationEV);
			vkCmdEndRenderPass(command);
			output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			Transition(command, output.image, output.layout, output.layout,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			mark(PostProcessStage::Exposure, false);
		}
		const auto frameId = publication ? publication->frameId : 0u;
		constexpr auto shaderStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		if(targets.passes->motionBlur)
		{
			const auto& view = *targets.passes->temporalView;
			auto& destination = targets.processing[targets.passes->motionBlurOutput];
			MotionBlurFrameInput input;
			input.motionOwner = temporal->motion;
			input.previous = temporal->frame.previous;
			input.sceneFrameId = input.depthFrameId = input.motionFrameId = frameId;
			input.motionProducer = view.motion.get();
			input.sceneColor = input.totalMotion = input.cameraMotion = {
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, shaderStages, VK_ACCESS_SHADER_READ_BIT};
			input.sceneDepth = {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
			input.previousDepth = {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
			input.output = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u};
			MotionBlurRecordedUse recorded;
			if(input.previous) mark(PostProcessStage::MotionBlur, true);
			const auto result = RecordMotionBlurPass(command, *targets.passes->motionBlur, effects.motionBlur, input, recorded);
			if(result == MotionBlurRecordResult::Rejected) return false;
			if(result != MotionBlurRecordResult::Recorded)
			{
				// No previous depth on the first frame after a reset. Preserve
				// the exact upstream image for the following already-bound stages.
				auto& sourceImage = targets.processing[targets.passes->motionBlurSource];
				Transition(command, sourceImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
					shaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
				Transition(command, destination.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				vkCmdCopyImage(command, sourceImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
				Transition(command, sourceImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, shaderStages);
				Transition(command, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, shaderStages);
			}
			destination.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if(input.previous) mark(PostProcessStage::MotionBlur, false);
		}
		if(targets.passes->depthOfField)
		{
			DepthOfFieldFrameInput input;
			input.publication = publication;
			input.presentationIndex = presentationIndex;
			input.sceneFrameId = input.depthFrameId = frameId;
			input.sceneColor = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, shaderStages, VK_ACCESS_SHADER_READ_BIT};
			input.sceneDepth = {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
			input.output = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u};
			DepthOfFieldRecordedUse recorded;
			mark(PostProcessStage::DepthOfField, true);
			if(RecordDepthOfFieldPass(command, *targets.passes->depthOfField, effects.depthOfField, input, recorded)
				!= DepthOfFieldRecordResult::Recorded) return false;
			mark(PostProcessStage::DepthOfField, false);
		}
		LensHdrRecordContext lens;
		if(const auto& radiance = targets.passes->lensRadiance)
		{
			GraphicsSettings::PostProcessColorSettings radianceSettings;
			// Flare gates its own contribution with its live threshold. Keep
			// every positive source lane available in the filtered radiance.
			radianceSettings.bloomThreshold = effects.lensFlare.Active() ? 0.0f : 1.0f;
			mark(PostProcessStage::LensRadiance, true);
			RecordBloomPass(command, *radiance, radianceSettings);
			mark(PostProcessStage::LensRadiance, false);
			lens.sceneFrameId = lens.producedRadianceFrameId = frameId;
			lens.radianceProducer = radiance.get();
			lens.producedRadianceImage = ReadBloomRadianceTarget(*radiance).image;
			lens.scene = lens.radiance = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, shaderStages, VK_ACCESS_SHADER_READ_BIT};
			lens.dirt = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
			lens.output = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u};
		}
		if(targets.passes->bloom)
		{
			mark(PostProcessStage::Bloom, true);
			RecordBloomPass(command, *targets.passes->bloom, color);
			mark(PostProcessStage::Bloom, false);
		}
		if(targets.passes->lensDirt)
		{
			LensHdrRecordedUse recorded;
			mark(PostProcessStage::LensDirt, true);
			if(RecordLensDirtPass(command, *targets.passes->lensDirt, effects.lensDirt, lens, recorded)
				!= LensHdrRecordResult::Recorded) return false;
			mark(PostProcessStage::LensDirt, false);
		}
		if(targets.passes->lensFlare)
		{
			LensHdrRecordedUse recorded;
			mark(PostProcessStage::LensFlare, true);
			if(RecordLensFlarePass(command, *targets.passes->lensFlare, effects.lensFlare, lens, recorded)
				!= LensHdrRecordResult::Recorded) return false;
			mark(PostProcessStage::LensFlare, false);
		}
		if(targets.passes->tone)
		{
			mark(PostProcessStage::ToneAndGrade, true);
			RecordTonePass(command, *targets.passes->tone, color);
			mark(PostProcessStage::ToneAndGrade, false);
		}
		if(!recordCustom(CustomPostProcessStage::DisplayImage, PostProcessStage::CustomDisplay)) return false;
		if(targets.passes->fxaa || targets.passes->smaa || targets.passes->cmaa2)
		{
			mark(PostProcessStage::SpatialAA, true);
			if(targets.passes->fxaa) RecordFxaaPass(command, *targets.passes->fxaa);
			else if(targets.passes->smaa) RecordSmaaPass(command, *targets.passes->smaa);
			else RecordCmaa2Pass(command, *targets.passes->cmaa2);
			mark(PostProcessStage::SpatialAA, false);
		}
		if(!targets.passes->autoExposure && !targets.passes->exposurePipeline && !targets.passes->bloom
			&& !targets.passes->tone && !targets.passes->fxaa && !targets.passes->smaa && !targets.passes->cmaa2
			&& !targets.passes->lensPresentation && !targets.passes->temporalView
			&& !targets.passes->depthOfField && !targets.passes->lensDirt && !targets.passes->lensFlare
			&& !targets.passes->screenLighting && !targets.passes->atmosphere && !customFrame)
		{
			// Coverage alone has the same raw and processed pixels. Preserve a
			// separate immutable processed image for the normal readback API.
			Transition(command, raw.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			Transition(command, processed.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			vkCmdCopyImage(command, raw.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, processed.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			Transition(command, raw.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
			Transition(command, processed.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
		}
		if(targets.passes->lensPresentation)
		{
			const auto& lens = effects.presentation;
			LensPresentationControls controls;
			controls.vignetteEnabled = lens.vignetteEnabled;
			controls.grainEnabled = lens.grainEnabled;
			controls.chromaticEnabled = lens.chromaticEnabled;
			controls.distortionEnabled = lens.distortionEnabled;
			controls.vignetteIntensity = lens.vignetteIntensity;
			controls.vignetteRadius = lens.vignetteRadius;
			controls.vignetteSoftness = lens.vignetteSoftness;
			controls.grainIntensity = lens.grainIntensity;
			controls.grainResponse = lens.grainResponse;
			controls.chromaticPixels = lens.chromaticPixels;
			controls.distortionK1 = lens.distortionK1;
			controls.distortionK2 = lens.distortionK2;
			mark(PostProcessStage::Presentation, true);
			if(!RecordLensPresentationPass(command, *targets.passes->lensPresentation, controls, seed)) return false;
			mark(PostProcessStage::Presentation, false);
		}
		if(!recordCustom(CustomPostProcessStage::BeforeUI, PostProcessStage::CustomBeforeUI)) return false;
		targets.processing[targets.passes->finalProcessingImage].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		return true;
	}

	void BeginPostProcessComposite(VkCommandBuffer command, PostProcessTargetBundle& targets,
		VkFramebuffer destination, VkExtent2D extent, bool rawSource)
	{
		VkMemoryBarrier preservedInputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		preservedInputs.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		preservedInputs.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			0, 1, &preservedInputs, 0, nullptr, 0, nullptr);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = targets.passes->overlayPass;
		begin.framebuffer = targets.passes->overlayFramebuffer != VK_NULL_HANDLE
			? targets.passes->overlayFramebuffer : destination;
		begin.renderArea.extent = extent;
		// Runtime destination uses its original clear pass; the offscreen load
		// pass ignores these values and preserves geometry identity and depth.
		std::array<VkClearValue, 4> clears{};
		clears[2].depthStencil = {1.0f, 0};
		begin.clearValueCount = static_cast<std::uint32_t>(clears.size());
		begin.pClearValues = clears.data();
		vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
		Viewport(command, extent);
		const auto pipeline = !rawSource && targets.passes->ditherPipeline ? targets.passes->ditherPipeline : targets.passes->compositePipeline;
		Fullscreen(command, *targets.passes, pipeline,
			rawSource ? targets.sceneColor.descriptor : targets.processing[targets.passes->finalProcessingImage].descriptor, 0.0f);
	}

	void RecordPostProcessGrid(VkCommandBuffer command, const PostProcessTargetBundle& targets,
		const RenderViewPresentationInput& presentation, VkExtent2D extent)
	{
		if(!targets.passes->gridPipeline) return;
		const auto origin = LocalizeRenderPosition(Scene::WorldPosition{}, presentation.referenceFrame);
		if(!origin.Succeeded()) return;
		struct GridConstants { glm::mat4 view, projection, inverseView, inverseProjection; };
		GridConstants constants;
		constants.view = presentation.viewMatrix * glm::translate(glm::mat4(1.0f), origin.offsetMetres);
		constants.projection = presentation.projectionMatrix;
		constants.inverseView = glm::inverse(constants.view);
		constants.inverseProjection = glm::inverse(constants.projection);
		Viewport(command, extent);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, targets.passes->gridPipeline);
		vkCmdPushConstants(command, targets.passes->gridLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
		vkCmdDraw(command, 6, 1, 0, 0);
	}

	void EndPostProcessComposite(VkCommandBuffer command, bool runtimeDestination, VkImage destination)
	{
		vkCmdEndRenderPass(command);
		if(!runtimeDestination)
		{
			Transition(command, destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}
	}
}
