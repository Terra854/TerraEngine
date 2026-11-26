// VulkanPostProcessBloom.cpp - Isolated, normalized multiresolution HDR bloom.
#include "VulkanPostProcessBloom.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct BloomImage
		{
			VkImage image = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkExtent2D extent{};
		};
		struct DownConstants { std::uint32_t source[2], destination[2]; float threshold, knee; std::uint32_t flags, reserved; };
		struct UpConstants { std::uint32_t low[2], high[2]; float spread, radius; std::uint32_t flags, reserved; };
		struct CompositeConstants { float intensity; std::uint32_t enabled, size[2]; };
		static_assert(sizeof(DownConstants) == 32 && sizeof(UpConstants) == 32 && sizeof(CompositeConstants) == 16);

		struct ShaderModule
		{
			VkDevice device;
			VkShaderModule module = VK_NULL_HANDLE;
			ShaderModule(VkDevice owner, const char* path) : device(owner)
			{
				const auto bytes = ResourceManager::Instance().ReadFileBytes(ResourceManager::Instance().ResolveEngineAssetPath(path));
				if (bytes.empty() || bytes.size() % sizeof(std::uint32_t)) return;
				std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
				std::memcpy(words.data(), bytes.data(), bytes.size());
				VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
				info.codeSize = bytes.size(); info.pCode = words.data();
				vkCreateShaderModule(device, &info, nullptr, &module);
			}
			~ShaderModule() { if (module) vkDestroyShaderModule(device, module, nullptr); }
		};

		bool CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, BloomImage& image)
		{
			image.extent = extent;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
			info.extent = {extent.width, extent.height, 1};
			info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo allocation{};
			allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			if (vmaCreateImage(allocator, &info, &allocation, &image.image, &image.allocation, nullptr) != VK_SUCCESS) return false;
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			return vkCreateImageView(device, &view, nullptr, &image.view) == VK_SUCCESS;
		}

		bool CreateLayout(VkDevice device, std::span<const VkDescriptorType> types, VkShaderStageFlags stages,
			std::uint32_t pushSize, VkDescriptorSetLayout& descriptors, VkPipelineLayout& pipeline)
		{
			std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
			for (std::uint32_t i = 0; i < types.size(); ++i) bindings[i] = {i, types[i], 1, stages, nullptr};
			VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			set.bindingCount = static_cast<std::uint32_t>(types.size()); set.pBindings = bindings.data();
			if (vkCreateDescriptorSetLayout(device, &set, nullptr, &descriptors) != VK_SUCCESS) return false;
			VkPushConstantRange push{stages, 0, pushSize};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout.setLayoutCount = 1; layout.pSetLayouts = &descriptors;
			layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
			return vkCreatePipelineLayout(device, &layout, nullptr, &pipeline) == VK_SUCCESS;
		}

		bool CreateCompute(VkDevice device, const char* shaderPath, VkPipelineLayout layout, VkPipeline& pipeline)
		{
			ShaderModule shader(device, shaderPath);
			if (!shader.module) return false;
			VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = shader.module; info.stage.pName = "main";
			info.layout = layout;
			return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS;
		}

		bool CreateComposite(VkDevice device, VkRenderPass renderPass, VkPipelineLayout layout, VkPipeline& pipeline)
		{
			ShaderModule vertex(device, "Shaders/PostProcess/PostProcessFullscreen.mesh.spv");
			ShaderModule fragment(device, "Shaders/PostProcess/BloomComposite.frag.spv");
			if (!vertex.module || !fragment.module) return false;
			std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
			for (auto& stage : stages) stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT; stages[0].module = vertex.module; stages[0].pName = "main";
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment.module; stages[1].pName = "PSMain";
			VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
			viewport.viewportCount = 1; viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
			raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1.0f;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
			samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
			VkPipelineColorBlendAttachmentState attachment{};
			attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			// The shader performs addition and copies scene alpha; fixed-function
			// blending is disabled so alpha and intensity are never applied twice.
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
			blend.attachmentCount = 1; blend.pAttachments = &attachment;
			const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
			VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
			dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamicStates.data();
			VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			info.stageCount = 2; info.pStages = stages.data(); info.pViewportState = &viewport; info.pRasterizationState = &raster;
			info.pMultisampleState = &samples; info.pDepthStencilState = &depth; info.pColorBlendState = &blend;
			info.pDynamicState = &dynamic; info.layout = layout; info.renderPass = renderPass;
			return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS;
		}

		void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
			VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess, VkPipelineStageFlags destinationStage, VkAccessFlags destinationAccess)
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = destinationAccess;
			barrier.oldLayout = oldLayout; barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
	}

	struct BloomPass
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		PostProcessTargetImage source;
		PostProcessTargetImage* destination = nullptr;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		std::array<VkDescriptorSetLayout, 3> descriptorLayouts{};
		std::array<VkPipelineLayout, 3> pipelineLayouts{};
		std::array<VkPipeline, 3> pipelines{};
		std::vector<BloomImage> down, up;
		std::vector<VkDescriptorSet> downSets, upSets;
		VkDescriptorSet compositeSet = VK_NULL_HANDLE;

		~BloomPass()
		{
			// The bundle retains this owner through its actual borrowers. Descriptor
			// and pipeline owners disappear before the image views they reference.
			for (const auto pipeline : pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
			for (const auto layout : pipelineLayouts) if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
			if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
			for (const auto layout : descriptorLayouts) if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
			if (sampler) vkDestroySampler(device, sampler, nullptr);
			const auto destroy = [&](const BloomImage& image) {
				if (image.view) vkDestroyImageView(device, image.view, nullptr);
				if (image.image) vmaDestroyImage(allocator, image.image, image.allocation);
			};
			for (const auto& image : down) destroy(image);
			for (const auto& image : up) destroy(image);
		}
	};

	namespace
	{
		bool AllocateSet(BloomPass& pass, std::uint32_t layout, VkDescriptorSet& set)
		{
			VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			info.descriptorPool = pass.pool; info.descriptorSetCount = 1; info.pSetLayouts = &pass.descriptorLayouts[layout];
			return vkAllocateDescriptorSets(pass.device, &info, &set) == VK_SUCCESS;
		}
		void WriteImages(BloomPass& pass, VkDescriptorSet set, std::span<const VkDescriptorType> types,
			std::span<const VkImageView> images)
		{
			std::array<VkDescriptorImageInfo, 3> infos{};
			std::array<VkWriteDescriptorSet, 3> writes{};
			for (std::uint32_t i = 0; i < types.size(); ++i) {
				infos[i].sampler = types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? pass.sampler : VK_NULL_HANDLE;
				infos[i].imageView = images[i];
				infos[i].imageLayout = types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
				writes[i].descriptorType = types[i]; writes[i].pImageInfo = &infos[i];
			}
			vkUpdateDescriptorSets(pass.device, static_cast<std::uint32_t>(types.size()), writes.data(), 0, nullptr);
		}
	}

	static std::shared_ptr<BloomPass> PrepareBloomResources(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& exposedSource, PostProcessTargetImage* destination, VkRenderPass processingPass,
		VkFramebuffer bloomDestination, std::string& reason)
	{
		reason.clear();
		if (!targets.device || !targets.allocator || !exposedSource.image || !exposedSource.view
			|| exposedSource.extent.width == 0 || exposedSource.extent.height == 0
			|| (destination && (!destination->image || !destination->view || !processingPass || !bloomDestination
				|| exposedSource.image == destination->image || exposedSource.extent.width != destination->extent.width
				|| exposedSource.extent.height != destination->extent.height || exposedSource.format != destination->format))
			|| (exposedSource.format != VK_FORMAT_R16G16B16A16_SFLOAT && exposedSource.format != VK_FORMAT_R32G32B32A32_SFLOAT)) {
			reason = "Bloom requires a sampled linear HDR source and a compatible destination when compositing.";
			return {};
		}
		try {
			auto pass = std::make_shared<BloomPass>();
			pass->device = targets.device; pass->allocator = targets.allocator;
			pass->source = exposedSource; pass->destination = destination;
			pass->renderPass = processingPass; pass->framebuffer = bloomDestination;
			// Preparation owns every partial allocation. Failure destroys only this
			// candidate and cannot replace a previously admitted finished-image owner.
			reason = "Bloom mip, descriptor or pipeline preparation failed before publication.";
			VkExtent2D extent = exposedSource.extent;
			do {
				extent = {std::max(1u, extent.width / 2u + extent.width % 2u), std::max(1u, extent.height / 2u + extent.height % 2u)};
				pass->down.emplace_back();
				if (!CreateImage(pass->device, pass->allocator, exposedSource.format, extent, pass->down.back())) return {};
			} while (pass->down.size() < 6u && (extent.width > 1u || extent.height > 1u));
			pass->up.resize(pass->down.size() - 1u);
			for (std::size_t i = 0; i < pass->up.size(); ++i)
				if (!CreateImage(pass->device, pass->allocator, exposedSource.format, pass->down[i].extent, pass->up[i])) return {};
			VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
			sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			if (vkCreateSampler(pass->device, &sampler, nullptr, &pass->sampler) != VK_SUCCESS) return {};
			const std::array downTypes{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
			const std::array upTypes{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
			const std::array compositeTypes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
			if (!CreateLayout(pass->device, downTypes, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(DownConstants), pass->descriptorLayouts[0], pass->pipelineLayouts[0])
				|| !CreateLayout(pass->device, upTypes, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(UpConstants), pass->descriptorLayouts[1], pass->pipelineLayouts[1])
				|| (destination && !CreateLayout(pass->device, compositeTypes, VK_SHADER_STAGE_FRAGMENT_BIT,
					sizeof(CompositeConstants), pass->descriptorLayouts[2], pass->pipelineLayouts[2]))) return {};
			const bool wide = exposedSource.format == VK_FORMAT_R32G32B32A32_SFLOAT;
			if (!CreateCompute(pass->device, wide ? "Shaders/PostProcess/BloomDownsample32.comp.spv" : "Shaders/PostProcess/BloomDownsample.comp.spv", pass->pipelineLayouts[0], pass->pipelines[0])
				|| !CreateCompute(pass->device, wide ? "Shaders/PostProcess/BloomUpsample32.comp.spv" : "Shaders/PostProcess/BloomUpsample.comp.spv", pass->pipelineLayouts[1], pass->pipelines[1])
				|| (destination && !CreateComposite(pass->device, processingPass, pass->pipelineLayouts[2], pass->pipelines[2]))) return {};
			const auto count = static_cast<std::uint32_t>(pass->down.size());
			const std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 2u},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count * 2u - 1u}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool.maxSets = count * 2u; pool.poolSizeCount = 3; pool.pPoolSizes = sizes.data();
			if (vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool) != VK_SUCCESS) return {};
			pass->downSets.resize(count); pass->upSets.resize(count - 1u);
			for (std::size_t i = 0; i < count; ++i) {
				if (!AllocateSet(*pass, 0, pass->downSets[i])) return {};
				const std::array views{i ? pass->down[i - 1u].view : exposedSource.view, pass->down[i].view};
				WriteImages(*pass, pass->downSets[i], downTypes, views);
			}
			for (std::size_t i = 0; i < pass->up.size(); ++i) {
				if (!AllocateSet(*pass, 1, pass->upSets[i])) return {};
				const auto lowView = i + 1u == pass->down.size() - 1u ? pass->down.back().view : pass->up[i + 1u].view;
				const std::array views{lowView, pass->down[i].view, pass->up[i].view};
				WriteImages(*pass, pass->upSets[i], upTypes, views);
			}
			if(destination)
			{
				if (!AllocateSet(*pass, 2, pass->compositeSet)) return {};
				const std::array views{exposedSource.view, ReadBloomRadianceTarget(*pass).view};
				WriteImages(*pass, pass->compositeSet, compositeTypes, views);
			}
			reason.clear();
			return pass;
		} catch (...) { reason = "Bloom preparation failed before candidate publication."; return {}; }
	}

	std::shared_ptr<BloomPass> PrepareBloomPass(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& exposedSource, PostProcessTargetImage& bloomOutput,
		VkRenderPass processingPass, VkFramebuffer bloomDestination, std::string& reason)
	{
		return PrepareBloomResources(targets, exposedSource, &bloomOutput, processingPass, bloomDestination, reason);
	}

	std::shared_ptr<BloomPass> PrepareBloomRadiance(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& exposedSource, std::string& reason)
	{
		return PrepareBloomResources(targets, exposedSource, nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, reason);
	}

	PostProcessTargetImage ReadBloomRadianceTarget(const BloomPass& pass) noexcept
	{
		if(pass.down.empty()) return {};
		const auto& image = pass.up.empty() ? pass.down.front() : pass.up.front();
		// Describe ownership only. The command recorder must write this mip in
		// the same accepted view before a lens pass reads it.
		PostProcessTargetImage target;
		target.image = image.image;
		target.view = image.view;
		target.extent = image.extent;
		target.format = pass.source.format;
		target.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		return target;
	}

	void RecordBloomPass(VkCommandBuffer command, BloomPass& pass, const GraphicsSettings::PostProcessColorSettings& settings)
	{
		// A bypass leaves every image untouched. The owning graph forwards its
		// exposed source and never advertises the destination for this skipped path.
		if (!command || (pass.destination && !settings.HasBloom())) return;
		constexpr auto readers = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		const auto beginWrite = [&](const BloomImage& image) {
			// Contents are discarded each recording, avoiding speculative layout
			// state when a recorded command is rejected. The read-stage dependency
			// still orders prior-frame readers before this write on the same queue.
			Barrier(command, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				readers, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
		};
		const auto finishWrite = [&](const BloomImage& image) {
			Barrier(command, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, readers, VK_ACCESS_SHADER_READ_BIT);
		};
		VkExtent2D source = pass.source.extent;
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelines[0]);
		for (std::size_t i = 0; i < pass.down.size(); ++i) {
			const auto& image = pass.down[i]; beginWrite(image);
			const DownConstants constants{{source.width, source.height}, {image.extent.width, image.extent.height}, settings.bloomThreshold, settings.bloomSoftKnee, i == 0 ? 3u : 1u, 0};
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelineLayouts[0], 0, 1, &pass.downSets[i], 0, nullptr);
			vkCmdPushConstants(command, pass.pipelineLayouts[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
			vkCmdDispatch(command, (image.extent.width + 7u) / 8u, (image.extent.height + 7u) / 8u, 1);
			finishWrite(image); source = image.extent;
		}
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelines[1]);
		for (std::size_t end = pass.up.size(); end > 0; --end) {
			const auto i = end - 1u;
			const auto& image = pass.up[i]; beginWrite(image);
			const UpConstants constants{{source.width, source.height}, {image.extent.width, image.extent.height}, std::clamp(settings.bloomSpread * 0.5f, 0.0f, 1.0f), 1.0f, 1u, 0};
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelineLayouts[1], 0, 1, &pass.upSets[i], 0, nullptr);
			vkCmdPushConstants(command, pass.pipelineLayouts[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
			vkCmdDispatch(command, (image.extent.width + 7u) / 8u, (image.extent.height + 7u) / 8u, 1);
			finishWrite(image); source = image.extent;
		}
		if(!pass.destination) return; // The lens pass owns final composition.
		const auto extent = pass.destination->extent;
		Barrier(command, pass.destination->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			// A previous explicit capture can still be a transfer reader. Discard
			// needs execution ordering against that copy and prior color writes too.
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = pass.renderPass; begin.framebuffer = pass.framebuffer; begin.renderArea.extent = extent;
		vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
		VkRect2D scissor{{0, 0}, extent};
		vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipelines[2]);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipelineLayouts[2], 0, 1, &pass.compositeSet, 0, nullptr);
		const CompositeConstants constants{settings.bloomIntensity, 1u, {extent.width, extent.height}};
		vkCmdPushConstants(command, pass.pipelineLayouts[2], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
		vkCmdDrawMeshTasksEXT(command, 1, 1, 1);
		vkCmdEndRenderPass(command);
		// The supplied processing pass ends in shader-read. Make its color write
		// visible to subsequent tone/grade compute as well as fragment consumers.
		Barrier(command, pass.destination->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			readers | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
		pass.destination->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
}
