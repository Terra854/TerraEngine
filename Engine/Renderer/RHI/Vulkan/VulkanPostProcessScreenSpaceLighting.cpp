#include "VulkanPostProcessScreenSpaceLighting.h"
#include "VulkanPostProcessScreenSpaceHiZ.h"
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
#include "Core/Logger/Logger.h"
#endif
#include "Renderer/Graph/RenderFramePublication.h"
#include "Resource/ResourceManager.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct AoConstants { std::uint32_t extent[2]; float radius, strength; std::uint32_t directions, steps; float bias, reserved; };
		struct BlurConstants { std::uint32_t extent[2]; float sigma, normalPower; };
		struct TraceConstants { std::uint32_t extent[2], steps, mip; float distance, thickness, strength; std::uint32_t flags; };
		struct CompositeConstants { std::uint32_t extent[2], enabledMask, inputFlags; };
		struct TraceView { glm::mat4 projection, inverse; };
		static_assert(sizeof(AoConstants) == 32 && offsetof(AoConstants, directions) == 16 && offsetof(AoConstants, bias) == 24);
		static_assert(sizeof(BlurConstants) == 16 && offsetof(BlurConstants, sigma) == 8);
		static_assert(sizeof(TraceConstants) == 32 && offsetof(TraceConstants, distance) == 16 && offsetof(TraceConstants, flags) == 28);
		static_assert(sizeof(CompositeConstants) == 16 && offsetof(CompositeConstants, enabledMask) == 8);
		static_assert(sizeof(TraceView) == 128 && offsetof(TraceView, inverse) == 64 && sizeof(glm::mat4) == 64);
		constexpr ScreenSpaceLightingImageState kRead{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr ScreenSpaceLightingImageState kDepthRead{VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		constexpr ScreenSpaceLightingImageState kWrite{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
		constexpr ScreenSpaceLightingImageState kFinished{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
		void Require(bool valid, const char* reason) { if(!valid) throw std::runtime_error(reason); }
		void Check(VkResult status, const char* reason) { Require(status == VK_SUCCESS, reason); }
		bool Hdr(VkFormat format) noexcept { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; }
		bool SameExtent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool SameImage(const ScreenSpaceInputImage& a, const ScreenSpaceInputImage& b) noexcept
		{
			return a.image && a.image == b.image && a.view == b.view && a.owner == b.owner && a.format == b.format
				&& a.usage == b.usage && SameExtent(a.extent, b.extent) && a.samples == b.samples && a.mipLevels == b.mipLevels;
		}
		std::uint32_t Mask(const ScreenSpaceInputDemand& demand) noexcept
		{
			return (demand.reflections ? 1u : 0u) | (demand.indirectLighting ? 2u : 0u) | (demand.ambientOcclusion ? 4u : 0u);
		}
		bool Finite(const glm::mat4& value) noexcept
		{
			for(std::size_t c = 0; c < 4; ++c) for(std::size_t r = 0; r < 4; ++r) if(!std::isfinite(value[c][r])) return false;
			return true;
		}
		bool StandardProjection(const glm::mat4& projection, glm::mat4& inverse) noexcept
		{
			if(!Finite(projection)) return false;
			const float determinant = glm::determinant(projection);
			if(!std::isfinite(determinant) || determinant == 0) return false;
			inverse = glm::inverse(projection); if(!Finite(inverse)) return false;
			const auto near = inverse * glm::vec4(0, 0, 0, 1), middle = inverse * glm::vec4(0, 0, 0.5f, 1);
			if(std::abs(near.w) < 1e-8f || std::abs(middle.w) < 1e-8f) return false;
			const float a = -near.z / near.w, b = -middle.z / middle.w;
			return std::isfinite(a) && std::isfinite(b) && a > 0 && b > a;
		}
		void Qualify(VkPhysicalDevice physical, const ScreenSpaceInputImage& image, bool requireHandles = true)
		{
			Require((!requireHandles || (image.image && image.view)) && image.extent.width && image.extent.height && image.mipLevels
				&& image.samples == VK_SAMPLE_COUNT_1_BIT, "Screen lighting actual image metadata is incomplete or multisampled.");
			VkFormatFeatureFlags required = 0;
			if(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if(image.usage & VK_IMAGE_USAGE_STORAGE_BIT) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if(image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if(image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if(image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
			if(image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			Require((format.optimalTilingFeatures & required) == required, "Screen lighting image lacks exact optimal format features.");
			VkImageFormatProperties limits{};
			Check(vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
				image.usage, 0, &limits), "Screen lighting complete format/usage query failed.");
			Require(image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height
				&& limits.maxExtent.depth >= 1 && limits.maxArrayLayers >= 1 && image.mipLevels <= limits.maxMipLevels
				&& (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "Screen lighting image exceeds exact native limits.");
			std::uint32_t maximumMips = 1;
			for(auto edge = std::max(image.extent.width, image.extent.height); edge > 1; edge >>= 1u) ++maximumMips;
			Require(image.mipLevels <= maximumMips, "Screen lighting image declares more mips than its actual extent can contain.");
		}
		struct OwnedImage { ScreenSpaceInputImage image; VmaAllocation allocation = VK_NULL_HANDLE; };
		struct ComputeStage
		{
			VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkDescriptorSet set = VK_NULL_HANDLE;
		};
		struct Binding
		{
			std::uint32_t binding;
			VkDescriptorType type;
			VkImageView view = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkDeviceSize offset = 0, bytes = 0;
		};
		Binding Sample(std::uint32_t binding, const ScreenSpaceInputImage& image, bool depth = false)
		{
			return {binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, image.view,
				depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		}
		Binding Storage(std::uint32_t binding, const ScreenSpaceInputImage& image)
		{
			return {binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, image.view, VK_IMAGE_LAYOUT_GENERAL};
		}
		Binding Uniform(std::uint32_t binding, VkDeviceSize offset, VkDeviceSize bytes)
		{
			return {binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, offset, bytes};
		}
		void Barrier(VkCommandBuffer command, const ScreenSpaceInputImage& image, bool depth,
			ScreenSpaceLightingImageState before, ScreenSpaceLightingImageState after) noexcept
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.image = image.image; barrier.oldLayout = before.layout; barrier.newLayout = after.layout;
			barrier.srcAccessMask = before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : before.access; barrier.dstAccessMask = after.access;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, image.mipLevels, 0, 1};
			vkCmdPipelineBarrier(command, before.stage, after.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		void Dispatch(VkCommandBuffer command, const ComputeStage& stage, const void* push, std::uint32_t bytes, VkExtent2D extent) noexcept
		{
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, stage.pipeline);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, stage.layout, 0, 1, &stage.set, 0, nullptr);
			vkCmdPushConstants(command, stage.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, push);
			vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
		}
	}

	struct ScreenSpaceLightingPass
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		std::shared_ptr<const ScreenSpaceRasterResources> raster;
		ScreenSpaceInputImage output;
		std::optional<ScreenSpaceInputImage> hiZ;
		std::shared_ptr<ScreenSpaceLightingHiZ> ownedHiZ;
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
		bool reportedOwnedHiZ = false;
#endif
		std::uint64_t generation = 0;
		std::uint32_t preparedMask = 0;
		std::array<OwnedImage, 6> scratch{}; // raw/blur AO, SSR color/confidence, SSGI color/confidence
		std::array<ComputeStage, 5> stages{}; // AO, blur, SSR, SSGI, composite
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkBuffer matrices = VK_NULL_HANDLE;
		VmaAllocation matrixAllocation = VK_NULL_HANDLE;
		VkDeviceSize matrixBytes = 0, aoOffset = 0;
		~ScreenSpaceLightingPass()
		{
			// Actual queue completion is established by the parent retirement owner.
			for(auto& stage : stages) if(stage.pipeline) vkDestroyPipeline(device, stage.pipeline, nullptr);
			if(pool) vkDestroyDescriptorPool(device, pool, nullptr);
			for(auto& stage : stages) if(stage.layout) vkDestroyPipelineLayout(device, stage.layout, nullptr);
			for(auto& stage : stages) if(stage.descriptors) vkDestroyDescriptorSetLayout(device, stage.descriptors, nullptr);
			if(matrices) vmaDestroyBuffer(allocator, matrices, matrixAllocation);
			for(auto& image : scratch)
			{
				if(image.image.view) vkDestroyImageView(device, image.image.view, nullptr);
				if(image.image.image) vmaDestroyImage(allocator, image.image.image, image.allocation);
			}
		}
	};

	namespace
	{
		void CreateImage(ScreenSpaceLightingPass& pass, VkPhysicalDevice physical, std::size_t index, VkFormat format)
		{
			auto& owned = pass.scratch[index]; auto& image = owned.image;
			image.owner = pass.raster->owner; image.extent = pass.raster->extent; image.format = format;
			image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			Qualify(physical, image, false);
			VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			create.imageType = VK_IMAGE_TYPE_2D; create.format = format; create.extent = {image.extent.width, image.extent.height, 1};
			create.mipLevels = create.arrayLayers = 1; create.samples = VK_SAMPLE_COUNT_1_BIT;
			create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = image.usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			Check(vmaCreateImage(pass.allocator, &create, &allocation, &image.image, &owned.allocation, nullptr), "Screen lighting scratch allocation failed.");
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			Check(vkCreateImageView(pass.device, &view, nullptr, &image.view), "Screen lighting scratch view failed.");
		}

		void ValidateModule(const std::vector<std::uint32_t>& words, const std::vector<Binding>& expected,
			std::uint32_t pushBytes, bool rgba32)
		{
			Require(words.size() >= 5 && words[0] == 0x07230203u, "Screen lighting module has no SPIR-V header.");
			std::map<std::uint32_t, std::uint32_t> bindings, sets, variables, storage, pointers, formats, sampledKinds;
			std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
			std::uint32_t push = 0, openFunctions = 0, completedFunctions = 0; bool entry = false, local = false;
			for(std::size_t i = 5; i < words.size();)
			{
				const auto count = words[i] >> 16, op = words[i] & 65535u;
				Require(count && count <= words.size() - i, "Screen lighting SPIR-V instruction is truncated.");
				if(op == 17) Require(count == 2 && (words[i + 1] == 1 || words[i + 1] == 50), "Screen lighting module needs unqualified optional features.");
				if(op == 54) { Require(!openFunctions, "Screen lighting SPIR-V has nested functions."); openFunctions = 1; }
				if(op == 56) { Require(openFunctions == 1 && count == 1, "Screen lighting SPIR-V has an unmatched function end."); openFunctions = 0; ++completedFunctions; }
				if(op == 15 && count >= 5 && words[i + 1] == 5)
				{
					const auto* name = reinterpret_cast<const char*>(words.data() + i + 3);
					entry = std::memchr(name, 0, (count - 3) * 4) && std::strcmp(name, "main") == 0;
				}
				if(op == 16 && count == 6 && words[i + 2] == 17) local = words[i + 3] == 8 && words[i + 4] == 8 && words[i + 5] == 1;
				if(op == 71 && count == 4 && words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
				if(op == 71 && count == 4 && words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
				if(op == 72 && count == 5 && words[i + 3] == 35) offsets[words[i + 1]][words[i + 2]] = words[i + 4];
				if(op == 59 && count >= 4)
				{
					variables[words[i + 2]] = words[i + 1]; storage[words[i + 2]] = words[i + 3];
					if(words[i + 3] == 9) push = words[i + 1];
				}
				if(op == 32 && count == 4) pointers[words[i + 1]] = words[i + 3];
				if(op == 25 && count >= 9)
				{
					Require(words[i + 3] == 1 && words[i + 5] == 0 && words[i + 6] == 0, "Screen lighting descriptor must be a nonarray single-sample 2D image.");
					formats[words[i + 1]] = words[i + 8]; sampledKinds[words[i + 1]] = words[i + 7];
				}
				i += count;
			}
			Require(entry && local && !openFunctions && completedFunctions && bindings.size() == expected.size() && pointers.contains(push), "Screen lighting entry/workgroup/descriptors disagree with native ABI.");
			for(const auto& item : expected)
			{
				const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& pair) { return pair.second == item.binding; });
				Require(found != bindings.end() && sets.contains(found->first) && sets.at(found->first) == 0
					&& variables.contains(found->first) && pointers.contains(variables.at(found->first)), "Screen lighting binding/set is missing.");
				const auto type = pointers.at(variables.at(found->first));
				if(item.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
				{
					Require(storage.at(found->first) == 2 && offsets.contains(type) && offsets.at(type).at(0) == 0,
						"Screen lighting matrix UBO layout is invalid.");
					Require((item.bytes == 64 && offsets.at(type).size() == 1) || item.bytes == 128,
						"Screen lighting matrix UBO range differs from the shader block.");
					if(item.bytes == 128) Require(offsets.at(type).size() == 2 && offsets.at(type).at(1) == 64, "Screen lighting projection UBO offsets are invalid.");
				}
				else
				{
					Require(storage.at(found->first) == 0 && formats.contains(type), "Screen lighting descriptor is not a direct image.");
					Require(sampledKinds.at(type) == (item.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 2u : 1u),
						"Screen lighting sampled/storage descriptor kind disagrees with the shader.");
					if(item.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
					{
						const bool scalar = (item.binding == 2 && pushBytes == 32 && expected.size() == 4)
							|| (item.binding == 3 && expected.size() == 5) || item.binding == 6;
						Require(formats.at(type) == (scalar ? 3u : rgba32 ? 1u : 2u), "Screen lighting storage format differs from the allocated image.");
					}
				}
			}
			const auto& pushOffsets = offsets.at(pointers.at(push));
			const std::vector<std::uint32_t> wanted = pushBytes == 16 ? std::vector<std::uint32_t>{0, 8, 12}
				: std::vector<std::uint32_t>{0, 8, 12, 16, 20, 24, 28};
			Require(pushOffsets.size() == wanted.size(), "Screen lighting push member count differs.");
			for(std::size_t i = 0; i < wanted.size(); ++i) Require(pushOffsets.at(static_cast<std::uint32_t>(i)) == wanted[i], "Screen lighting push offset differs.");
		}

		void CreateStage(ScreenSpaceLightingPass& pass, std::size_t index, const std::string& file,
			const std::vector<Binding>& bindings, std::uint32_t pushBytes)
		{
			auto& stage = pass.stages[index]; std::vector<VkDescriptorSetLayoutBinding> declarations;
			for(const auto& binding : bindings) declarations.push_back({binding.binding, binding.type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
			VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			descriptors.bindingCount = static_cast<std::uint32_t>(declarations.size()); descriptors.pBindings = declarations.data();
			Check(vkCreateDescriptorSetLayout(pass.device, &descriptors, nullptr, &stage.descriptors), "Screen lighting descriptor layout failed.");
			VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout.setLayoutCount = 1; layout.pSetLayouts = &stage.descriptors; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
			Check(vkCreatePipelineLayout(pass.device, &layout, nullptr, &stage.layout), "Screen lighting pipeline layout failed.");
			auto& resources = ResourceManager::Instance(); const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(file));
			Require(!bytes.empty() && bytes.size() % 4 == 0, "Installed screen lighting shader is missing or malformed.");
			std::vector<std::uint32_t> words(bytes.size() / 4); std::memcpy(words.data(), bytes.data(), bytes.size());
			ValidateModule(words, bindings, pushBytes, pass.output.format == VK_FORMAT_R32G32B32A32_SFLOAT);
			VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize = bytes.size(); shader.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE; Check(vkCreateShaderModule(pass.device, &shader, nullptr, &module), "Screen lighting shader module failed.");
			struct ModuleOwner { VkDevice device; VkShaderModule module; ~ModuleOwner() { vkDestroyShaderModule(device, module, nullptr); } } moduleOwner{pass.device, module};
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline.layout = stage.layout;
			pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			pipeline.stage.module = module; pipeline.stage.pName = "main";
			Check(vkCreateComputePipelines(pass.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &stage.pipeline), "Screen lighting compute pipeline failed.");
			VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocate.descriptorPool = pass.pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &stage.descriptors;
			Check(vkAllocateDescriptorSets(pass.device, &allocate, &stage.set), "Screen lighting descriptor allocation failed.");
			std::vector<VkDescriptorImageInfo> images(bindings.size()); std::vector<VkDescriptorBufferInfo> buffers(bindings.size());
			std::vector<VkWriteDescriptorSet> writes(bindings.size());
			for(std::size_t i = 0; i < bindings.size(); ++i)
			{
				const auto& binding = bindings[i]; auto& write = writes[i]; write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				write.dstSet = stage.set; write.dstBinding = binding.binding; write.descriptorCount = 1; write.descriptorType = binding.type;
				if(binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) { buffers[i] = {pass.matrices, binding.offset, binding.bytes}; write.pBufferInfo = &buffers[i]; }
				else { images[i] = {VK_NULL_HANDLE, binding.view, binding.layout}; write.pImageInfo = &images[i]; }
			}
			vkUpdateDescriptorSets(pass.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
	}

	std::shared_ptr<ScreenSpaceLightingPass> PrepareScreenSpaceLightingPass(const ScreenSpaceLightingCreateInfo& info, std::string& reason)
	{
		reason.clear();
		if(!info.demand.Any()) return {};
		try
		{
			Require(info.physicalDevice && info.device && info.allocator && info.raster && info.raster->renderPass && info.raster->framebuffer,
				"Screen lighting requires the real prepared raster source and native device ownership.");
			const auto& raster = *info.raster; const auto extent = raster.extent; const auto mask = Mask(raster.demand);
			Require(mask && mask == Mask(info.demand) && info.samples == VK_SAMPLE_COUNT_1_BIT && !info.variableRateShading
				&& raster.device == info.device && raster.allocator == info.allocator,
				"Screen lighting initially excludes MSAA/VRS and mismatched raster device ownership.");
			Require(raster.owner.lifecycle.rendererSessionGeneration && raster.owner.lifecycle.backendResourceGeneration
				&& raster.owner.view.lifetimeGeneration && raster.owner.sceneTargetGeneration,
				"Screen lighting requires a complete renderer/view/scene-target generation.");
			Require(Hdr(raster.colors[0].format) && info.output.format == raster.colors[0].format
				&& info.output.owner == raster.owner && SameExtent(info.output.extent, extent) && info.output.mipLevels == 1
				&& (info.output.usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
				"Screen lighting output must be a distinct same-view/extent/format HDR sampled storage image.");
			std::array<const ScreenSpaceInputImage*, 9> images{}; std::size_t count = 0;
			auto validate = [&](const ScreenSpaceInputImage& image) {
				Require(image.owner == raster.owner && SameExtent(image.extent, extent) && image.mipLevels == 1
					&& (image.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Screen lighting borrowed role belongs to another view/extent or lacks readable usage.");
				Qualify(info.physicalDevice, image);
				for(std::size_t i = 0; i < count; ++i) Require(images[i]->image != image.image && images[i]->view != image.view,
					"Independent screen lighting role images cannot alias.");
				images[count++] = &image;
			};
			for(std::size_t logical = 0; logical < raster.colors.size(); ++logical)
			{
				if(logical == 1) continue; // Integer identity remains retained by the raster owner, not sampled as color.
				const bool needed = logical < 3 || ((logical == 3 || logical == 5) ? (mask & 5u) != 0 : (mask & 6u) != 0);
				if(!needed) continue;
				validate(raster.colors[logical]);
				Require(raster.colors[logical].format == (logical == 2 ? VK_FORMAT_R16G16B16A16_SFLOAT : raster.colors[0].format),
					"Screen lighting normal/incident/material-weight format differs from its surface ABI.");
			}
			Require((raster.depth.format == VK_FORMAT_D32_SFLOAT || raster.depth.format == VK_FORMAT_D16_UNORM)
				&& (raster.depth.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT), "Screen lighting requires qualified single-sample standard-Z depth.");
			validate(raster.depth); validate(info.output);
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(info.physicalDevice, &properties); const auto& limits = properties.limits;
			Require(properties.apiVersion >= VK_API_VERSION_1_3 && limits.maxPushConstantsSize >= 32
				&& limits.maxComputeWorkGroupInvocations >= 64 && limits.maxComputeWorkGroupSize[0] >= 8 && limits.maxComputeWorkGroupSize[1] >= 8
				&& limits.maxComputeWorkGroupSize[2] >= 1 && (std::uint64_t(extent.width) + 7) / 8 <= limits.maxComputeWorkGroupCount[0]
				&& (std::uint64_t(extent.height) + 7) / 8 <= limits.maxComputeWorkGroupCount[1] && limits.maxComputeWorkGroupCount[2] >= 1
				&& limits.maxPerStageDescriptorSampledImages >= 11 && limits.maxDescriptorSetSampledImages >= 11
				&& limits.maxPerStageDescriptorStorageImages >= 2 && limits.maxDescriptorSetStorageImages >= 2
				&& limits.maxPerStageDescriptorUniformBuffers >= 1 && limits.maxDescriptorSetUniformBuffers >= 1
				&& limits.maxPerStageResources >= 12 && limits.maxUniformBufferRange >= 128
				&& extent.width <= std::numeric_limits<std::int32_t>::max() - 64u && extent.height <= std::numeric_limits<std::int32_t>::max() - 64u,
				"Screen lighting compute, dispatch, descriptor or matrix limits are insufficient.");
			auto pass = std::make_shared<ScreenSpaceLightingPass>();
			pass->device = info.device; pass->allocator = info.allocator; pass->raster = info.raster; pass->output = info.output; pass->preparedMask = mask;
			std::string hierarchyFailure;
			if((mask & 3u) && info.hiZ)
			{
				try
				{
					const auto& hierarchy = *info.hiZ;
					Require(hierarchy.owner == raster.owner && SameExtent(hierarchy.extent, extent) && hierarchy.format == VK_FORMAT_R32_SFLOAT
						&& (hierarchy.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Screen lighting HiZ is not a real same-view standard-Z MAX hierarchy.");
					Qualify(info.physicalDevice, hierarchy);
					for(std::size_t i = 0; i < count; ++i) Require(images[i]->image != hierarchy.image && images[i]->view != hierarchy.view,
						"A declared shared HiZ pyramid must not alias scene role images.");
					pass->hiZ = hierarchy;
				}
				catch(const std::exception& error) { hierarchyFailure = error.what(); }
			}
			if((mask & 3u) && !pass->hiZ && info.prepareSameFrameHiZ)
			{
				// Both radiance tracers actually sample this shared hierarchy. The
				// independent owner makes optional failure transactional: a missing
				// shader/format/allocation never rejects valid direct-depth lighting.
				try
				{
					auto& resources = ResourceManager::Instance();
					const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath("Shaders/PostProcess/HiZGenerate.comp.spv"));
					Require(!bytes.empty() && bytes.size() % 4 == 0, "Installed optional same-frame Hi-Z shader is missing or malformed.");
					std::vector<std::uint32_t> words(bytes.size()/4); std::memcpy(words.data(), bytes.data(), bytes.size());
					ScreenSpaceLightingHiZCreateInfo hierarchy;
					hierarchy.physicalDevice = info.physicalDevice; hierarchy.device = info.device; hierarchy.allocator = info.allocator;
					hierarchy.depth = raster.depth; hierarchy.shader = words;
					pass->ownedHiZ = PrepareScreenSpaceLightingHiZ(hierarchy, hierarchyFailure);
					if(pass->ownedHiZ) pass->hiZ = DescribeScreenSpaceLightingHiZ(*pass->ownedHiZ);
				}
				catch(const std::exception& error) { hierarchyFailure = error.what(); }
			}
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
			if((mask & 3u) && !pass->hiZ && !hierarchyFailure.empty())
			{
				try { TERRAENGINE_LOG(LogLevel::Debug, "Optional screen lighting Hi-Z unavailable; direct same-frame depth retained: " << hierarchyFailure); }
				catch(...) {} // Optional diagnostics cannot reject valid direct lighting.
			}
#endif
			static std::atomic<std::uint64_t> generations{0}; auto previous = generations.load(std::memory_order_relaxed);
			while(previous != std::numeric_limits<std::uint64_t>::max() && !generations.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {}
			Require(previous != std::numeric_limits<std::uint64_t>::max(), "Screen lighting pass generation space is exhausted."); pass->generation = previous + 1;
			if(mask & 4u) { CreateImage(*pass, info.physicalDevice, 0, VK_FORMAT_R32_SFLOAT); CreateImage(*pass, info.physicalDevice, 1, VK_FORMAT_R32_SFLOAT); }
			if(mask & 1u) { CreateImage(*pass, info.physicalDevice, 2, info.output.format); CreateImage(*pass, info.physicalDevice, 3, VK_FORMAT_R32_SFLOAT); }
			if(mask & 2u) { CreateImage(*pass, info.physicalDevice, 4, info.output.format); CreateImage(*pass, info.physicalDevice, 5, VK_FORMAT_R32_SFLOAT); }
			const auto alignment = std::max<VkDeviceSize>(limits.minUniformBufferOffsetAlignment, 1);
			Require(alignment <= std::numeric_limits<VkDeviceSize>::max() - 128, "Screen lighting UBO alignment overflows.");
			pass->aoOffset = mask & 3u ? ((128 + alignment - 1) / alignment) * alignment : 0;
			Require(pass->aoOffset <= std::numeric_limits<VkDeviceSize>::max() - 64, "Screen lighting UBO size overflows.");
			pass->matrixBytes = mask & 4u ? pass->aoOffset + 64 : 128;
			VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size = pass->matrixBytes;
			buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			Check(vmaCreateBuffer(pass->allocator, &buffer, &allocation, &pass->matrices, &pass->matrixAllocation, nullptr), "Screen lighting persistent matrix buffer failed.");
			const std::uint32_t aoStages = mask & 4u ? 2u : 0u, reflectionStages = mask & 1u ? 1u : 0u, indirectStages = mask & 2u ? 1u : 0u;
			const auto sampled = 2u + (mask & 4u ? 6u : 0u) + reflectionStages * 7u + indirectStages * 7u
				+ (mask & 5u ? 2u : 0u) + (mask & 6u ? 2u : 0u);
			const std::array<VkDescriptorPoolSize, 3> poolSizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u + aoStages + reflectionStages * 2u + indirectStages * 2u},
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, aoStages + reflectionStages + indirectStages}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1u + aoStages + reflectionStages + indirectStages;
			pool.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()); pool.pPoolSizes = poolSizes.data();
			Check(vkCreateDescriptorPool(pass->device, &pool, nullptr, &pass->pool), "Screen lighting descriptor pool failed.");
			if(mask & 4u)
			{
				CreateStage(*pass, 0, "Shaders/PostProcess/GTAOView.comp.spv", {Sample(0, raster.depth, true), Sample(1, raster.colors[2]),
					Storage(2, pass->scratch[0].image), Uniform(3, pass->aoOffset, 64)}, 32);
				CreateStage(*pass, 1, "Shaders/PostProcess/GTAOBlurView.comp.spv", {Sample(0, raster.depth, true), Sample(1, raster.colors[2]),
					Sample(2, pass->scratch[0].image), Storage(3, pass->scratch[1].image), Uniform(4, pass->aoOffset, 64)}, 16);
			}
			const std::string suffix = info.output.format == VK_FORMAT_R32G32B32A32_SFLOAT ? "32.comp.spv" : "16.comp.spv";
			for(std::size_t index = 0; index < 2; ++index)
			{
				if(!(mask & (1u << index))) continue;
				const auto offset = index == 0 ? 2u : 4u;
				// Without optional HiZ, binding3 truthfully borrows actual current
				// depth. The shader's pyramid flag stays OFF; no fake pyramid exists.
				const auto hierarchy = pass->hiZ ? Sample(3, *pass->hiZ) : Sample(3, raster.depth, true);
				CreateStage(*pass, index + 2, std::string("Shaders/PostProcess/") + (index == 0 ? "ScreenSpaceReflections" : "ScreenSpaceIndirect") + suffix,
					{Sample(0, raster.depth, true), Sample(1, raster.colors[2]), Sample(2, raster.colors[0]), hierarchy,
					Sample(4, raster.colors[index == 0 ? 3 : 4]), Storage(5, pass->scratch[offset].image),
					Storage(6, pass->scratch[offset + 1].image), Uniform(7, 0, 128)}, 32);
			}
			std::vector<Binding> composition{Sample(0, raster.colors[0]), Storage(8, info.output), Sample(9, raster.colors[2])};
			if(mask & 4u) composition.push_back(Sample(1, pass->scratch[1].image));
			if(mask & 1u) { composition.push_back(Sample(2, pass->scratch[2].image)); composition.push_back(Sample(10, pass->scratch[3].image)); }
			if(mask & 5u) { composition.push_back(Sample(3, raster.colors[3])); composition.push_back(Sample(4, raster.colors[5])); }
			if(mask & 2u) { composition.push_back(Sample(5, pass->scratch[4].image)); composition.push_back(Sample(11, pass->scratch[5].image)); }
			if(mask & 6u) { composition.push_back(Sample(6, raster.colors[4])); composition.push_back(Sample(7, raster.colors[6])); }
			CreateStage(*pass, 4, "Shaders/PostProcess/ScreenSpaceComposite-Mask" + std::to_string(mask) + "-" + suffix, composition, 16);
			return pass;
		}
		catch(const std::exception& error) { reason = error.what(); return {}; }
		catch(...) { reason = "Screen lighting resource preparation failed without replacing its prior owner."; return {}; }
	}

	namespace
	{
		bool SameControls(const GraphicsSettings::ScreenSpaceEffectControls& a, const GraphicsSettings::ScreenSpaceEffectControls& b) noexcept
		{
			return a.enabled == b.enabled && a.intensity == b.intensity && a.quality == b.quality;
		}
		bool UsableState(ScreenSpaceLightingImageState state, bool depth = false) noexcept
		{
			if(!state.stage) return false;
			if(depth) return state.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || state.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			return state.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || state.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || state.layout == VK_IMAGE_LAYOUT_GENERAL;
		}
		bool ValidStyle(const ScreenSpaceLightingStyle& style) noexcept
		{
			return std::isfinite(style.aoRadius) && style.aoRadius > 0 && std::isfinite(style.aoBias) && style.aoBias >= 0
				&& std::isfinite(style.aoDepthSigma) && style.aoDepthSigma > 0 && std::isfinite(style.aoNormalPower) && style.aoNormalPower >= 1
				&& std::isfinite(style.traceDistance) && style.traceDistance > 0 && std::isfinite(style.traceThickness)
				&& style.traceThickness > 0 && style.traceThickness < style.traceDistance * 0.25f;
		}
		bool OutputState(ScreenSpaceLightingImageState state) noexcept
		{
			if(!state.stage) return false;
			return state.layout == VK_IMAGE_LAYOUT_UNDEFINED || state.layout == VK_IMAGE_LAYOUT_GENERAL
				|| state.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || state.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				|| state.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || state.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		}
		bool QualifyFrame(VkCommandBuffer command, const ScreenSpaceLightingPass& pass,
			const GraphicsSettings::PostProcessEffectSettings& controls, const ScreenSpaceLightingFrameInput& frame, glm::mat4& inverse) noexcept
		{
			if(!command || !frame.publication || !pass.raster || !frame.geometry.frameId || frame.geometry.command != command
				|| frame.geometry.owner != pass.raster->owner || frame.geometry.frameId != frame.publication->frameId
				|| frame.geometry.samples != VK_SAMPLE_COUNT_1_BIT || frame.geometry.variableRateShading
				|| frame.geometry.originalAo == ScreenSpaceOriginalAo::Unknown
				|| frame.publication->rendererSessionGeneration != pass.raster->owner.lifecycle.rendererSessionGeneration
				|| frame.publication->backendResourceGeneration != pass.raster->owner.lifecycle.backendResourceGeneration
				|| frame.presentationIndex >= frame.publication->presentationInputs.size()) return false;
			const auto& input = frame.publication->presentationInputs[frame.presentationIndex];
			if(!input.graphicsSettings || !input.graphicsView || *input.graphicsView != pass.raster->owner.view
				|| input.graphicsSettings->lifecycle != pass.raster->owner.lifecycle || !input.graphicsSettings->revision
				|| input.graphicsSettings->publishedFrameId > frame.geometry.frameId || input.graphicsSettings->sampleCount != 1
				|| input.settings.variableRateShading != 0
				|| input.output.kind != input.graphicsView->outputKind || input.output.diagnosticsView != input.graphicsView->viewType
				|| input.output.outputId != input.graphicsView->outputId
				|| input.graphicsSettings->internalWidth != pass.raster->extent.width || input.graphicsSettings->internalHeight != pass.raster->extent.height
				|| input.graphicsSettings->outputWidth != pass.raster->extent.width || input.graphicsSettings->outputHeight != pass.raster->extent.height) return false;
			// The standalone swapchain publication can leave extent implicit.
			// Its exact same-view native size was checked against the settings above.
			const bool implicitRuntimeExtent = input.output.kind == RenderOutputKind::MainRuntimeView
				&& input.output.targetSize == glm::uvec2(0u);
			if(!implicitRuntimeExtent && (input.output.targetSize.x != pass.raster->extent.width
				|| input.output.targetSize.y != pass.raster->extent.height)) return false;
			if(input.graphicsSettings->target.view && *input.graphicsSettings->target.view != *input.graphicsView) return false;
			if(input.graphicsSettings->target.camera && (!input.graphicsCamera || *input.graphicsSettings->target.camera != *input.graphicsCamera)) return false;
			GraphicsSettings::PostProcessEffectSettings consumed;
			if(GraphicsSettings::ResolvePostProcessEffectSettings(input.graphicsSettings->values, consumed) != GraphicsSettings::GraphicsRequestReason::None
				|| !SameControls(consumed.gtao, controls.gtao) || !SameControls(consumed.reflections, controls.reflections)
				|| !SameControls(consumed.indirect, controls.indirect)) return false;
			for(std::size_t i = 0; i < pass.raster->colors.size(); ++i)
			{
				const auto& expected = pass.raster->colors[i]; const auto& actual = frame.geometry.colors[i];
				if(expected.image)
				{
					if(!SameImage(expected, actual) || (i != 1 && !UsableState(frame.geometry.colorStates[i]))) return false;
				}
				else if(actual.image || actual.view) return false;
			}
			if(!SameImage(pass.raster->depth, frame.geometry.depth) || !UsableState(frame.geometry.depthState, true)) return false;
			if(pass.ownedHiZ)
			{
				if(frame.hiZ || !CanRecordScreenSpaceLightingHiZ(command, *pass.ownedHiZ, frame.geometry)) return false;
			}
			else if(pass.hiZ)
			{
				if(!frame.hiZ || frame.hiZ->command != command || frame.hiZ->frameId != frame.geometry.frameId
					|| !SameImage(*pass.hiZ, frame.hiZ->image) || !UsableState(frame.hiZ->state)) return false;
			}
			else if(frame.hiZ) return false;
			return StandardProjection(input.projectionMatrix, inverse);
		}
	}

	ScreenSpaceLightingRecordResult RecordScreenSpaceLightingPass(VkCommandBuffer command, ScreenSpaceLightingPass& pass,
		const GraphicsSettings::PostProcessEffectSettings& controls, const ScreenSpaceLightingFrameInput& frame,
		ScreenSpaceLightingRecordedUse& recorded, const ScreenSpaceLightingStyle& style,
		ScreenSpaceLightingStageMarker marker, void* markerContext) noexcept
	{
		recorded = {}; glm::mat4 inverse;
		if(!ValidStyle(style) || !QualifyFrame(command, pass, controls, frame, inverse)) return ScreenSpaceLightingRecordResult::Rejected;
		const std::uint32_t mask = (controls.reflections.Active() ? 1u : 0u) | (controls.indirect.Active() ? 2u : 0u) | (controls.gtao.Active() ? 4u : 0u);
		if(!mask) return ScreenSpaceLightingRecordResult::Skipped;
		// Toggle/zero-amplitude changes alter the descriptor topology and require
		// a new prepared owner. Strength/quality changes within it allocate nothing.
		if(mask != pass.preparedMask || ((mask & 4u) && frame.geometry.originalAo != ScreenSpaceOriginalAo::LegacyScreenAoOmitted)
			|| !OutputState(frame.outputState)) return ScreenSpaceLightingRecordResult::Rejected;
		for(std::size_t stage = 0; stage < pass.stages.size(); ++stage)
		{
			const bool needed = stage < 2 ? (mask & 4u) != 0 : stage < 4 ? (mask & (1u << (stage - 2))) != 0 : true;
			if(needed && (!pass.stages[stage].pipeline || !pass.stages[stage].layout || !pass.stages[stage].set)) return ScreenSpaceLightingRecordResult::Rejected;
		}
		const auto extent = pass.raster->extent;
		const auto& input = frame.publication->presentationInputs[frame.presentationIndex];
		// This persistent device-local UBO is updated by GPU commands, never CPU
		// mapped writes. A WAR barrier protects prior in-flight reads on the same
		// serialized queue, then transfer writes become visible to these dispatches.
		VkBufferMemoryBarrier matrices{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; matrices.buffer = pass.matrices;
		matrices.offset = 0; matrices.size = VK_WHOLE_SIZE;
		matrices.srcQueueFamilyIndex = matrices.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		matrices.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; matrices.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &matrices, 0, nullptr);
		if(mask & 3u)
		{
			const TraceView view{input.projectionMatrix, inverse};
			vkCmdUpdateBuffer(command, pass.matrices, 0, sizeof(view), &view);
		}
		if(mask & 4u) vkCmdUpdateBuffer(command, pass.matrices, pass.aoOffset, sizeof(inverse), &inverse);
		matrices.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; matrices.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &matrices, 0, nullptr);
		Barrier(command, pass.raster->depth, true, frame.geometry.depthState, kDepthRead);
		for(std::size_t logical = 0; logical < pass.raster->colors.size(); ++logical)
			if(logical != 1 && pass.raster->colors[logical].image) Barrier(command, pass.raster->colors[logical], false, frame.geometry.colorStates[logical], kRead);
		std::uint32_t ownedHiZDispatches = 0;
		if(pass.ownedHiZ)
		{
			ScreenSpaceHiZRecordedUse hierarchy;
			// Both radiance passes share this cost. Measure the real mip writes
			// separately so neither tracer double-counts the hierarchy build.
			if(marker) marker(markerContext, ScreenSpaceLightingStage::DepthHierarchy, true);
			if(!RecordScreenSpaceLightingHiZ(command, *pass.ownedHiZ, frame.geometry, hierarchy, ownedHiZDispatches))
				return ScreenSpaceLightingRecordResult::Rejected;
			if(marker) marker(markerContext, ScreenSpaceLightingStage::DepthHierarchy, false);
			// Every mip is now a current-command read dependency. No stored
			// accepted/previous-frame layout or producer stamp was updated.
		}
		else if(pass.hiZ) Barrier(command, *pass.hiZ, false, frame.hiZ->state, kRead);
		const ScreenSpaceLightingImageState discarded{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0};
		if(mask & 4u)
		{
			if(marker) marker(markerContext, ScreenSpaceLightingStage::GTAO, true);
			const auto quality = static_cast<std::uint32_t>(controls.gtao.quality);
			const AoConstants ao{{extent.width, extent.height}, style.aoRadius, controls.gtao.intensity,
				4u << quality, 4u << quality, style.aoBias, 0};
			Barrier(command, pass.scratch[0].image, false, discarded, kWrite);
			Dispatch(command, pass.stages[0], &ao, sizeof(ao), extent);
			Barrier(command, pass.scratch[0].image, false, kWrite, kRead);
			const BlurConstants blur{{extent.width, extent.height}, style.aoDepthSigma, style.aoNormalPower};
			Barrier(command, pass.scratch[1].image, false, discarded, kWrite);
			Dispatch(command, pass.stages[1], &blur, sizeof(blur), extent);
			Barrier(command, pass.scratch[1].image, false, kWrite, kRead);
			if(marker) marker(markerContext, ScreenSpaceLightingStage::GTAO, false);
		}
		for(std::size_t index = 0; index < 2; ++index)
		{
			if(!(mask & (1u << index))) continue;
			const auto stage = index == 0 ? ScreenSpaceLightingStage::Reflections : ScreenSpaceLightingStage::Indirect;
			if(marker) marker(markerContext, stage, true);
			const auto& control = index == 0 ? controls.reflections : controls.indirect;
			const auto quality = static_cast<std::uint32_t>(control.quality), scratch = index == 0 ? 2u : 4u;
			const TraceConstants trace{{extent.width, extent.height}, 64u << quality, pass.hiZ ? pass.hiZ->mipLevels - 1 : 0u,
				style.traceDistance, style.traceThickness, control.intensity, pass.hiZ ? 2u : 0u};
			Barrier(command, pass.scratch[scratch].image, false, discarded, kWrite);
			Barrier(command, pass.scratch[scratch + 1].image, false, discarded, kWrite);
			Dispatch(command, pass.stages[index + 2], &trace, sizeof(trace), extent);
			Barrier(command, pass.scratch[scratch].image, false, kWrite, kRead);
			Barrier(command, pass.scratch[scratch + 1].image, false, kWrite, kRead);
			if(marker) marker(markerContext, stage, false);
		}
		if(marker) marker(markerContext, ScreenSpaceLightingStage::Composite, true);
		Barrier(command, pass.output, false, frame.outputState, kWrite);
		const CompositeConstants composite{{extent.width, extent.height}, mask,
			frame.geometry.originalAo == ScreenSpaceOriginalAo::LegacyScreenAoOmitted ? 1u : 0u};
		Dispatch(command, pass.stages[4], &composite, sizeof(composite), extent);
		Barrier(command, pass.output, false, kWrite, kFinished);
		if(marker) marker(markerContext, ScreenSpaceLightingStage::Composite, false);
		// Restore borrowed input layouts; exact returned output state belongs to
		// this recording proof and is adopted only after the real queue accepts it.
		if(pass.hiZ && !pass.ownedHiZ) Barrier(command, *pass.hiZ, false, kRead, frame.hiZ->state);
		for(std::size_t logical = 0; logical < pass.raster->colors.size(); ++logical)
			if(logical != 1 && pass.raster->colors[logical].image) Barrier(command, pass.raster->colors[logical], false, kRead, frame.geometry.colorStates[logical]);
		Barrier(command, pass.raster->depth, true, kDepthRead, frame.geometry.depthState);
		recorded = {frame.geometry.frameId, input.graphicsSettings->revision, pass.generation, pass.raster->owner, pass.output.image, kFinished, mask};
		recorded.usedOwnedHiZ = bool(pass.ownedHiZ);
		recorded.hiZMipLevels = pass.hiZ ? pass.hiZ->mipLevels : 0;
		recorded.ownedHiZDispatches = ownedHiZDispatches;
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
		// Bounded validation-only observation: one actual recording receipt per
		// prepared owner, never unconditional per-frame logging or GPU completion.
		if(recorded.usedOwnedHiZ && !pass.reportedOwnedHiZ)
		{
			try
			{
				TERRAENGINE_LOG(LogLevel::Info, "Screen lighting same-frame MAX Hi-Z recorded frame=" << recorded.frameId
					<< " mips=" << recorded.hiZMipLevels << " dispatches=" << recorded.ownedHiZDispatches);
			}
			catch(...) {} // Diagnostics cannot throw through this native recorder.
			pass.reportedOwnedHiZ = true;
		}
#endif
		return ScreenSpaceLightingRecordResult::Recorded;
	}
}
