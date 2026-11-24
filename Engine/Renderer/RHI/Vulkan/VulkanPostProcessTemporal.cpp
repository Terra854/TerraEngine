// VulkanPostProcessTemporal.cpp - immutable bindings and explicit slot ownership.
#include "VulkanPostProcessTemporal.h"
#include "Resource/ResourceManager.h"
#include <algorithm>
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
		struct ResolveConstants
		{
			std::uint32_t internal[2], output[2], historyValid, flags;
			float historyWeight, absoluteDepthTolerance, relativeDepthTolerance, neighborhoodDepthTolerance;
			std::uint32_t reserved0 = 0, reserved1 = 0;
			float currentJitter[2], previousJitter[2];
		};
		struct SharpenConstants { std::uint32_t extent[2]; float strength; std::uint32_t reserved = 0; };
		static_assert(sizeof(ResolveConstants) == 64 && offsetof(ResolveConstants, historyWeight) == 24
			&& offsetof(ResolveConstants, currentJitter) == 48 && offsetof(ResolveConstants, previousJitter) == 56);
		static_assert(sizeof(SharpenConstants) == 16 && offsetof(SharpenConstants, strength) == 8);
		constexpr VkImageUsageFlags kHistoryUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		constexpr std::array<VkDescriptorType, 7> kResolveTypes{
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		constexpr std::array<VkDescriptorType, 2> kSharpenTypes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		void Require(bool condition, const char* reason) { if(!condition) throw std::runtime_error(reason); }
		bool SameExtent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool FloatColor(VkFormat format) noexcept { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; }
		bool DepthFormat(VkFormat format, VkImageAspectFlags aspect) noexcept
		{
			if(aspect == VK_IMAGE_ASPECT_COLOR_BIT) return format == VK_FORMAT_R32_SFLOAT;
			// Combined depth/stencil images need separately qualified layout features
			// or joint-aspect transitions; do not assume those optional features.
			return aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM);
		}
		void Qualify(VkPhysicalDevice physical, const PostProcessTargetImage& image, VkFormatFeatureFlags features)
		{
			Require(image.extent.width && image.extent.height && image.usage, "Temporal image metadata is incomplete.");
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			Require((format.optimalTilingFeatures & features) == features, "Temporal image format lacks required optimal features.");
			VkImageFormatProperties limits{};
			Require(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
				image.usage, 0, &limits) == VK_SUCCESS, "Temporal exact image usage is unsupported.");
			Require(image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height
				&& limits.maxExtent.depth >= 1 && limits.maxMipLevels >= 1 && limits.maxArrayLayers >= 1
				&& (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "Temporal image extent or sample count exceeds format support.");
		}
		void Barrier(VkCommandBuffer command, const PostProcessTargetImage& image, VkImageAspectFlags aspect,
			TemporalImageState before, TemporalImageState after) noexcept
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : before.access;
			barrier.dstAccessMask = after.access; barrier.oldLayout = before.layout; barrier.newLayout = after.layout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image.image; barrier.subresourceRange = {aspect, 0, 1, 0, 1};
			vkCmdPipelineBarrier(command, before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : before.stage,
				after.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		constexpr TemporalImageState kRead{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr TemporalImageState kWrite{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
		constexpr TemporalImageState kFinished{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		TemporalImageState HistoryBefore(VkImageLayout layout) noexcept
		{
			return {layout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
		}
		void CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, PostProcessTargetImage& image)
		{
			image.format = format; image.extent = extent; image.usage = kHistoryUsage;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
			info.extent = {extent.width, extent.height, 1}; info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = kHistoryUsage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			Require(vmaCreateImage(allocator, &info, &allocation, &image.image, &image.allocation, nullptr) == VK_SUCCESS, "Temporal history image allocation failed.");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = image.image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			Require(vkCreateImageView(device, &view, nullptr, &image.view) == VK_SUCCESS, "Temporal history image view failed.");
		}
		void CreateLayout(VkDevice device, std::span<const VkDescriptorType> types, std::uint32_t pushBytes,
			VkDescriptorSetLayout& descriptors, VkPipelineLayout& pipeline)
		{
			std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
			for(std::uint32_t i = 0; i < types.size(); ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			info.bindingCount = static_cast<std::uint32_t>(types.size()); info.pBindings = bindings.data();
			Require(vkCreateDescriptorSetLayout(device, &info, nullptr, &descriptors) == VK_SUCCESS, "Temporal descriptor layout creation failed.");
			VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1;
			layout.pSetLayouts = &descriptors; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
			Require(vkCreatePipelineLayout(device, &layout, nullptr, &pipeline) == VK_SUCCESS, "Temporal pipeline layout creation failed.");
		}
		// Validate installed module descriptors, image formats, push offsets and
		// workgroup before creating an executable. This does not replace native
		// view/motion/history qualification or advertise reconstruction support.
		void ValidateModule(const std::vector<std::uint32_t>& words, bool resolve, bool rgba32)
		{
			Require(words.size() >= 5 && words[0] == 0x07230203u, "Temporal SPIR-V header is invalid.");
			std::map<std::uint32_t, std::uint32_t> bindings, sets, variables, pointers, images;
			std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
			std::uint32_t pushPointer = 0; bool localSize = false, entry = false;
			for(std::size_t i = 5; i < words.size();) {
				const auto count = words[i] >> 16, op = words[i] & 0xffffu;
				Require(count && count <= words.size() - i, "Temporal SPIR-V instruction is truncated.");
				if(op == 17) Require(count == 2 && (words[i + 1] == 1 || words[i + 1] == 50), "Temporal module requires unqualified optional shader features.");
				if(op == 15 && count >= 5 && words[i + 1] == 5) {
					const auto* name = reinterpret_cast<const char*>(words.data() + i + 3);
					entry = std::memchr(name, 0, (count - 3) * 4) != nullptr && std::strcmp(name, "main") == 0;
				}
				if(op == 16 && count == 6 && words[i + 2] == 17) localSize = words[i + 3] == 8 && words[i + 4] == 8 && words[i + 5] == 1;
				if(op == 71 && count == 4 && words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
				if(op == 71 && count == 4 && words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
				if(op == 72 && count == 5 && words[i + 3] == 35) offsets[words[i + 1]][words[i + 2]] = words[i + 4];
				if(op == 59 && count >= 4) { variables[words[i + 2]] = words[i + 1]; if(words[i + 3] == 9) pushPointer = words[i + 1]; }
				if(op == 32 && count == 4) pointers[words[i + 1]] = words[i + 3];
				if(op == 25 && count >= 9) images[words[i + 1]] = words[i + 8];
				i += count;
			}
			const std::size_t expected = resolve ? 7 : 2;
			Require(entry && localSize && bindings.size() == expected, "Temporal entry/workgroup/descriptor count disagrees with prepared ABI.");
			std::array<bool, 7> found{};
			for(const auto& [id, binding] : bindings) {
				Require(binding < expected && !found[binding] && sets.contains(id) && sets.at(id) == 0, "Temporal descriptor set or binding disagrees with ABI."); found[binding] = true;
				Require(variables.contains(id) && pointers.contains(variables.at(id)) && images.contains(pointers.at(variables.at(id))), "Temporal descriptor is not a direct image.");
				const auto imageFormat = images.at(pointers.at(variables.at(id)));
				const bool storageColor = binding == (resolve ? 5u : 1u);
				Require(storageColor ? imageFormat == (rgba32 ? 1u : 2u) : (resolve && binding == 6 ? imageFormat == 3 : imageFormat == 0), "Temporal storage image variant disagrees with its actual format.");
			}
			Require(pushPointer && pointers.contains(pushPointer), "Temporal push constant owner is missing.");
			const std::vector<std::uint32_t> expectedOffsets = resolve ? std::vector<std::uint32_t>{0, 8, 16, 20, 24, 28, 32, 36, 40, 44, 48, 56} : std::vector<std::uint32_t>{0, 8, 12};
			const auto& actual = offsets[pointers.at(pushPointer)];
			Require(actual.size() == expectedOffsets.size(), "Temporal push constant member count changed.");
			for(std::uint32_t i = 0; i < expectedOffsets.size(); ++i) Require(actual.contains(i) && actual.at(i) == expectedOffsets[i], "Temporal push constant offsets changed.");
		}
		void CreatePipeline(VkDevice device, VkPipelineLayout layout, VkPipeline& pipeline, bool resolve, bool rgba32)
		{
			const std::string path = std::string("Shaders/PostProcess/Temporal") + (resolve ? "Resolve" : "Sharpen") + (rgba32 ? "32.comp.spv" : "16.comp.spv");
			auto& resources = ResourceManager::Instance(); const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			Require(!bytes.empty() && bytes.size() % 4 == 0, "Temporal exact installed shader variant is missing.");
			std::vector<std::uint32_t> words(bytes.size() / 4); std::memcpy(words.data(), bytes.data(), bytes.size()); ValidateModule(words, resolve, rgba32);
			VkShaderModule module = VK_NULL_HANDLE;
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = bytes.size(); info.pCode = words.data();
			Require(vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS, "Temporal shader module creation failed.");
			VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; compute.layout = layout;
			compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module = module; compute.stage.pName = "main";
			const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline);
			vkDestroyShaderModule(device, module, nullptr); Require(result == VK_SUCCESS, "Temporal compute pipeline creation failed.");
		}
	}

	struct TemporalPass
	{
		struct Slot { PostProcessTargetImage color, depth; };
		VkDevice device = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE;
		PostProcessTargetKey key;
		TemporalPassInputs inputs; std::uint64_t generation = 0;
		std::vector<Slot> slots;
		VkDescriptorSetLayout resolveDescriptors = VK_NULL_HANDLE, sharpenDescriptors = VK_NULL_HANDLE;
		VkPipelineLayout resolveLayout = VK_NULL_HANDLE, sharpenLayout = VK_NULL_HANDLE;
		VkPipeline resolvePipeline = VK_NULL_HANDLE, sharpenPipeline = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		std::vector<VkDescriptorSet> resolveSets, sharpenSets;
		~TemporalPass()
		{
			// Parent retirement must prove all descriptor/image borrowers complete.
			if(resolvePipeline) vkDestroyPipeline(device, resolvePipeline, nullptr);
			if(sharpenPipeline) vkDestroyPipeline(device, sharpenPipeline, nullptr);
			if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			if(resolveLayout) vkDestroyPipelineLayout(device, resolveLayout, nullptr);
			if(sharpenLayout) vkDestroyPipelineLayout(device, sharpenLayout, nullptr);
			if(resolveDescriptors) vkDestroyDescriptorSetLayout(device, resolveDescriptors, nullptr);
			if(sharpenDescriptors) vkDestroyDescriptorSetLayout(device, sharpenDescriptors, nullptr);
			for(auto& slot : slots) for(auto* image : {&slot.color, &slot.depth}) {
				if(image->view) vkDestroyImageView(device, image->view, nullptr);
				if(image->image) vmaDestroyImage(allocator, image->image, image->allocation);
			}
		}
	};

	std::shared_ptr<TemporalPass> PrepareTemporalPass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const TemporalPassInputs& inputs, std::string& reason)
	{
		reason.clear();
		try
		{
			Require(physical && targets.device && targets.allocator && inputs.sceneSamples == VK_SAMPLE_COUNT_1_BIT && inputs.historySlotCount >= 2 && inputs.historySlotCount <= 8,
				"Temporal preparation requires an actual device/allocator and2..8 history slots.");
			const auto& source = inputs.sceneColor; const auto& depth = inputs.sceneDepth; const auto& motion = inputs.motion; const auto& output = inputs.output;
			Require(FloatColor(source.format) && output.format == source.format && FloatColor(motion.format)
				&& DepthFormat(depth.format, inputs.depthAspect), "Temporal typed input/output formats or depth aspect are unsupported.");
			Require(SameExtent(source.extent, depth.extent) && SameExtent(source.extent, motion.extent)
				&& SameExtent(source.extent, targets.internalExtent) && SameExtent(output.extent, targets.outputExtent), "Temporal internal/output extents disagree with target ownership.");
			const std::array<const PostProcessTargetImage*, 4> borrowed{&source, &depth, &motion, &output};
			for(std::size_t i = 0; i < borrowed.size(); ++i) {
				Require(borrowed[i]->image && borrowed[i]->view, "Temporal borrowed image/view is missing.");
				for(std::size_t j = 0; j < i; ++j) Require(borrowed[i]->image != borrowed[j]->image && borrowed[i]->view != borrowed[j]->view, "Temporal source/depth/motion/output must be distinct images.");
				Require((borrowed[i]->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0, "Temporal image is not sampled-capable.");
				if(i == 3) Require((borrowed[i]->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0, "Temporal output is not storage-capable.");
				Qualify(physical, *borrowed[i], VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (i == 3 ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0));
			}
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties); const auto& limits = properties.limits;
			Require(properties.apiVersion >= VK_API_VERSION_1_3 && limits.maxComputeWorkGroupInvocations >= 64
				&& limits.maxComputeWorkGroupSize[0] >= 8 && limits.maxComputeWorkGroupSize[1] >= 8 && limits.maxComputeWorkGroupSize[2] >= 1
				&& limits.maxPushConstantsSize >= sizeof(ResolveConstants) && limits.maxPerStageDescriptorSampledImages >= 5
				&& limits.maxDescriptorSetSampledImages >= 5 && limits.maxPerStageDescriptorStorageImages >= 2
				&& limits.maxDescriptorSetStorageImages >= 2 && limits.maxPerStageResources >= 7
				&& (std::uint64_t(output.extent.width) + 7) / 8 <= limits.maxComputeWorkGroupCount[0]
				&& (std::uint64_t(output.extent.height) + 7) / 8 <= limits.maxComputeWorkGroupCount[1]
				&& limits.maxComputeWorkGroupCount[2] >= 1
				&& output.extent.width <= std::numeric_limits<std::int32_t>::max() - 7u
				&& output.extent.height <= std::numeric_limits<std::int32_t>::max() - 7u
				&& source.extent.width <= std::numeric_limits<std::int32_t>::max()
				&& source.extent.height <= std::numeric_limits<std::int32_t>::max(), "Temporal compute, descriptor, or dispatch limits are insufficient.");
			for(const auto format : {output.format, VK_FORMAT_R32_SFLOAT}) {
				PostProcessTargetImage description; description.extent = output.extent; description.format = format; description.usage = kHistoryUsage;
				Qualify(physical, description, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
			}
			auto pass = std::make_shared<TemporalPass>(); pass->device = targets.device; pass->allocator = targets.allocator; pass->inputs = inputs;
			pass->key = targets.key;
			static std::atomic<std::uint64_t> generations{0}; auto value = generations.load(std::memory_order_relaxed);
			while(value != std::numeric_limits<std::uint64_t>::max() && !generations.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {}
			Require(value != std::numeric_limits<std::uint64_t>::max(), "Temporal pass generation space is exhausted."); pass->generation = value + 1;
			pass->slots.resize(inputs.historySlotCount);
			for(auto& slot : pass->slots) { CreateImage(pass->device, pass->allocator, output.format, output.extent, slot.color); CreateImage(pass->device, pass->allocator, VK_FORMAT_R32_SFLOAT, output.extent, slot.depth); }
			CreateLayout(pass->device, kResolveTypes, sizeof(ResolveConstants), pass->resolveDescriptors, pass->resolveLayout);
			CreateLayout(pass->device, kSharpenTypes, sizeof(SharpenConstants), pass->sharpenDescriptors, pass->sharpenLayout);
			CreatePipeline(pass->device, pass->resolveLayout, pass->resolvePipeline, true, output.format == VK_FORMAT_R32G32B32A32_SFLOAT);
			CreatePipeline(pass->device, pass->sharpenLayout, pass->sharpenPipeline, false, output.format == VK_FORMAT_R32G32B32A32_SFLOAT);
			const auto n = inputs.historySlotCount, pairs = n * (n - 1);
			const std::array<VkDescriptorPoolSize, 2> poolSizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pairs * 5 + n}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pairs * 2 + n}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = pairs + n;
			pool.poolSizeCount = 2; pool.pPoolSizes = poolSizes.data(); Require(vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool) == VK_SUCCESS, "Temporal descriptor pool failed.");
			pass->resolveSets.resize(n * n, VK_NULL_HANDLE); pass->sharpenSets.resize(n, VK_NULL_HANDLE);
			const auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set) {
				VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = pass->pool;
				allocation.descriptorSetCount = 1; allocation.pSetLayouts = &layout;
				Require(vkAllocateDescriptorSets(pass->device, &allocation, &set) == VK_SUCCESS, "Temporal immutable descriptor allocation failed.");
			};
			const auto write = [&](VkDescriptorSet set, std::span<const VkDescriptorType> types, std::span<const VkImageView> views) {
				std::array<VkDescriptorImageInfo, 7> images{}; std::array<VkWriteDescriptorSet, 7> writes{};
				for(std::uint32_t i = 0; i < types.size(); ++i) {
					images[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL}; writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = types[i]; writes[i].pImageInfo = &images[i];
				}
				vkUpdateDescriptorSets(pass->device, static_cast<std::uint32_t>(types.size()), writes.data(), 0, nullptr);
			};
			for(std::uint32_t destination = 0; destination < n; ++destination) {
				for(std::uint32_t previous = 0; previous < n; ++previous) if(destination != previous) {
					auto& set = pass->resolveSets[destination * n + previous]; allocate(pass->resolveDescriptors, set);
					const std::array<VkImageView, 7> views{source.view, depth.view, motion.view, pass->slots[previous].color.view,
						pass->slots[previous].depth.view, pass->slots[destination].color.view, pass->slots[destination].depth.view}; write(set, kResolveTypes, views);
				}
				allocate(pass->sharpenDescriptors, pass->sharpenSets[destination]);
				const std::array<VkImageView, 2> views{pass->slots[destination].color.view, output.view}; write(pass->sharpenSets[destination], kSharpenTypes, views);
			}
			return pass;
		}
		catch(const std::exception& e) { reason = e.what(); return {}; }
		catch(...) { reason = "Temporal resource preparation failed; previous owner remains usable."; return {}; }
	}

	bool RecordTemporalPass(VkCommandBuffer command, TemporalPass& pass,
		const TemporalRecordRequest& request, TemporalRecordedUse& recorded) noexcept
	{
		// Validate every request field before recording the first GPU operation.
		// A failed call must not leave a caller's previous receipt looking new.
		recorded = {};
		const auto n = pass.slots.size();
		if(!command || !pass.resolvePipeline || !pass.sharpenPipeline || !request.recordingId || !request.frameId || !request.historyToken
			|| request.history.size() != n || request.writeSlot >= n || request.readSlot >= n || request.writeSlot == request.readSlot
			|| !std::isfinite(request.historyWeight) || request.historyWeight < 0 || request.historyWeight > 0.98f
			|| !std::isfinite(request.absoluteDepthTolerance) || request.absoluteDepthTolerance < 0
			|| !std::isfinite(request.relativeDepthTolerance) || request.relativeDepthTolerance < 0
			|| !std::isfinite(request.neighborhoodDepthTolerance) || request.neighborhoodDepthTolerance < 0
			|| !std::isfinite(request.sharpenStrength) || request.sharpenStrength < 0 || request.sharpenStrength > 1) return false;
		for(const auto value : request.currentJitterUV) if(!std::isfinite(value)) return false;
		for(const auto value : request.previousJitterUV) if(!std::isfinite(value)) return false;
		for(const auto state : {request.sceneColor, request.sceneDepth, request.motion})
			if(state.layout == VK_IMAGE_LAYOUT_UNDEFINED || state.layout == VK_IMAGE_LAYOUT_PREINITIALIZED || !state.stage) return false;
		if(!request.output.stage || request.output.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return false;
		const auto& oldRead = request.history[request.readSlot]; const auto& oldWrite = request.history[request.writeSlot];
		const auto privateLayout = [](VkImageLayout layout) { return layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_GENERAL; };
		if(!privateLayout(oldRead.colorLayout) || !privateLayout(oldRead.depthLayout) || !privateLayout(oldWrite.colorLayout) || !privateLayout(oldWrite.depthLayout)) return false;
		if(request.previous && (request.previous->passGeneration != pass.generation || request.previous->slot != request.readSlot
			|| !request.previous->frameId || !request.previous->historyToken || request.previous->historyToken == request.historyToken
			|| oldRead.colorLayout != VK_IMAGE_LAYOUT_GENERAL || oldRead.depthLayout != VK_IMAGE_LAYOUT_GENERAL)) return false;
		const auto& inputs = pass.inputs; const auto& read = pass.slots[request.readSlot]; const auto& write = pass.slots[request.writeSlot];
		Barrier(command, inputs.sceneColor, VK_IMAGE_ASPECT_COLOR_BIT, request.sceneColor, kRead);
		Barrier(command, inputs.sceneDepth, inputs.depthAspect, request.sceneDepth, kRead);
		Barrier(command, inputs.motion, VK_IMAGE_ASPECT_COLOR_BIT, request.motion, kRead);
		// Even a reset frame binds a distinct legal previous slot in GENERAL. Its
		// undefined contents are never sampled when previous is absent/historyValid0.
		Barrier(command, read.color, VK_IMAGE_ASPECT_COLOR_BIT, HistoryBefore(oldRead.colorLayout), kRead);
		Barrier(command, read.depth, VK_IMAGE_ASPECT_COLOR_BIT, HistoryBefore(oldRead.depthLayout), kRead);
		Barrier(command, write.color, VK_IMAGE_ASPECT_COLOR_BIT, HistoryBefore(oldWrite.colorLayout), kWrite);
		Barrier(command, write.depth, VK_IMAGE_ASPECT_COLOR_BIT, HistoryBefore(oldWrite.depthLayout), kWrite);
		ResolveConstants constants{{inputs.sceneColor.extent.width, inputs.sceneColor.extent.height}, {inputs.output.extent.width, inputs.output.extent.height},
			request.previous ? 1u : 0u, request.reverseDepth ? 1u : 0u, request.historyWeight, request.absoluteDepthTolerance,
			request.relativeDepthTolerance, request.neighborhoodDepthTolerance, 0, 0,
			{request.currentJitterUV[0], request.currentJitterUV[1]}, {request.previousJitterUV[0], request.previousJitterUV[1]}};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.resolvePipeline);
		const auto set = pass.resolveSets[request.writeSlot * n + request.readSlot];
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.resolveLayout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(command, pass.resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
		const auto groupsX = (inputs.output.extent.width + 7) / 8, groupsY = (inputs.output.extent.height + 7) / 8;
		vkCmdDispatch(command, groupsX, groupsY, 1);
		Barrier(command, write.color, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kRead);
		Barrier(command, write.depth, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kRead);
		Barrier(command, inputs.output, VK_IMAGE_ASPECT_COLOR_BIT, request.output, kWrite);
		const SharpenConstants sharpen{{inputs.output.extent.width, inputs.output.extent.height}, request.sharpenStrength, 0};
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.sharpenPipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.sharpenLayout, 0, 1, &pass.sharpenSets[request.writeSlot], 0, nullptr);
		vkCmdPushConstants(command, pass.sharpenLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sharpen), &sharpen);
		vkCmdDispatch(command, groupsX, groupsY, 1);
		Barrier(command, inputs.output, VK_IMAGE_ASPECT_COLOR_BIT, kWrite, kFinished);
		Barrier(command, inputs.sceneColor, VK_IMAGE_ASPECT_COLOR_BIT, kRead, request.sceneColor);
		Barrier(command, inputs.sceneDepth, inputs.depthAspect, kRead, request.sceneDepth);
		Barrier(command, inputs.motion, VK_IMAGE_ASPECT_COLOR_BIT, kRead, request.motion);
		recorded = {pass.generation, request.recordingId, request.frameId, request.historyToken, request.writeSlot, request.readSlot,
			{VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL}, {VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL}, kFinished};
		return true;
	}
	std::uint64_t TemporalPassGeneration(const TemporalPass& pass) noexcept { return pass.generation; }
	std::uint32_t TemporalPassSlotCount(const TemporalPass& pass) noexcept { return static_cast<std::uint32_t>(pass.slots.size()); }
	PostProcessTargetImage ReadTemporalHistoryDepthTarget(const TemporalPass& pass, std::uint32_t slot) noexcept
	{
		return slot < pass.slots.size() ? pass.slots[slot].depth : PostProcessTargetImage{};
	}
	PostProcessTargetKey TemporalPassTargetKey(const TemporalPass& pass) noexcept { return pass.key; }
	VkDevice TemporalPassDevice(const TemporalPass& pass) noexcept { return pass.device; }
}
