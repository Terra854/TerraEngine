// Actual per-view raster-motion conversion, disocclusion and HDR blur.
#include "VulkanPostProcessMotionBlur.h"
#include "VulkanPostProcessMotionTargets.h"
#include "VulkanPostProcessMotionBuffers.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "Resource/ResourceManager.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <array>
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
		struct alignas(16) ConversionConstants
		{
			glm::mat4 currentInverse, previousInverse;
			std::uint32_t extent[2]; float currentJitter[2], previousJitter[2];
			float absoluteTolerance, relativeTolerance; std::uint32_t historyValid, flags, reserved0, reserved1;
		};
		struct BlurConstants { std::uint32_t extent[2]; float strength, camera, object, radius, tolerance; std::uint32_t enabled; };
		static_assert(sizeof(ConversionConstants) == 176 && offsetof(ConversionConstants, extent) == 128 && offsetof(ConversionConstants, currentJitter) == 136
			&& offsetof(ConversionConstants, previousJitter) == 144 && offsetof(ConversionConstants, absoluteTolerance) == 152 && offsetof(ConversionConstants, historyValid) == 160);
		static_assert(sizeof(BlurConstants) == 32 && offsetof(BlurConstants, object) == 16 && offsetof(BlurConstants, enabled) == 28);
		constexpr std::array<VkDescriptorType, 8> kConversionTypes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		constexpr std::array<VkDescriptorType, 6> kBlurTypes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		void Require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
		bool Extent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool Hdr(VkFormat format) noexcept { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; }
		bool SameImage(const PostProcessTargetImage& a, const PostProcessTargetImage& b) noexcept { return a.image == b.image && a.view == b.view && a.format == b.format && a.usage == b.usage && Extent(a.extent, b.extent); }
		std::uint64_t Token(std::uint32_t low, std::uint32_t high) noexcept { return low | (std::uint64_t(high) << 32u); }
		bool Finite(const glm::mat4& matrix) noexcept { for(unsigned c = 0; c < 4; ++c) for(unsigned r = 0; r < 4; ++r) if(!std::isfinite(matrix[c][r])) return false; return true; }
		bool InverseStandard(const glm::mat4& projection, glm::mat4& inverse) noexcept
		{
			if(!Finite(projection)) return false; float determinant = glm::determinant(projection); if(!std::isfinite(determinant) || determinant == 0) return false;
			auto candidate = glm::inverse(projection); if(!Finite(candidate)) return false;
			auto nearPoint = candidate * glm::vec4(0, 0, 0, 1), middle = candidate * glm::vec4(0, 0, .5f, 1);
			if(std::abs(nearPoint.w) <= 1e-8f || std::abs(middle.w) <= 1e-8f) return false;
			float nearMetres = -nearPoint.z / nearPoint.w, middleMetres = -middle.z / middle.w;
			if(!std::isfinite(nearMetres) || !std::isfinite(middleMetres) || nearMetres <= 0 || middleMetres <= nearMetres) return false; inverse = candidate; return true;
		}
		void Qualify(VkPhysicalDevice physical, const PostProcessTargetImage& image, VkFormatFeatureFlags features)
		{
			Require(image.extent.width && image.extent.height && image.usage, "Motion blur image metadata is incomplete."); VkFormatProperties format{};
			vkGetPhysicalDeviceFormatProperties(physical, image.format, &format); Require((format.optimalTilingFeatures & features) == features, "Motion blur image lacks actual sampled/storage format support.");
			VkImageFormatProperties limits{}; Require(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, image.usage, 0, &limits) == VK_SUCCESS, "Motion blur complete image usage is unsupported.");
			Require(image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height && limits.maxExtent.depth >= 1 && limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1 && (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "Motion blur image exceeds extent/sample limits.");
		}
		void CreateLayout(VkDevice device, std::span<const VkDescriptorType> types, std::uint32_t bytes, VkDescriptorSetLayout& descriptors, VkPipelineLayout& pipeline)
		{
			std::array<VkDescriptorSetLayoutBinding, 8> bindings{}; for(std::uint32_t i = 0; i < types.size(); ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount = static_cast<std::uint32_t>(types.size()); set.pBindings = bindings.data();
			Require(vkCreateDescriptorSetLayout(device, &set, nullptr, &descriptors) == VK_SUCCESS, "Motion blur descriptor layout failed.");
			VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes}; VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout.setLayoutCount = 1; layout.pSetLayouts = &descriptors; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
			Require(vkCreatePipelineLayout(device, &layout, nullptr, &pipeline) == VK_SUCCESS, "Motion blur pipeline layout failed.");
		}
		void ValidateBlurModule(const std::vector<std::uint32_t>& words, bool conversion, bool rgba32)
		{
			Require(words.size() >= 5 && words[0] == 0x07230203u, "Motion blur SPIR-V header is invalid.");
			std::map<std::uint32_t, std::uint32_t> bindings, sets, variables, pointers; std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> images;
			std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets; std::uint32_t push = 0; bool entry = false, localSize = false;
			for(std::size_t i = 5; i < words.size();) {
				const auto count = words[i] >> 16, op = words[i] & 65535u; Require(count && count <= words.size() - i, "Motion blur SPIR-V instruction is truncated.");
				if(op == 17) Require(count == 2 && (words[i + 1] == 1 || words[i + 1] == 50), "Motion blur module requires unqualified optional features.");
				if(op == 15 && count >= 5 && words[i + 1] == 5) { const auto* name = reinterpret_cast<const char*>(words.data() + i + 3); entry = std::memchr(name, 0, (count - 3) * 4) && std::strcmp(name, "CSMain") == 0; }
				if(op == 16 && count == 6 && words[i + 2] == 17) localSize = words[i + 3] == 8 && words[i + 4] == 8 && words[i + 5] == 1;
				if(op == 71 && count == 4 && words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
				if(op == 71 && count == 4 && words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
				if(op == 72 && count == 5 && words[i + 3] == 35) offsets[words[i + 1]][words[i + 2]] = words[i + 4];
				if(op == 59 && count >= 4) { variables[words[i + 2]] = words[i + 1]; if(words[i + 3] == 9) push = words[i + 1]; }
				if(op == 32 && count == 4) pointers[words[i + 1]] = words[i + 3];
				if(op == 25 && count >= 9) images[words[i + 1]] = {words[i + 7], words[i + 8]}; i += count;
			}
			const auto count = conversion ? 8u : 6u; std::array<bool, 8> seen{};
			Require(entry && localSize && bindings.size() == count, "Motion blur entry/workgroup/descriptor count changed.");
			for(const auto& [id, binding] : bindings) {
				Require(binding < count && !seen[binding] && sets.contains(id) && sets.at(id) == 0, "Motion blur descriptor set/binding is invalid."); seen[binding] = true;
				Require(variables.contains(id) && pointers.contains(variables.at(id)) && images.contains(pointers.at(variables.at(id))), "Motion blur descriptor is not a direct image.");
				const auto [sampled, format] = images.at(pointers.at(variables.at(id))); const bool storage = conversion ? binding >= 4 : binding == 5;
				const auto expected = conversion ? (binding == 5 || binding == 6 ? 1u : 3u) : rgba32 ? 1u : 2u;
				Require(storage ? sampled == 2 && format == expected : sampled == 1 && format == 0, "Motion blur image storage/sampled format variant disagrees with prepared ABI.");
			}
			Require(push && pointers.contains(push), "Motion blur push block is absent.");
			const std::vector<std::uint32_t> expected = conversion ? std::vector<std::uint32_t>{0, 64, 128, 136, 144, 152, 156, 160, 164, 168, 172} : std::vector<std::uint32_t>{0, 8, 12, 16, 20, 24, 28};
			const auto& actual = offsets[pointers.at(push)]; Require(actual.size() == expected.size(), "Motion blur push member count changed.");
			for(std::uint32_t i = 0; i < expected.size(); ++i) Require(actual.contains(i) && actual.at(i) == expected[i], "Motion blur push member offsets changed.");
		}
		void CreatePipeline(VkDevice device, const char* path, VkPipelineLayout layout, VkPipeline& pipeline, bool conversion, bool rgba32)
		{
			auto& resources = ResourceManager::Instance(); const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			Require(!bytes.empty() && bytes.size() % 4 == 0, "Actual motion blur shader variant is missing."); std::vector<std::uint32_t> words(bytes.size() / 4);
			std::memcpy(words.data(), bytes.data(), bytes.size()); ValidateBlurModule(words, conversion, rgba32);
			VkShaderModule module = VK_NULL_HANDLE; VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize = bytes.size(); shader.pCode = words.data();
			Require(vkCreateShaderModule(device, &shader, nullptr, &module) == VK_SUCCESS, "Motion blur shader module failed.");
			VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; info.layout = layout;
			info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = module; info.stage.pName = "CSMain";
			const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline); vkDestroyShaderModule(device, module, nullptr); Require(result == VK_SUCCESS, "Motion blur compute pipeline failed.");
		}
		void Barrier(VkCommandBuffer command, const PostProcessTargetImage& image, VkImageAspectFlags aspect, MotionBlurImageState before, MotionBlurImageState after) noexcept
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image.image; barrier.oldLayout = before.layout; barrier.newLayout = after.layout;
			barrier.srcAccessMask = before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : before.access; barrier.dstAccessMask = after.access; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange = {aspect, 0, 1, 0, 1}; vkCmdPipelineBarrier(command, before.stage, after.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		constexpr MotionBlurImageState kRead{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr MotionBlurImageState kWrite{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
		constexpr MotionBlurImageState kSampled{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr MotionBlurImageState kDepthRead{VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr MotionBlurImageState kFinished{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
	}

	struct MotionBlurPass
	{
		VkDevice device = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE; MotionBlurInputs inputs; PostProcessTargetKey key;
		std::array<PostProcessTargetImage, 4> converted; //linear metres,total,camera,mask.
		std::vector<PostProcessTargetImage> historyDepth;
		std::array<VkDescriptorSetLayout, 2> descriptors{}; std::array<VkPipelineLayout, 2> layouts{}; std::array<VkPipeline, 2> pipelines{};
		VkDescriptorPool pool = VK_NULL_HANDLE; std::vector<VkDescriptorSet> conversionSets; VkDescriptorSet blurSet = VK_NULL_HANDLE;
		MotionBlurPass() = default;
		MotionBlurPass(const MotionBlurPass&) = delete;
		MotionBlurPass& operator=(const MotionBlurPass&) = delete;
		~MotionBlurPass()
		{
			for(auto pipeline : pipelines) if(pipeline) vkDestroyPipeline(device, pipeline, nullptr); if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			for(auto layout : layouts) if(layout) vkDestroyPipelineLayout(device, layout, nullptr); for(auto set : descriptors) if(set) vkDestroyDescriptorSetLayout(device, set, nullptr);
			for(auto& image : converted) { if(image.view) vkDestroyImageView(device, image.view, nullptr); if(image.image) vmaDestroyImage(allocator, image.image, image.allocation); }
			// Borrowed motion/history owners release only after their descriptors.
		}
	};

	std::shared_ptr<MotionBlurPass> PrepareMotionBlurPass(VkPhysicalDevice physical, PostProcessTargetBundle& targets, const MotionBlurInputs& inputs, std::string& reason)
	{
		reason.clear();
		try {
			Require(physical && targets.device && targets.allocator && inputs.motion && inputs.previousDepthOwner
				&& targets.key.rendererGeneration && inputs.viewLifetimeGeneration && inputs.backendResourceGeneration && inputs.samples == VK_SAMPLE_COUNT_1_BIT
				&& (!targets.key.viewLifetimeGeneration || targets.key.viewLifetimeGeneration == inputs.viewLifetimeGeneration),
				"Motion blur requires real per-view raw motion/previous depth owners and sample1.");
			Require(inputs.motion->device == targets.device && TemporalPassDevice(*inputs.previousDepthOwner) == targets.device
				&& TemporalPassTargetKey(*inputs.previousDepthOwner) == targets.key, "Motion blur previous depth or raw motion belongs to a different device/view.");
			Require(inputs.colorGrid == MotionBlurColorGrid::CurrentRaster || inputs.colorGrid == MotionBlurColorGrid::UnjitteredOutput, "Motion blur source color grid must be explicitly qualified.");
			const auto& source = inputs.sceneColor; const auto& depth = inputs.sceneDepth; const auto& output = inputs.output;
			Require(Hdr(source.format) && source.format == output.format && (depth.format == VK_FORMAT_D32_SFLOAT || depth.format == VK_FORMAT_D16_UNORM)
				&& inputs.motion->usesCameraMotion && inputs.motion->sourceDepth == depth.view && Extent(inputs.motion->extent, source.extent)
				&& Extent(depth.extent, source.extent) && Extent(output.extent, source.extent) && Extent(source.extent, targets.internalExtent) && Extent(source.extent, targets.outputExtent),
				"Motion blur initially requires real standardZ depth and both raw velocities at one native extent.");
			const auto& total = inputs.motion->totalMotion; const auto& camera = inputs.motion->cameraMotion;
			Require(total.format == VK_FORMAT_R32G32B32A32_SFLOAT && camera.format == total.format && Extent(total.extent, source.extent) && Extent(camera.extent, source.extent), "Motion blur raw velocity targets are not exact float32 native outputs.");
			const std::array<const PostProcessTargetImage*, 5> borrowed{&source, &depth, &total, &camera, &output};
			for(std::size_t i = 0; i < borrowed.size(); ++i) {
				Require(borrowed[i]->image && borrowed[i]->view && (borrowed[i]->usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Motion blur actual sampled image/view is missing.");
				for(std::size_t j = 0; j < i; ++j) Require(borrowed[i]->image != borrowed[j]->image && borrowed[i]->view != borrowed[j]->view, "Motion blur borrowed inputs/output cannot alias.");
				Qualify(physical, *borrowed[i], VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (i == 4 ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0));
			}
			Require((depth.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && (output.usage & VK_IMAGE_USAGE_STORAGE_BIT), "Motion blur needs actual scene attachment depth and distinct storage HDR output.");
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties); const auto& limits = properties.limits;
			Require(properties.apiVersion >= VK_API_VERSION_1_3 && limits.maxPushConstantsSize >= sizeof(ConversionConstants) && limits.maxComputeWorkGroupInvocations >= 64
				&& limits.maxComputeWorkGroupSize[0] >= 8 && limits.maxComputeWorkGroupSize[1] >= 8 && limits.maxComputeWorkGroupSize[2] >= 1
				&& limits.maxPerStageDescriptorSampledImages >= 5 && limits.maxDescriptorSetSampledImages >= 5 && limits.maxPerStageDescriptorStorageImages >= 4 && limits.maxDescriptorSetStorageImages >= 4 && limits.maxPerStageResources >= 8
				&& (std::uint64_t(source.extent.width) + 7) / 8 <= limits.maxComputeWorkGroupCount[0] && (std::uint64_t(source.extent.height) + 7) / 8 <= limits.maxComputeWorkGroupCount[1] && limits.maxComputeWorkGroupCount[2] >= 1
				&& source.extent.width <= std::numeric_limits<std::int32_t>::max() - 128u && source.extent.height <= std::numeric_limits<std::int32_t>::max() - 128u, "Motion blur physical push/descriptor/workgroup limits are insufficient.");
			auto pass = std::make_shared<MotionBlurPass>(); pass->device = targets.device; pass->allocator = targets.allocator; pass->inputs = inputs; pass->key = targets.key;
			const auto slots = TemporalPassSlotCount(*inputs.previousDepthOwner); Require(slots >= 2 && slots <= 8, "Motion blur previous depth slot count is invalid."); pass->historyDepth.reserve(slots);
			// Allocation metadata is checked here; only the accepted per-record
			// history receipt below can certify which slot contains previous data.
			for(std::uint32_t slot = 0; slot < slots; ++slot) {
				auto history = ReadTemporalHistoryDepthTarget(*inputs.previousDepthOwner, slot);
				Require(history.image && history.view && history.format == VK_FORMAT_R32_SFLOAT && Extent(history.extent, source.extent) && (history.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Motion blur actual prior normalized depth target is unavailable.");
				for(const auto* image : borrowed) Require(history.image != image->image && history.view != image->view, "Motion blur history depth aliases current inputs.");
				Qualify(physical, history, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT); pass->historyDepth.push_back(history);
			}
			for(std::size_t i = 0; i < pass->converted.size(); ++i) {
				auto& target = pass->converted[i]; target.extent = source.extent; target.format = i == 1 || i == 2 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32_SFLOAT; target.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
				Qualify(physical, target, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
				VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = target.format; image.extent = {source.extent.width, source.extent.height, 1}; image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
				image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = target.usage; image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
				Require(vmaCreateImage(pass->allocator, &image, &allocation, &target.image, &target.allocation, nullptr) == VK_SUCCESS, "Motion blur converted-input allocation failed.");
				VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = target.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = target.format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				Require(vkCreateImageView(pass->device, &view, nullptr, &target.view) == VK_SUCCESS, "Motion blur converted-input view failed.");
			}
			CreateLayout(pass->device, kConversionTypes, sizeof(ConversionConstants), pass->descriptors[0], pass->layouts[0]); CreateLayout(pass->device, kBlurTypes, sizeof(BlurConstants), pass->descriptors[1], pass->layouts[1]);
			CreatePipeline(pass->device, "Shaders/PostProcess/MotionBlurInputs.comp.spv", pass->layouts[0], pass->pipelines[0], true, true);
			const bool rgba32 = output.format == VK_FORMAT_R32G32B32A32_SFLOAT; CreatePipeline(pass->device, rgba32 ? "Shaders/PostProcess/MotionBlur32.comp.spv" : "Shaders/PostProcess/MotionBlur16.comp.spv", pass->layouts[1], pass->pipelines[1], false, rgba32);
			const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, slots * 4 + 5}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slots * 4 + 1}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = slots + 1; pool.poolSizeCount = 2; pool.pPoolSizes = sizes.data(); Require(vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool) == VK_SUCCESS, "Motion blur descriptor pool failed.");
			const auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set) { VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; info.descriptorPool = pass->pool; info.descriptorSetCount = 1; info.pSetLayouts = &layout; Require(vkAllocateDescriptorSets(pass->device, &info, &set) == VK_SUCCESS, "Motion blur immutable descriptor allocation failed."); };
			const auto write = [&](VkDescriptorSet set, std::span<const VkDescriptorType> types, std::span<const VkDescriptorImageInfo> images) {
				std::array<VkWriteDescriptorSet, 8> writes{}; for(std::uint32_t i = 0; i < types.size(); ++i) { writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = types[i]; writes[i].pImageInfo = &images[i]; }
				vkUpdateDescriptorSets(pass->device, static_cast<std::uint32_t>(types.size()), writes.data(), 0, nullptr);
			};
			// Bind every possible old slot once. Frame recording selects a set,
			// avoiding mutable descriptors or pipeline/image allocation each frame.
			pass->conversionSets.resize(slots, VK_NULL_HANDLE);
			for(std::uint32_t slot = 0; slot < slots; ++slot) {
				allocate(pass->descriptors[0], pass->conversionSets[slot]);
				const std::array<VkDescriptorImageInfo, 8> images{{{VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, total.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, camera.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
					{VK_NULL_HANDLE, pass->historyDepth[slot].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pass->converted[0].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pass->converted[1].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pass->converted[2].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pass->converted[3].view, VK_IMAGE_LAYOUT_GENERAL}}}; write(pass->conversionSets[slot], kConversionTypes, images);
			}
			allocate(pass->descriptors[1], pass->blurSet);
			const std::array<VkDescriptorImageInfo, 6> images{{{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, pass->converted[0].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pass->converted[1].view, VK_IMAGE_LAYOUT_GENERAL},
				{VK_NULL_HANDLE, pass->converted[2].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pass->converted[3].view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL}}}; write(pass->blurSet, kBlurTypes, images); return pass;
		} catch(const std::exception& e) { reason = e.what(); return {}; } catch(...) { reason = "Motion blur preparation failed; previous usable owner remains retained."; return {}; }
	}

	namespace
	{
		bool StateProjection(const PostProcessHistoryState& state, const PostProcessTargetKey& key,
			std::uint64_t backend, std::uint64_t viewLifetime, VkExtent2D extent, glm::mat4& inverse) noexcept
		{
			if(!state.publication || !state.frameId || !state.recordingId || state.frameId != state.publication->frameId || state.presentationIndex >= state.publication->presentationInputs.size()
				|| state.publication->rendererSessionGeneration != key.rendererGeneration || state.publication->backendResourceGeneration != backend || state.scope.lifecycle.rendererSessionGeneration != key.rendererGeneration || state.scope.lifecycle.backendResourceGeneration != backend
				|| !state.scope.target.view || state.scope.target.view->lifetimeGeneration != viewLifetime || state.scope.target.view->outputId != key.viewId
				|| key.viewKind != (static_cast<std::uint32_t>(state.scope.target.view->outputKind) << 8u | static_cast<std::uint32_t>(state.scope.target.view->viewType))) return false;
			const auto& signature = state.signature;
			if(!signature.reconstructionGeneration || !signature.projectionGeneration || !signature.cutGeneration || !signature.worldOriginGeneration || !signature.surfaceGeneration
				|| signature.internalWidth != extent.width || signature.internalHeight != extent.height || signature.outputWidth != extent.width || signature.outputHeight != extent.height
				|| !signature.scaleNumerator || signature.scaleNumerator != signature.scaleDenominator) return false;
			const auto& view = state.publication->presentationInputs[state.presentationIndex];
			if(view.graphicsView != state.scope.target.view || view.graphicsCamera != state.scope.target.camera || !view.graphicsSettings || view.graphicsSettings->lifecycle != state.scope.lifecycle || view.graphicsSettings->sampleCount != 1
				|| view.output.targetSize.x != extent.width || view.output.targetSize.y != extent.height || view.output.kind != state.scope.target.view->outputKind || view.output.diagnosticsView != state.scope.target.view->viewType || view.output.outputId != state.scope.target.view->outputId) return false;
			if(std::any_of(key.cameraId.begin(), key.cameraId.end(), [](std::uint8_t v) { return v != 0; })) {
				if(!view.graphicsCamera || view.graphicsCamera->cameraId != key.cameraId || view.graphicsCamera->sceneGeneration != key.cameraGenerations[0] || view.graphicsCamera->objectLifetimeGeneration != key.cameraGenerations[1]
					|| view.graphicsCamera->componentGeneration != key.cameraGenerations[2] || view.graphicsCamera->bindingGeneration != key.cameraGenerations[3]) return false;
			}
			return InverseStandard(view.projectionMatrix, inverse);
		}
		// Join actual uploaded pose roots to the exact accepted depth slot. A
		// matching frame number alone is insufficient across views or recordings.
		bool FrameMatrices(const MotionBlurPass& pass, const MotionBlurFrameInput& frame, const GraphicsSettings::MotionBlurControls& controls, glm::mat4& currentInverse, glm::mat4& previousInverse) noexcept
		{
			if(!pass.inputs.motion || !pass.inputs.previousDepthOwner || !frame.motionOwner || !frame.previous || frame.motionProducer != pass.inputs.motion.get()) return false;
			const auto& prepared = frame.motionOwner->prepared; if(!prepared.current || !prepared.previous || !prepared.cpuHistoryCompatible || !(prepared.shader.state.x & 1u)) return false;
			const auto& current = *prepared.current; const auto& previous = *prepared.previous; const auto& proof = *frame.previous;
			if(current.scope != previous.scope || current.signature != previous.signature || frame.motionOwner->device != pass.device
				|| frame.sceneFrameId != current.frameId || frame.depthFrameId != current.frameId || frame.motionFrameId != current.frameId
				|| proof.passGeneration != TemporalPassGeneration(*pass.inputs.previousDepthOwner) || proof.slot >= pass.historyDepth.size() || proof.frameId != previous.frameId || proof.historyToken != previous.recordingId
				|| Token(prepared.shader.recordingTokens.x, prepared.shader.recordingTokens.y) != current.recordingId || Token(prepared.shader.recordingTokens.z, prepared.shader.recordingTokens.w) != previous.recordingId
				|| prepared.shader.extents.x != pass.inputs.sceneColor.extent.width || prepared.shader.extents.y != pass.inputs.sceneColor.extent.height || prepared.shader.extents.z != prepared.shader.extents.x || prepared.shader.extents.w != prepared.shader.extents.y
				|| current.signature.colorFormat != static_cast<std::uint32_t>(pass.inputs.sceneColor.format) || current.signature.depthFormat != static_cast<std::uint32_t>(pass.inputs.sceneDepth.format) || current.signature.motionFormat != static_cast<std::uint32_t>(pass.inputs.motion->totalMotion.format)) return false;
			if(!SameImage(ReadTemporalHistoryDepthTarget(*pass.inputs.previousDepthOwner, proof.slot), pass.historyDepth[proof.slot])) return false;
			if(!StateProjection(current, pass.key, pass.inputs.backendResourceGeneration, pass.inputs.viewLifetimeGeneration, pass.inputs.sceneColor.extent, currentInverse)
				|| !StateProjection(previous, pass.key, pass.inputs.backendResourceGeneration, pass.inputs.viewLifetimeGeneration, pass.inputs.sceneColor.extent, previousInverse)) return false;
			const auto& snapshot = *current.publication->presentationInputs[current.presentationIndex].graphicsSettings; GraphicsSettings::PostProcessEffectSettings consumed;
			return GraphicsSettings::ResolvePostProcessEffectSettings(snapshot.values, consumed) == GraphicsSettings::GraphicsRequestReason::None && consumed.motionBlur.enabled == controls.enabled
				&& consumed.motionBlur.strength == controls.strength && consumed.motionBlur.cameraContribution == controls.cameraContribution && consumed.motionBlur.objectContribution == controls.objectContribution;
		}
	}
	MotionBlurRecordResult RecordMotionBlurPass(VkCommandBuffer command, MotionBlurPass& pass, const GraphicsSettings::MotionBlurControls& controls,
		const MotionBlurFrameInput& frame, MotionBlurRecordedUse& recorded, const MotionBlurStyle& style) noexcept
	{
		recorded = {};
		if(!std::isfinite(controls.strength) || controls.strength < 0 || controls.strength > 2 || !std::isfinite(controls.cameraContribution) || controls.cameraContribution < 0 || controls.cameraContribution > 1
			|| !std::isfinite(controls.objectContribution) || controls.objectContribution < 0 || controls.objectContribution > 1 || !std::isfinite(style.maximumRadiusPixels) || style.maximumRadiusPixels < 0 || style.maximumRadiusPixels > 128
			|| !std::isfinite(style.relativeDepthTolerance) || style.relativeDepthTolerance < 0 || !std::isfinite(style.disocclusionAbsoluteMetres) || style.disocclusionAbsoluteMetres < 0 || !std::isfinite(style.disocclusionRelativeDepth) || style.disocclusionRelativeDepth < 0) return MotionBlurRecordResult::Rejected;
		if(!controls.Active() || style.maximumRadiusPixels == 0) return MotionBlurRecordResult::SkippedDisabled;
		// First frames/cuts keep the original HDR source. No dummy velocity or
		// constant-zero visibility mask is ever treated as fresh blur output.
		if(!frame.previous || (frame.motionOwner && (!frame.motionOwner->prepared.previous || !frame.motionOwner->prepared.cpuHistoryCompatible || !(frame.motionOwner->prepared.shader.state.x & 1u)))) return MotionBlurRecordResult::SkippedNoHistory;
		if(!command || !pass.pipelines[0] || !pass.pipelines[1]) return MotionBlurRecordResult::Rejected;
		for(const auto state : {frame.sceneColor, frame.sceneDepth, frame.totalMotion, frame.cameraMotion}) if(!state.stage || state.layout == VK_IMAGE_LAYOUT_UNDEFINED || state.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return MotionBlurRecordResult::Rejected;
		if(frame.previousDepth.layout != VK_IMAGE_LAYOUT_GENERAL || !frame.previousDepth.stage || !frame.output.stage || frame.output.layout == VK_IMAGE_LAYOUT_PREINITIALIZED
			|| (frame.privateInputLayout != VK_IMAGE_LAYOUT_UNDEFINED && frame.privateInputLayout != VK_IMAGE_LAYOUT_GENERAL)) return MotionBlurRecordResult::Rejected;
		glm::mat4 currentInverse, previousInverse; if(!FrameMatrices(pass, frame, controls, currentInverse, previousInverse)) return MotionBlurRecordResult::Rejected;
		const auto& prepared = frame.motionOwner->prepared; const auto jitter = prepared.shader.jitterUV;
		for(unsigned i = 0; i < 4; ++i) if(!std::isfinite(jitter[i])) return MotionBlurRecordResult::Rejected;
		const auto slot = frame.previous->slot; const auto& extent = pass.inputs.sceneColor.extent;
		Barrier(command, pass.inputs.sceneDepth, VK_IMAGE_ASPECT_DEPTH_BIT, frame.sceneDepth, kDepthRead);
		Barrier(command, pass.inputs.motion->totalMotion, VK_IMAGE_ASPECT_COLOR_BIT, frame.totalMotion, kSampled); Barrier(command, pass.inputs.motion->cameraMotion, VK_IMAGE_ASPECT_COLOR_BIT, frame.cameraMotion, kSampled);
		Barrier(command, pass.historyDepth[slot], VK_IMAGE_ASPECT_COLOR_BIT, frame.previousDepth, kRead);
		const MotionBlurImageState oldPrivate{frame.privateInputLayout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
		for(const auto& image : pass.converted) Barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, oldPrivate, kWrite);
		const ConversionConstants conversion{currentInverse, previousInverse, {extent.width, extent.height}, {jitter.x, jitter.y}, {jitter.z, jitter.w}, style.disocclusionAbsoluteMetres, style.disocclusionRelativeDepth, 1,
			pass.inputs.colorGrid == MotionBlurColorGrid::UnjitteredOutput ? 1u : 0u, 0, 0};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelines[0]); vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layouts[0], 0, 1, &pass.conversionSets[slot], 0, nullptr);
		vkCmdPushConstants(command, pass.layouts[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(conversion), &conversion); const auto x = (extent.width + 7) / 8, y = (extent.height + 7) / 8; vkCmdDispatch(command, x, y, 1);
		for(const auto& image : pass.converted) Barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kRead);
		Barrier(command, pass.inputs.sceneColor, VK_IMAGE_ASPECT_COLOR_BIT, frame.sceneColor, kSampled); Barrier(command, pass.inputs.output, VK_IMAGE_ASPECT_COLOR_BIT, frame.output, kWrite);
		const BlurConstants blur{{extent.width, extent.height}, controls.strength, controls.cameraContribution, controls.objectContribution, style.maximumRadiusPixels, style.relativeDepthTolerance, 1};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelines[1]); vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layouts[1], 0, 1, &pass.blurSet, 0, nullptr);
		vkCmdPushConstants(command, pass.layouts[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blur), &blur); vkCmdDispatch(command, x, y, 1);
		Barrier(command, pass.inputs.output, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kFinished); Barrier(command, pass.inputs.sceneColor, VK_IMAGE_ASPECT_COLOR_BIT, kSampled, frame.sceneColor);
		Barrier(command, pass.inputs.sceneDepth, VK_IMAGE_ASPECT_DEPTH_BIT, kDepthRead, frame.sceneDepth); Barrier(command, pass.inputs.motion->totalMotion, VK_IMAGE_ASPECT_COLOR_BIT, kSampled, frame.totalMotion);
		Barrier(command, pass.inputs.motion->cameraMotion, VK_IMAGE_ASPECT_COLOR_BIT, kSampled, frame.cameraMotion); Barrier(command, pass.historyDepth[slot], VK_IMAGE_ASPECT_COLOR_BIT, kRead, frame.previousDepth);
		recorded = {frame.sceneFrameId, frame.previous->frameId, frame.motionOwner, *frame.previous, pass.inputs.output.image, kFinished, VK_IMAGE_LAYOUT_GENERAL}; return MotionBlurRecordResult::Recorded;
	}
}
