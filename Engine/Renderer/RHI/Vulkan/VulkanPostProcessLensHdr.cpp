// VulkanPostProcessLensHdr.cpp - actual bloom-driven HDR lens pass ownership.
#include "VulkanPostProcessLensHdr.h"
#include "VulkanPostProcessBloom.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "Resource/ResourceManager.h"
#include "File Formats/TerraEngineTextureFile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		enum class Kind { Dirt, Flare };
		struct DirtConstants { std::uint32_t output[2]; float intensity; std::uint32_t enabled; float scale[2], offset[2]; };
		struct FlareConstants { std::uint32_t output[2]; float intensity; std::uint32_t enabled; float threshold, spacing, radius, width, tint[3]; std::uint32_t count; };
		static_assert(sizeof(DirtConstants) == 32 && offsetof(DirtConstants, scale) == 16 && offsetof(DirtConstants, offset) == 24);
		static_assert(sizeof(FlareConstants) == 48 && offsetof(FlareConstants, threshold) == 16 && offsetof(FlareConstants, tint) == 32 && offsetof(FlareConstants, count) == 44);
		void Require(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
		bool Extent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool Hdr(VkFormat format) noexcept { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; }
		bool LinearDirt(VkFormat format) noexcept
		{
			switch(format) {
			case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R16G16B16A16_UNORM:
			case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
			case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC2_UNORM_BLOCK:
			case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK: return true;
			default: return false;
			}
		}
		void Qualify(VkPhysicalDevice physical, const PostProcessTargetImage& image, VkFormatFeatureFlags features)
		{
			Require(image.image && image.view && image.extent.width && image.extent.height && image.usage, "HDR lens image metadata is incomplete.");
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			Require((format.optimalTilingFeatures & features) == features, "HDR lens image format lacks required sampled/filter/attachment support.");
			VkImageFormatProperties limits{};
			Require(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
				image.usage, 0, &limits) == VK_SUCCESS, "HDR lens complete image usage is unsupported.");
			Require(image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height && limits.maxExtent.depth >= 1
				&& limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1 && (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "HDR lens image exceeds exact extent/sample limits.");
		}
		bool SameRadiance(const PostProcessTargetImage& a, const PostProcessTargetImage& b) noexcept
		{
			return a.image == b.image && a.view == b.view && a.format == b.format && Extent(a.extent, b.extent) && a.usage == b.usage;
		}
		struct Module
		{
			VkDevice device; VkShaderModule value = VK_NULL_HANDLE;
			explicit Module(VkDevice owner) : device(owner) {}
			~Module() { if(value) vkDestroyShaderModule(device, value, nullptr); }
		};
		// Check the actual installed descriptor kinds and member offsets. No
		// shader string or fallback bytecode is embedded in this native adapter.
		void ValidateLensModule(const std::vector<std::uint32_t>& words, Kind kind)
		{
			Require(words.size() >= 5 && words[0] == 0x07230203u, "HDR lens SPIR-V header is invalid.");
			std::map<std::uint32_t, std::uint32_t> bindings, sets, variables, pointers, images, sampled;
			std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
			std::uint32_t push = 0; bool entry = false;
			for(std::size_t i = 5; i < words.size();) {
				const auto count = words[i] >> 16, op = words[i] & 65535u;
				Require(count && count <= words.size() - i, "HDR lens SPIR-V instruction is truncated.");
				if(op == 17) Require(count == 2 && (words[i + 1] == 1 || words[i + 1] == 50), "HDR lens module requires unqualified optional features.");
				if(op == 15 && count >= 5 && words[i + 1] == 4) {
					const auto* name = reinterpret_cast<const char*>(words.data() + i + 3);
					entry = std::memchr(name, 0, (count - 3) * 4) && std::strcmp(name, "PSMain") == 0;
				}
				if(op == 71 && count == 4 && words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
				if(op == 71 && count == 4 && words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
				if(op == 72 && count == 5 && words[i + 3] == 35) offsets[words[i + 1]][words[i + 2]] = words[i + 4];
				if(op == 59 && count >= 4) { variables[words[i + 2]] = words[i + 1]; if(words[i + 3] == 9) push = words[i + 1]; }
				if(op == 32 && count == 4) pointers[words[i + 1]] = words[i + 3];
				if(op == 25 && count >= 9) images[words[i + 1]] = words[i + 7];
				if(op == 27 && count == 3) sampled[words[i + 1]] = words[i + 2];
				i += count;
			}
			const std::uint32_t expected = kind == Kind::Dirt ? 3u : 2u; std::array<bool, 3> seen{};
			Require(entry && bindings.size() == expected, "HDR lens entry or descriptor count disagrees with the prepared ABI.");
			for(const auto& [id, binding] : bindings) {
				Require(binding < expected && !seen[binding] && sets.contains(id) && sets.at(id) == 0, "HDR lens set/binding is invalid or duplicated."); seen[binding] = true;
				Require(variables.contains(id) && pointers.contains(variables.at(id)), "HDR lens descriptor pointer is missing.");
				auto type = pointers.at(variables.at(id));
				if(binding != 0) { Require(sampled.contains(type), "HDR lens filtered input is not a combined sampler."); type = sampled.at(type); }
				Require(images.contains(type) && images.at(type) == 1, "HDR lens input is not a sampled image.");
			}
			Require(push && pointers.contains(push), "HDR lens push block is missing.");
			const std::vector<std::uint32_t> expectedOffsets = kind == Kind::Dirt ? std::vector<std::uint32_t>{0, 8, 12, 16, 24} : std::vector<std::uint32_t>{0, 8, 12, 16, 20, 24, 28, 32, 44};
			const auto& actual = offsets[pointers.at(push)]; Require(actual.size() == expectedOffsets.size(), "HDR lens push member count changed.");
			for(std::uint32_t i = 0; i < expectedOffsets.size(); ++i) Require(actual.contains(i) && actual.at(i) == expectedOffsets[i], "HDR lens push offsets changed.");
		}
		void LoadModule(Module& module, const char* path, const Kind* lensKind = nullptr)
		{
			auto& resources = ResourceManager::Instance(); const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			Require(!bytes.empty() && bytes.size() % 4 == 0, "Installed HDR lens shader is missing.");
			std::vector<std::uint32_t> words(bytes.size() / 4); std::memcpy(words.data(), bytes.data(), bytes.size());
			Require(words.size() >= 5 && words[0] == 0x07230203u, "HDR lens shader header is invalid."); if(lensKind) ValidateLensModule(words, *lensKind);
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = bytes.size(); info.pCode = words.data();
			Require(vkCreateShaderModule(module.device, &info, nullptr, &module.value) == VK_SUCCESS, "HDR lens shader module creation failed.");
		}
		constexpr LensHdrImageState kSampled{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr LensHdrImageState kAttachment{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
		constexpr LensHdrImageState kFinished{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		void Barrier(VkCommandBuffer command, const PostProcessTargetImage& image, LensHdrImageState before, LensHdrImageState after) noexcept
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image.image;
			barrier.oldLayout = before.layout; barrier.newLayout = after.layout; barrier.srcAccessMask = before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : before.access; barrier.dstAccessMask = after.access;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(command, before.stage, after.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
	}

	struct LensHdrPass
	{
		VkDevice device = VK_NULL_HANDLE; Kind kind = Kind::Dirt;
		PostProcessTargetImage scene, output; LensHdrBloomRadiance radiance; LensHdrDirtTexture dirt;
		VkRenderPass renderPass = VK_NULL_HANDLE; VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptors = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE; VkPipeline pipeline = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
		~LensHdrPass()
		{
			// Parent completion/descriptor retirement protects every borrowed image.
			if(pipeline) vkDestroyPipeline(device, pipeline, nullptr); if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			if(layout) vkDestroyPipelineLayout(device, layout, nullptr); if(descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
			if(sampler) vkDestroySampler(device, sampler, nullptr);
			// Strong radiance/dirt owners release after their descriptor borrowers.
		}
	};

	namespace
	{
		std::shared_ptr<LensHdrPass> Prepare(Kind kind, VkPhysicalDevice physical, PostProcessTargetBundle& targets,
			const PostProcessTargetImage& scene, LensHdrBloomRadiance radiance, LensHdrDirtTexture dirt,
			const PostProcessTargetImage& output, VkRenderPass renderPass, VkFramebuffer framebuffer, std::string& reason)
		{
			reason.clear();
			try {
				Require(physical && targets.device && renderPass && framebuffer && radiance.producer, "HDR lens preparation is missing its actual native owners.");
				Require(Hdr(scene.format) && scene.format == output.format && Hdr(radiance.image.format)
					&& Extent(scene.extent, output.extent) && Extent(output.extent, targets.outputExtent), "HDR lens exposed color format or output extent is inconsistent.");
				Require(SameRadiance(ReadBloomRadianceTarget(*radiance.producer), radiance.image), "HDR lens input does not match the actual producer's bloom-only radiance.");
				Require(scene.image != output.image && scene.view != output.view && radiance.image.image != scene.image
					&& radiance.image.image != output.image && radiance.image.view != scene.view && radiance.image.view != output.view, "HDR lens scene/radiance/output images must be distinct.");
				Require((scene.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (radiance.image.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
					&& (output.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (output.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "HDR lens images lack required usages.");
				Qualify(physical, scene, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
				Qualify(physical, radiance.image, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
				Qualify(physical, output, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
				if(kind == Kind::Dirt) {
					Require(dirt.assetId != TerraEngine::EMPTY_UUID && dirt.sourceOwner && dirt.imageOwner && dirt.sourceOwner->publicationGeneration,
						"Lens dirt needs its actual immutable texture and dedicated native image owners.");
					const auto& asset = *dirt.sourceOwner;
					Require((asset.header.flags & (1u | 4u | 8u | 16u)) == 0 && asset.descriptor.semantic == 2 && asset.descriptor.depth == 1
						&& asset.descriptor.layers == 1 && asset.descriptor.faces == 1 && asset.descriptor.mipCount >= 1
						&& asset.descriptor.width == dirt.image.extent.width && asset.descriptor.height == dirt.image.extent.height
						&& LinearDirt(dirt.image.format) && (dirt.image.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Lens dirt texture must be actual linear2D data with matching mip0 dimensions.");
					Require(dirt.image.image != scene.image && dirt.image.image != output.image && dirt.image.image != radiance.image.image
						&& dirt.image.view != scene.view && dirt.image.view != output.view && dirt.image.view != radiance.image.view, "Lens dirt cannot alias camera or bloom images.");
					Qualify(physical, dirt.image, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
				}
				VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties); const auto& limits = properties.limits;
				const auto count = kind == Kind::Dirt ? 3u : 2u;
				Require(properties.apiVersion >= VK_API_VERSION_1_3 && limits.maxPushConstantsSize >= (kind == Kind::Dirt ? 32u : 48u)
					&& limits.maxPerStageDescriptorSampledImages >= count && limits.maxDescriptorSetSampledImages >= count
					&& limits.maxPerStageDescriptorSamplers >= count - 1 && limits.maxDescriptorSetSamplers >= count - 1
					&& limits.maxPerStageResources >= count && limits.maxColorAttachments >= 1
					&& output.extent.width <= limits.maxFramebufferWidth && output.extent.height <= limits.maxFramebufferHeight
					&& output.extent.width <= limits.maxViewportDimensions[0] && output.extent.height <= limits.maxViewportDimensions[1], "HDR lens physical descriptor/push/viewport limits are insufficient.");
				auto pass = std::make_shared<LensHdrPass>(); pass->device = targets.device; pass->kind = kind; pass->scene = scene; pass->output = output;
				pass->radiance = std::move(radiance); if(kind == Kind::Dirt) pass->dirt = std::move(dirt); pass->renderPass = renderPass; pass->framebuffer = framebuffer;
				VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
				sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				sampler.maxLod = 0; Require(vkCreateSampler(pass->device, &sampler, nullptr, &pass->sampler) == VK_SUCCESS, "HDR lens linear sampler creation failed.");
				std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
				for(std::uint32_t i = 0; i < count; ++i) bindings[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
				VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; descriptors.bindingCount = count; descriptors.pBindings = bindings.data();
				Require(vkCreateDescriptorSetLayout(pass->device, &descriptors, nullptr, &pass->descriptors) == VK_SUCCESS, "HDR lens descriptor layout creation failed.");
				const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count - 1}}};
				VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1; pool.poolSizeCount = 2; pool.pPoolSizes = sizes.data();
				Require(vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool) == VK_SUCCESS, "HDR lens descriptor pool creation failed.");
				VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = pass->pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &pass->descriptors;
				Require(vkAllocateDescriptorSets(pass->device, &allocation, &pass->set) == VK_SUCCESS, "HDR lens immutable descriptor allocation failed.");
				const std::array<VkImageView, 3> views{scene.view, pass->radiance.image.view, pass->dirt.image.view};
				std::array<VkDescriptorImageInfo, 3> images{}; std::array<VkWriteDescriptorSet, 3> writes{};
				for(std::uint32_t i = 0; i < count; ++i) {
					images[i] = {i ? pass->sampler : VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					writes[i].dstSet = pass->set; writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = bindings[i].descriptorType; writes[i].pImageInfo = &images[i];
				}
				vkUpdateDescriptorSets(pass->device, count, writes.data(), 0, nullptr);
				VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, kind == Kind::Dirt ? 32u : 48u};
				VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &pass->descriptors; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
				Require(vkCreatePipelineLayout(pass->device, &layout, nullptr, &pass->layout) == VK_SUCCESS, "HDR lens pipeline layout creation failed.");
				Module vertex(pass->device), fragment(pass->device); LoadModule(vertex, "Shaders/PostProcess/PostProcessFullscreen.mesh.spv");
				LoadModule(fragment, kind == Kind::Dirt ? "Shaders/PostProcess/LensDirt.frag.spv" : "Shaders/PostProcess/LensFlare.frag.spv", &kind);
				std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
				for(auto& stage : stages) stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT; stages[0].module = vertex.value; stages[0].pName = "main";
				stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment.value; stages[1].pName = "PSMain";
				VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
				VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
				VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
				VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
				VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
				VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
				const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
				VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamicStates.data();
				VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.stageCount = 2; pipeline.pStages = stages.data(); pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster;
				pipeline.pMultisampleState = &samples; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic;
				pipeline.layout = pass->layout; pipeline.renderPass = renderPass;
				Require(vkCreateGraphicsPipelines(pass->device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pass->pipeline) == VK_SUCCESS, "HDR lens exact graphics pipeline creation failed.");
				return pass;
			} catch(const std::exception& e) { reason = e.what(); return {}; }
			catch(...) { reason = "HDR lens preparation failed; previous usable output remains retained."; return {}; }
		}
		bool ContextValid(const LensHdrPass& pass, const LensHdrRecordContext& context) noexcept
		{
			if(!pass.radiance.producer || !pass.radiance.image.image || !context.sceneFrameId || context.producedRadianceFrameId != context.sceneFrameId || context.radianceProducer != pass.radiance.producer.get()
				|| context.producedRadianceImage != pass.radiance.image.image) return false;
			for(const auto state : {context.scene, context.radiance}) if(!state.stage || state.layout == VK_IMAGE_LAYOUT_UNDEFINED || state.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return false;
			if(!context.output.stage || context.output.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return false;
			return pass.kind != Kind::Dirt || (context.dirt.stage && context.dirt.layout != VK_IMAGE_LAYOUT_UNDEFINED && context.dirt.layout != VK_IMAGE_LAYOUT_PREINITIALIZED);
		}
		void Draw(VkCommandBuffer command, LensHdrPass& pass, const LensHdrRecordContext& context, const void* constants, std::uint32_t bytes, LensHdrRecordedUse& recorded) noexcept
		{
			Barrier(command, pass.scene, context.scene, kSampled); Barrier(command, pass.radiance.image, context.radiance, kSampled);
			if(pass.kind == Kind::Dirt) Barrier(command, pass.dirt.image, context.dirt, kSampled);
			Barrier(command, pass.output, context.output, kAttachment);
			VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass.renderPass; begin.framebuffer = pass.framebuffer; begin.renderArea.extent = pass.output.extent;
			vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
			VkViewport viewport{0, 0, float(pass.output.extent.width), float(pass.output.extent.height), 0, 1}; VkRect2D scissor{{0, 0}, pass.output.extent};
			vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 1, &pass.set, 0, nullptr);
			vkCmdPushConstants(command, pass.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, constants); vkCmdDrawMeshTasksEXT(command, 1, 1, 1); vkCmdEndRenderPass(command);
			// Compatible render pass supplies COLOR_ATTACHMENT->SHADER_READ layout.
			LensHdrImageState colorWrite{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
			Barrier(command, pass.output, colorWrite, kFinished);
			Barrier(command, pass.scene, kSampled, context.scene); Barrier(command, pass.radiance.image, kSampled, context.radiance);
			if(pass.kind == Kind::Dirt) Barrier(command, pass.dirt.image, kSampled, context.dirt);
			recorded = {context.sceneFrameId, pass.radiance.image.image, pass.output.image, kFinished};
		}
	}

	std::shared_ptr<LensHdrPass> PrepareLensDirtPass(VkPhysicalDevice physical, PostProcessTargetBundle& targets,
		const PostProcessTargetImage& scene, LensHdrBloomRadiance radiance, LensHdrDirtTexture dirt,
		const PostProcessTargetImage& output, VkRenderPass renderPass, VkFramebuffer framebuffer, std::string& reason)
	{
		return Prepare(Kind::Dirt, physical, targets, scene, std::move(radiance), std::move(dirt), output, renderPass, framebuffer, reason);
	}
	std::shared_ptr<LensHdrPass> PrepareLensFlarePass(VkPhysicalDevice physical, PostProcessTargetBundle& targets,
		const PostProcessTargetImage& scene, LensHdrBloomRadiance radiance, const PostProcessTargetImage& output,
		VkRenderPass renderPass, VkFramebuffer framebuffer, std::string& reason)
	{
		return Prepare(Kind::Flare, physical, targets, scene, std::move(radiance), {}, output, renderPass, framebuffer, reason);
	}
	LensHdrRecordResult RecordLensDirtPass(VkCommandBuffer command, LensHdrPass& pass, const GraphicsSettings::LensDirtControls& controls,
		const LensHdrRecordContext& context, LensHdrRecordedUse& recorded, const LensDirtStyle& style) noexcept
	{
		recorded = {};
		if(pass.kind != Kind::Dirt || !std::isfinite(controls.intensity) || controls.intensity < 0 || controls.intensity > 8) return LensHdrRecordResult::Rejected;
		if(!controls.Active()) return LensHdrRecordResult::Skipped;
		for(float v : style.textureScale) if(!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max() / 4) return LensHdrRecordResult::Rejected;
		for(float v : style.textureOffset) if(!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max() / 4) return LensHdrRecordResult::Rejected;
		if(!command || !pass.pipeline || controls.texture != pass.dirt.assetId || !ContextValid(pass, context)) return LensHdrRecordResult::Rejected;
		const DirtConstants constants{{pass.output.extent.width, pass.output.extent.height}, controls.intensity, 1,
			{style.textureScale[0], style.textureScale[1]}, {style.textureOffset[0], style.textureOffset[1]}};
		Draw(command, pass, context, &constants, sizeof(constants), recorded); return LensHdrRecordResult::Recorded;
	}
	LensHdrRecordResult RecordLensFlarePass(VkCommandBuffer command, LensHdrPass& pass, const GraphicsSettings::LensFlareControls& controls,
		const LensHdrRecordContext& context, LensHdrRecordedUse& recorded, const LensFlareStyle& style) noexcept
	{
		recorded = {};
		if(pass.kind != Kind::Flare || !std::isfinite(controls.intensity) || controls.intensity < 0 || controls.intensity > 8
			|| !std::isfinite(controls.threshold) || controls.threshold < 0 || controls.threshold > 64) return LensHdrRecordResult::Rejected;
		if(!controls.Active()) return LensHdrRecordResult::Skipped;
		if(!std::isfinite(style.ghostSpacing) || style.ghostSpacing < 0 || style.ghostSpacing > std::numeric_limits<float>::max() / 16
			|| !std::isfinite(style.haloRadius) || style.haloRadius < 0 || !std::isfinite(style.haloWidth) || style.haloWidth < 0 || style.ghostCount > 8) return LensHdrRecordResult::Rejected;
		for(float v : style.tint) if(!std::isfinite(v) || v < 0) return LensHdrRecordResult::Rejected;
		if(!command || !pass.pipeline || !ContextValid(pass, context)) return LensHdrRecordResult::Rejected;
		const FlareConstants constants{{pass.output.extent.width, pass.output.extent.height}, controls.intensity, 1, controls.threshold,
			style.ghostSpacing, style.haloRadius, style.haloWidth, {style.tint[0], style.tint[1], style.tint[2]}, style.ghostCount};
		Draw(command, pass, context, &constants, sizeof(constants), recorded); return LensHdrRecordResult::Recorded;
	}
}
