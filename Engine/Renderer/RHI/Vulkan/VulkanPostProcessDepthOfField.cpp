// Real standard-Z depth linearization followed by the frozen HDR DOF gather.
#include "VulkanPostProcessDepthOfField.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "Resource/ResourceManager.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct alignas(16) LinearConstants { glm::mat4 inverseProjection; std::uint32_t extent[2], reserved[2]; };
		struct DofConstants { std::uint32_t extent[2]; float focusDistance, focusRange, radius; std::uint32_t enabled; float tolerance; std::uint32_t reserved; };
		static_assert(sizeof(LinearConstants) == 80 && offsetof(LinearConstants, extent) == 64 && offsetof(LinearConstants, reserved) == 72);
		static_assert(sizeof(DofConstants) == 32 && offsetof(DofConstants, radius) == 16 && offsetof(DofConstants, enabled) == 20 && offsetof(DofConstants, tolerance) == 24);
		constexpr std::array<VkDescriptorType, 2> kLinearTypes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		constexpr std::array<VkDescriptorType, 3> kDofTypes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		void Require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
		bool Extent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool Hdr(VkFormat format) noexcept { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; }
		bool Finite(const glm::mat4& matrix) noexcept
		{
			for(std::size_t column = 0; column < 4; ++column) for(std::size_t row = 0; row < 4; ++row) if(!std::isfinite(matrix[column][row])) return false; return true;
		}
		bool InverseStandardProjection(const glm::mat4& projection, glm::mat4& inverse) noexcept
		{
			if(!Finite(projection)) return false; const auto determinant = glm::determinant(projection);
			if(!std::isfinite(determinant) || determinant == 0.0f) return false;
			const auto candidate = glm::inverse(projection); if(!Finite(candidate)) return false;
			const auto nearPoint = candidate * glm::vec4(0, 0, 0, 1), middle = candidate * glm::vec4(0, 0, 0.5f, 1);
			if(!std::isfinite(nearPoint.w) || !std::isfinite(middle.w) || std::abs(nearPoint.w) <= 1e-8f || std::abs(middle.w) <= 1e-8f) return false;
			const float nearMetres = -nearPoint.z / nearPoint.w, middleMetres = -middle.z / middle.w;
			// StandardZ increases with positive eye distance; reversedZ and cameras
			// with a different handedness need an explicit separately tested path.
			if(!std::isfinite(nearMetres) || !std::isfinite(middleMetres) || nearMetres <= 0 || middleMetres <= nearMetres) return false;
			inverse = candidate; return true;
		}
		void Qualify(VkPhysicalDevice physical, const PostProcessTargetImage& image, VkFormatFeatureFlags features)
		{
			Require(image.extent.width && image.extent.height && image.usage, "DOF image metadata is incomplete.");
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			Require((format.optimalTilingFeatures & features) == features, "DOF image lacks required optimal sampled/storage features.");
			VkImageFormatProperties limits{};
			Require(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, image.usage, 0, &limits) == VK_SUCCESS,
				"DOF complete image-usage combination is unsupported.");
			Require(image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height && limits.maxExtent.depth >= 1
				&& limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1 && (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "DOF extent/sample count exceeds exact format support.");
		}
		void ValidateDofModule(const std::vector<std::uint32_t>& words, bool linear, bool rgba32)
		{
			Require(words.size() >= 5 && words[0] == 0x07230203u, "DOF SPIR-V header is invalid.");
			std::map<std::uint32_t, std::uint32_t> bindings, sets, variables, pointers, formats;
			std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
			std::uint32_t push = 0; bool entry = false, workgroup = false;
			for(std::size_t i = 5; i < words.size();) {
				const auto count = words[i] >> 16, op = words[i] & 65535u; Require(count && count <= words.size() - i, "DOF SPIR-V instruction is truncated.");
				if(op == 17) Require(count == 2 && (words[i + 1] == 1 || words[i + 1] == 50), "DOF shader needs unqualified optional features.");
				if(op == 15 && count >= 5 && words[i + 1] == 5) { const auto* name = reinterpret_cast<const char*>(words.data() + i + 3); entry = std::memchr(name, 0, (count - 3) * 4) && std::strcmp(name, "CSMain") == 0; }
				if(op == 16 && count == 6 && words[i + 2] == 17) workgroup = words[i + 3] == 8 && words[i + 4] == 8 && words[i + 5] == 1;
				if(op == 71 && count == 4 && words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
				if(op == 71 && count == 4 && words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
				if(op == 72 && count == 5 && words[i + 3] == 35) offsets[words[i + 1]][words[i + 2]] = words[i + 4];
				if(op == 59 && count >= 4) { variables[words[i + 2]] = words[i + 1]; if(words[i + 3] == 9) push = words[i + 1]; }
				if(op == 32 && count == 4) pointers[words[i + 1]] = words[i + 3];
				if(op == 25 && count >= 9) formats[words[i + 1]] = words[i + 8];
				i += count;
			}
			const std::uint32_t count = linear ? 2u : 3u; std::array<bool, 3> seen{};
			Require(entry && workgroup && bindings.size() == count, "DOF entry/workgroup/descriptor count disagrees with prepared ABI.");
			for(const auto& [id, binding] : bindings) {
				Require(binding < count && !seen[binding] && sets.contains(id) && sets.at(id) == 0, "DOF descriptor binding/set is invalid or duplicated."); seen[binding] = true;
				Require(variables.contains(id) && pointers.contains(variables.at(id)) && formats.contains(pointers.at(variables.at(id))), "DOF descriptor is not a direct image.");
				const auto format = formats.at(pointers.at(variables.at(id)));
				Require(binding == count - 1 ? format == (linear ? 3u : rgba32 ? 1u : 2u) : format == 0u, "DOF image-format variant disagrees with native target.");
			}
			Require(push && pointers.contains(push), "DOF push constant owner is missing.");
			const std::vector<std::uint32_t> expected = linear ? std::vector<std::uint32_t>{0, 64, 72, 76} : std::vector<std::uint32_t>{0, 8, 12, 16, 20, 24, 28};
			const auto& actual = offsets[pointers.at(push)]; Require(actual.size() == expected.size(), "DOF push member count changed.");
			for(std::uint32_t i = 0; i < expected.size(); ++i) Require(actual.contains(i) && actual.at(i) == expected[i], "DOF push member offsets changed.");
		}
		void CreatePipeline(VkDevice device, const char* path, VkPipelineLayout layout, VkPipeline& pipeline, bool linear, bool rgba32)
		{
			auto& resources = ResourceManager::Instance(); const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			Require(!bytes.empty() && bytes.size() % 4 == 0, "Actual installed DOF shader variant is missing.");
			std::vector<std::uint32_t> words(bytes.size() / 4); std::memcpy(words.data(), bytes.data(), bytes.size()); ValidateDofModule(words, linear, rgba32);
			VkShaderModule module = VK_NULL_HANDLE; VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = bytes.size(); info.pCode = words.data();
			Require(vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS, "DOF shader module creation failed.");
			VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; compute.layout = layout;
			compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module = module; compute.stage.pName = "CSMain";
			const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline); vkDestroyShaderModule(device, module, nullptr);
			Require(result == VK_SUCCESS, "DOF compute pipeline creation failed.");
		}
		void CreateLayout(VkDevice device, std::span<const VkDescriptorType> types, std::uint32_t pushBytes, VkDescriptorSetLayout& descriptors, VkPipelineLayout& layout)
		{
			std::array<VkDescriptorSetLayoutBinding, 3> bindings{}; for(std::uint32_t i = 0; i < types.size(); ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount = static_cast<std::uint32_t>(types.size()); info.pBindings = bindings.data();
			Require(vkCreateDescriptorSetLayout(device, &info, nullptr, &descriptors) == VK_SUCCESS, "DOF descriptor layout creation failed.");
			VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes}; VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pipeline.setLayoutCount = 1; pipeline.pSetLayouts = &descriptors; pipeline.pushConstantRangeCount = 1; pipeline.pPushConstantRanges = &push;
			Require(vkCreatePipelineLayout(device, &pipeline, nullptr, &layout) == VK_SUCCESS, "DOF pipeline layout creation failed.");
		}
		void Barrier(VkCommandBuffer command, const PostProcessTargetImage& image, VkImageAspectFlags aspect, DepthOfFieldImageState before, DepthOfFieldImageState after) noexcept
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image.image; barrier.oldLayout = before.layout; barrier.newLayout = after.layout;
			barrier.srcAccessMask = before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : before.access; barrier.dstAccessMask = after.access;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.subresourceRange = {aspect, 0, 1, 0, 1};
			vkCmdPipelineBarrier(command, before.stage, after.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		constexpr DepthOfFieldImageState kSampled{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr DepthOfFieldImageState kDepthRead{VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr DepthOfFieldImageState kRead{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr DepthOfFieldImageState kWrite{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
		constexpr DepthOfFieldImageState kFinished{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
	}

	struct DepthOfFieldPass
	{
		VkDevice device = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE;
		DepthOfFieldInputs inputs; PostProcessTargetKey key; std::uint64_t generation = 0;
		PostProcessTargetImage linearDepth;
		std::array<VkDescriptorSetLayout, 2> descriptors{}; std::array<VkPipelineLayout, 2> layouts{};
		std::array<VkPipeline, 2> pipelines{}; std::array<VkDescriptorSet, 2> sets{}; VkDescriptorPool pool = VK_NULL_HANDLE;
		~DepthOfFieldPass()
		{
			// Parent actual completion/retirement protects all recorded borrowers.
			for(auto pipeline : pipelines) if(pipeline) vkDestroyPipeline(device, pipeline, nullptr);
			if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			for(auto layout : layouts) if(layout) vkDestroyPipelineLayout(device, layout, nullptr);
			for(auto descriptor : descriptors) if(descriptor) vkDestroyDescriptorSetLayout(device, descriptor, nullptr);
			if(linearDepth.view) vkDestroyImageView(device, linearDepth.view, nullptr);
			if(linearDepth.image) vmaDestroyImage(allocator, linearDepth.image, linearDepth.allocation);
		}
	};

	std::shared_ptr<DepthOfFieldPass> PrepareDepthOfFieldPass(VkPhysicalDevice physical, PostProcessTargetBundle& targets, const DepthOfFieldInputs& inputs, std::string& reason)
	{
		reason.clear();
		try {
			Require(physical && targets.device && targets.allocator && targets.key.rendererGeneration
				&& (inputs.viewLifetimeGeneration || targets.key.viewLifetimeGeneration)
				&& inputs.backendResourceGeneration && inputs.samples == VK_SAMPLE_COUNT_1_BIT, "DOF preparation requires exact renderer/view/backend ownership and sample1.");
			const auto& source = inputs.sceneColor; const auto& depth = inputs.sceneDepth; const auto& output = inputs.output;
			Require(Hdr(source.format) && source.format == output.format && (depth.format == VK_FORMAT_D32_SFLOAT || depth.format == VK_FORMAT_D16_UNORM)
				&& Extent(source.extent, depth.extent) && Extent(source.extent, output.extent) && Extent(output.extent, targets.outputExtent)
				&& Extent(source.extent, targets.internalExtent), "DOF initially requires matching native HDR/D32orD16/output extents.");
			const std::array<const PostProcessTargetImage*, 3> borrowed{&source, &depth, &output};
			for(std::size_t i = 0; i < borrowed.size(); ++i) {
				Require(borrowed[i]->image && borrowed[i]->view && (borrowed[i]->usage & VK_IMAGE_USAGE_SAMPLED_BIT), "DOF actual borrowed sampled image/view is absent.");
				for(std::size_t j = 0; j < i; ++j) Require(borrowed[i]->image != borrowed[j]->image && borrowed[i]->view != borrowed[j]->view, "DOF scene/depth/output must be distinct images.");
				Qualify(physical, *borrowed[i], VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (i == 2 ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0));
			}
			Require((depth.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && (output.usage & VK_IMAGE_USAGE_STORAGE_BIT), "DOF depth must be an actual scene attachment and output storage-capable.");
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties); const auto& limits = properties.limits;
			Require(properties.apiVersion >= VK_API_VERSION_1_3 && limits.maxPushConstantsSize >= sizeof(LinearConstants) && limits.maxComputeWorkGroupInvocations >= 64
				&& limits.maxComputeWorkGroupSize[0] >= 8 && limits.maxComputeWorkGroupSize[1] >= 8 && limits.maxComputeWorkGroupSize[2] >= 1
				&& limits.maxPerStageDescriptorSampledImages >= 2 && limits.maxDescriptorSetSampledImages >= 2
				&& limits.maxPerStageDescriptorStorageImages >= 1 && limits.maxDescriptorSetStorageImages >= 1 && limits.maxPerStageResources >= 3
				&& (std::uint64_t(source.extent.width) + 7) / 8 <= limits.maxComputeWorkGroupCount[0]
				&& (std::uint64_t(source.extent.height) + 7) / 8 <= limits.maxComputeWorkGroupCount[1] && limits.maxComputeWorkGroupCount[2] >= 1
				&& source.extent.width <= std::numeric_limits<std::int32_t>::max() - 64u && source.extent.height <= std::numeric_limits<std::int32_t>::max() - 64u,
				"DOF compute/descriptor/dispatch limits are insufficient.");
			PostProcessTargetImage linear; linear.extent = source.extent; linear.format = VK_FORMAT_R32_SFLOAT; linear.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			Qualify(physical, linear, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
			auto pass = std::make_shared<DepthOfFieldPass>(); pass->device = targets.device; pass->allocator = targets.allocator; pass->inputs = inputs; pass->key = targets.key; pass->linearDepth = linear;
			static std::atomic<std::uint64_t> generations{0}; auto previous = generations.load(std::memory_order_relaxed);
			while(previous != std::numeric_limits<std::uint64_t>::max() && !generations.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {}
			Require(previous != std::numeric_limits<std::uint64_t>::max(), "DOF generation space is exhausted."); pass->generation = previous + 1;
			VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = linear.format; image.extent = {linear.extent.width, linear.extent.height, 1};
			image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = linear.usage; image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			Require(vmaCreateImage(pass->allocator, &image, &allocation, &pass->linearDepth.image, &pass->linearDepth.allocation, nullptr) == VK_SUCCESS, "DOF linear depth allocation failed.");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = pass->linearDepth.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = linear.format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			Require(vkCreateImageView(pass->device, &view, nullptr, &pass->linearDepth.view) == VK_SUCCESS, "DOF linear depth view failed.");
			CreateLayout(pass->device, kLinearTypes, sizeof(LinearConstants), pass->descriptors[0], pass->layouts[0]);
			CreateLayout(pass->device, kDofTypes, sizeof(DofConstants), pass->descriptors[1], pass->layouts[1]);
			CreatePipeline(pass->device, "Shaders/PostProcess/LinearViewDepth.comp.spv", pass->layouts[0], pass->pipelines[0], true, false);
			const bool rgba32 = output.format == VK_FORMAT_R32G32B32A32_SFLOAT;
			CreatePipeline(pass->device, rgba32 ? "Shaders/PostProcess/DepthOfField32.comp.spv" : "Shaders/PostProcess/DepthOfField16.comp.spv", pass->layouts[1], pass->pipelines[1], false, rgba32);
			const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 2; pool.poolSizeCount = 2; pool.pPoolSizes = sizes.data();
			Require(vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool) == VK_SUCCESS, "DOF descriptor pool failed.");
			VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; set.descriptorPool = pass->pool; set.descriptorSetCount = 2; set.pSetLayouts = pass->descriptors.data();
			Require(vkAllocateDescriptorSets(pass->device, &set, pass->sets.data()) == VK_SUCCESS, "DOF immutable descriptor allocation failed.");
			const std::array<VkDescriptorImageInfo, 5> images{{{VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
				{VK_NULL_HANDLE, pass->linearDepth.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				{VK_NULL_HANDLE, pass->linearDepth.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL}}};
			std::array<VkWriteDescriptorSet, 5> writes{};
			for(std::uint32_t i = 0; i < writes.size(); ++i) {
				writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = pass->sets[i < 2 ? 0 : 1]; writes[i].dstBinding = i < 2 ? i : i - 2;
				writes[i].descriptorCount = 1; writes[i].descriptorType = i == 1 || i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo = &images[i];
			}
			vkUpdateDescriptorSets(pass->device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr); return pass;
		} catch(const std::exception& e) { reason = e.what(); return {}; }
		catch(...) { reason = "DOF preparation failed; previous usable owner remains retained."; return {}; }
	}

	namespace
	{
		bool FrameProjection(const DepthOfFieldPass& pass, const DepthOfFieldFrameInput& frame, const GraphicsSettings::DepthOfFieldControls& controls, glm::mat4& inverse) noexcept
		{
			if(!frame.publication || !frame.sceneFrameId || frame.sceneFrameId != frame.depthFrameId || frame.sceneFrameId != frame.publication->frameId
				|| frame.publication->rendererSessionGeneration != pass.key.rendererGeneration || frame.publication->backendResourceGeneration != pass.inputs.backendResourceGeneration
				|| frame.presentationIndex >= frame.publication->presentationInputs.size()) return false;
			const auto& input = frame.publication->presentationInputs[frame.presentationIndex];
			const auto lifetime = pass.inputs.viewLifetimeGeneration ? pass.inputs.viewLifetimeGeneration : pass.key.viewLifetimeGeneration;
			if(!input.graphicsView || !input.graphicsSettings || input.graphicsView->lifetimeGeneration != lifetime
				|| pass.key.viewId != input.graphicsView->outputId || pass.key.viewKind != (static_cast<std::uint32_t>(input.graphicsView->outputKind) << 8u | static_cast<std::uint32_t>(input.graphicsView->viewType))
				|| input.output.kind != input.graphicsView->outputKind || input.output.diagnosticsView != input.graphicsView->viewType || input.output.outputId != input.graphicsView->outputId
				|| input.graphicsSettings->lifecycle.rendererSessionGeneration != pass.key.rendererGeneration
				|| input.graphicsSettings->lifecycle.backendResourceGeneration != pass.inputs.backendResourceGeneration || input.graphicsSettings->sampleCount != 1) return false;
			// Runtime publications may leave size implicit. Only the settings
			// bound to this exact runtime output can supply the physical extent.
			auto extent = input.output.targetSize;
			if(extent == glm::uvec2(0u) && input.output.kind == RenderOutputKind::MainRuntimeView)
				extent = {input.graphicsSettings->outputWidth, input.graphicsSettings->outputHeight};
			if(extent.x != pass.inputs.sceneColor.extent.width || extent.y != pass.inputs.sceneColor.extent.height) return false;
			if(std::any_of(pass.key.cameraId.begin(), pass.key.cameraId.end(), [](std::uint8_t value) { return value != 0; })) {
				if(!input.graphicsCamera || input.graphicsCamera->cameraId != pass.key.cameraId
					|| input.graphicsCamera->sceneGeneration != pass.key.cameraGenerations[0] || input.graphicsCamera->objectLifetimeGeneration != pass.key.cameraGenerations[1]
					|| input.graphicsCamera->componentGeneration != pass.key.cameraGenerations[2] || input.graphicsCamera->bindingGeneration != pass.key.cameraGenerations[3]) return false;
			}
			GraphicsSettings::PostProcessEffectSettings consumed;
			if(GraphicsSettings::ResolvePostProcessEffectSettings(input.graphicsSettings->values, consumed) != GraphicsSettings::GraphicsRequestReason::None
				|| consumed.depthOfField.enabled != controls.enabled || consumed.depthOfField.focusDistance != controls.focusDistance
				|| consumed.depthOfField.focusRange != controls.focusRange || consumed.depthOfField.blurStrength != controls.blurStrength) return false;
			return InverseStandardProjection(input.projectionMatrix, inverse);
		}
	}
	DepthOfFieldRecordResult RecordDepthOfFieldPass(VkCommandBuffer command, DepthOfFieldPass& pass,
		const GraphicsSettings::DepthOfFieldControls& controls, const DepthOfFieldFrameInput& frame, DepthOfFieldRecordedUse& recorded, const DepthOfFieldStyle& style) noexcept
	{
		recorded = {};
		if(!std::isfinite(controls.focusDistance) || controls.focusDistance < 0.01f || controls.focusDistance > 10000
			|| !std::isfinite(controls.focusRange) || controls.focusRange < 0.01f || controls.focusRange > 10000
			|| !std::isfinite(controls.blurStrength) || controls.blurStrength < 0 || controls.blurStrength > 64
			|| !std::isfinite(style.relativeDepthTolerance) || style.relativeDepthTolerance < 0) return DepthOfFieldRecordResult::Rejected;
		if(!controls.Active()) return DepthOfFieldRecordResult::Skipped;
		if(!command || !pass.pipelines[0] || !pass.pipelines[1]) return DepthOfFieldRecordResult::Rejected;
		for(const auto state : {frame.sceneColor, frame.sceneDepth}) if(!state.stage || state.layout == VK_IMAGE_LAYOUT_UNDEFINED || state.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return DepthOfFieldRecordResult::Rejected;
		if(!frame.output.stage || frame.output.layout == VK_IMAGE_LAYOUT_PREINITIALIZED
			|| (frame.linearDepthLayout != VK_IMAGE_LAYOUT_UNDEFINED && frame.linearDepthLayout != VK_IMAGE_LAYOUT_GENERAL)) return DepthOfFieldRecordResult::Rejected;
		glm::mat4 inverse; if(!FrameProjection(pass, frame, controls, inverse)) return DepthOfFieldRecordResult::Rejected;
		Barrier(command, pass.inputs.sceneDepth, VK_IMAGE_ASPECT_DEPTH_BIT, frame.sceneDepth, kDepthRead);
		const DepthOfFieldImageState previousLinear{frame.linearDepthLayout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
		Barrier(command, pass.linearDepth, VK_IMAGE_ASPECT_COLOR_BIT, previousLinear, kWrite);
		const LinearConstants linear{inverse, {pass.inputs.sceneColor.extent.width, pass.inputs.sceneColor.extent.height}, {0, 0}};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelines[0]); vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layouts[0], 0, 1, &pass.sets[0], 0, nullptr);
		vkCmdPushConstants(command, pass.layouts[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(linear), &linear);
		const auto x = (pass.inputs.sceneColor.extent.width + 7) / 8, y = (pass.inputs.sceneColor.extent.height + 7) / 8; vkCmdDispatch(command, x, y, 1);
		Barrier(command, pass.linearDepth, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kRead);
		Barrier(command, pass.inputs.sceneColor, VK_IMAGE_ASPECT_COLOR_BIT, frame.sceneColor, kSampled);
		Barrier(command, pass.inputs.output, VK_IMAGE_ASPECT_COLOR_BIT, frame.output, kWrite);
		const DofConstants constants{{pass.inputs.sceneColor.extent.width, pass.inputs.sceneColor.extent.height}, controls.focusDistance, controls.focusRange, controls.blurStrength, 1, style.relativeDepthTolerance, 0};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelines[1]); vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layouts[1], 0, 1, &pass.sets[1], 0, nullptr);
		vkCmdPushConstants(command, pass.layouts[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants); vkCmdDispatch(command, x, y, 1);
		Barrier(command, pass.inputs.output, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kFinished);
		Barrier(command, pass.inputs.sceneDepth, VK_IMAGE_ASPECT_DEPTH_BIT, kDepthRead, frame.sceneDepth);
		Barrier(command, pass.inputs.sceneColor, VK_IMAGE_ASPECT_COLOR_BIT, kSampled, frame.sceneColor);
		recorded = {frame.sceneFrameId, pass.generation, frame.presentationIndex, pass.linearDepth.image, pass.inputs.output.image, kRead, kFinished};
		return DepthOfFieldRecordResult::Recorded;
	}
	PostProcessTargetImage ReadDepthOfFieldLinearDepthTarget(const DepthOfFieldPass& pass) noexcept { return pass.linearDepth; }
}
