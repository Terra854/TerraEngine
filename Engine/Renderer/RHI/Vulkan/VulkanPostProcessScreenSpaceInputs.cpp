// VulkanPostProcessScreenSpaceInputs.cpp - Demand-only native input ownership.
#include "VulkanPostProcessScreenSpaceInputs.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace TerraEngine::Renderer::Vulkan
{
	ScreenSpaceInputResources::~ScreenSpaceInputResources()
	{
		// The parent retains borrowed images and proves GPU completion before
		// releasing this owner. Only the optional new normal target belongs here.
		if (!normalsAllocation || !normalsRoughness) return;
		if (device && normalsRoughness->view) vkDestroyImageView(device, normalsRoughness->view, nullptr);
		if (allocator && normalsRoughness->image) vmaDestroyImage(allocator, normalsRoughness->image, normalsAllocation);
	}

	namespace
	{
		void Require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
		void Check(VkResult result, const char* detail) { Require(result == VK_SUCCESS, detail); }
		bool SameExtent(VkExtent2D a, VkExtent2D b) noexcept { return a.width == b.width && a.height == b.height; }
		bool HdrFormat(VkFormat format) noexcept { return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT; }
		std::uint64_t PixelBytes(VkFormat format) noexcept
		{
			switch (format) {
			case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
			case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
			case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_R32_SFLOAT: return 4;
			case VK_FORMAT_D16_UNORM: return 2;
			default: return 0;
			}
		}
		bool QualifiesImage(VkPhysicalDevice physical, const ScreenSpaceInputImage& image) noexcept
		{
			if (!image.extent.width || !image.extent.height || !image.mipLevels || !PixelBytes(image.format)) return false;
			VkFormatFeatureFlags required = 0;
			if (image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if (image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if (image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if (image.usage & VK_IMAGE_USAGE_STORAGE_BIT) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if (image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
			if (image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, image.format, &format);
			if ((format.optimalTilingFeatures & required) != required) return false;
			VkImageFormatProperties properties{};
			if (vkGetPhysicalDeviceImageFormatProperties(physical, image.format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, image.usage, 0, &properties) != VK_SUCCESS
				|| !(properties.sampleCounts & image.samples) || image.extent.width > properties.maxExtent.width
				|| image.extent.height > properties.maxExtent.height || properties.maxExtent.depth < 1
				|| properties.maxArrayLayers < 1 || image.mipLevels > properties.maxMipLevels) return false;
			std::uint32_t maximumMips = 1;
			for (auto edge = std::max(image.extent.width, image.extent.height); edge > 1; edge >>= 1U) ++maximumMips;
			if (image.mipLevels > maximumMips) return false;
			// Sum exact logical mip sizes with checked arithmetic. The native query
			// remains authoritative for the complete format/usage combination.
			std::uint64_t totalBytes = 0;
			auto width = image.extent.width, height = image.extent.height;
			for (std::uint32_t mip = 0; mip < image.mipLevels; ++mip)
			{
				const auto bytes = PixelBytes(image.format);
				const auto limit = std::numeric_limits<std::uint64_t>::max();
				if (std::uint64_t(width) > limit / height / bytes) return false;
				const auto size = std::uint64_t(width) * height * bytes;
				if (totalBytes > limit - size) return false;
				totalBytes += size;
				width = std::max(1U, width / 2); height = std::max(1U, height / 2);
			}
			return totalBytes <= properties.maxResourceSize;
		}
	}

	ScreenSpaceInputPreparation PrepareScreenSpaceInputResources(const ScreenSpaceInputCreateInfo& info)
	{
		ScreenSpaceInputPreparation result;
		// This branch deliberately precedes even physical-device validation. No
		// exclusively used input or allocation survives an all-disabled candidate.
		if (!info.demand.Any()) { result.state = ScreenSpaceInputPreparationState::Disabled; return result; }
		try
		{
			Require(info.physicalDevice && info.device && info.extent.width && info.extent.height, "Screen lighting inputs require a live device and nonempty extent.");
			Require(info.owner.lifecycle.rendererSessionGeneration && info.owner.lifecycle.backendResourceGeneration
				&& info.owner.view.lifetimeGeneration && info.owner.sceneTargetGeneration, "Screen lighting inputs require an exact native view lifetime and scene target owner.");
			std::array<VkImage, 6> used{}; std::size_t usedCount = 0;
			const auto validate = [&](const ScreenSpaceInputImage& image) {
				Require(image.image && image.view && image.owner == info.owner && SameExtent(image.extent, info.extent)
					&& image.samples == VK_SAMPLE_COUNT_1_BIT && (image.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Screen lighting input is missing or belongs to a different view lifetime, extent or sample count.");
				Require(!info.expectedFrameId || image.producedFrameId == info.expectedFrameId, "Screen lighting input has no exact current-frame producer proof.");
				Require(QualifiesImage(info.physicalDevice, image), "The exact screen input format, usage, mip range or extent is unsupported.");
				Require(std::find(used.begin(), used.begin() + usedCount, image.image) == used.begin() + usedCount, "Independent scene/depth/normal/lighting inputs must not alias the same image.");
				used[usedCount++] = image.image;
			};
			Require((info.depth.format == VK_FORMAT_D32_SFLOAT || info.depth.format == VK_FORMAT_D16_UNORM)
				&& info.depth.mipLevels == 1, "Screen lighting depth must be native standard-Z readable single-mip depth.");
			validate(info.depth);
			if (info.demand.NeedsRadiance())
			{
				Require(HdrFormat(info.sceneRadiance.image.format), "Screen lighting radiance must be scene-linear HDR before camera exposure.");
				validate(info.sceneRadiance.image);
			}
			if (info.demand.NeedsNormals() && info.normalsRoughness)
			{
				Require(info.normalsRoughness->format == VK_FORMAT_R16G16B16A16_SFLOAT && info.normalsRoughness->mipLevels == 1,
					"Screen lighting normals require signed view-space XYZ and perceptual roughness W in RGBA16F.");
				validate(*info.normalsRoughness);
			}
			if (info.demand.reflections)
			{
				Require(info.environmentSpecular && HdrFormat(info.environmentSpecular->image.format), "SSR requires a separate HDR environment-specular incident-radiance producer.");
				validate(info.environmentSpecular->image);
			}
			if (info.demand.indirectLighting)
			{
				Require(info.indirectDiffuse && HdrFormat(info.indirectDiffuse->image.format), "Screen indirect light requires a separate HDR diffuse-indirect incident-radiance producer.");
				validate(info.indirectDiffuse->image);
			}
			const bool useHiZ = info.demand.NeedsRadiance() && info.demand.sharedHiZ && info.hiZ.has_value();
			if (useHiZ)
			{
				Require(info.hiZ->format == VK_FORMAT_R32_SFLOAT, "Shared screen lighting Hi-Z must be a same-view standard-Z MAX float pyramid.");
				validate(*info.hiZ);
			}
			const bool createNormals = info.demand.NeedsNormals() && !info.normalsRoughness;
			ScreenSpaceInputImage normal;
			if (createNormals)
			{
				Require(info.geometrySamples == VK_SAMPLE_COUNT_1_BIT, "Owned screen normals require a qualified single-sample geometry producer; MSAA needs an explicitly resolved input.");
				Require(info.allocator, "Owned screen normals require a live VMA allocator.");
				VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(info.physicalDevice, &properties);
				Require(info.extent.width <= properties.limits.maxFramebufferWidth && info.extent.height <= properties.limits.maxFramebufferHeight
					&& properties.limits.maxColorAttachments != 0, "Owned screen normal extent exceeds native geometry framebuffer limits.");
				normal.owner = info.owner; normal.extent = info.extent; normal.format = VK_FORMAT_R16G16B16A16_SFLOAT;
				normal.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				Require(QualifiesImage(info.physicalDevice, normal), "The exact screen normal/roughness attachment format or usage is unsupported.");
			}
			auto candidate = std::make_shared<ScreenSpaceInputResources>();
			candidate->device = info.device; candidate->allocator = info.allocator; candidate->owner = info.owner;
			candidate->demand = info.demand; candidate->demand.sharedHiZ = useHiZ; candidate->depth = info.depth;
			candidate->borrowedFramesVerified = info.expectedFrameId != 0;
			if (info.demand.NeedsRadiance()) candidate->sceneRadiance = info.sceneRadiance;
			if (info.demand.NeedsNormals()) candidate->normalsRoughness = info.normalsRoughness;
			if (info.demand.reflections) candidate->environmentSpecular = info.environmentSpecular;
			if (info.demand.indirectLighting) candidate->indirectDiffuse = info.indirectDiffuse;
			if (useHiZ) candidate->hiZ = info.hiZ;
			if (createNormals)
			{
				candidate->normalsRoughness = normal;
				auto& image = *candidate->normalsRoughness;
				VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
				create.imageType = VK_IMAGE_TYPE_2D; create.format = image.format;
				create.extent = {info.extent.width, info.extent.height, 1}; create.mipLevels = create.arrayLayers = 1;
				create.samples = VK_SAMPLE_COUNT_1_BIT; create.tiling = VK_IMAGE_TILING_OPTIMAL;
				create.usage = image.usage; create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
				Check(vmaCreateImage(info.allocator, &create, &allocation, &image.image, &candidate->normalsAllocation, nullptr), "Screen normal/roughness image allocation failed.");
				vmaSetAllocationName(info.allocator, candidate->normalsAllocation, "Screen lighting view normals and roughness");
				VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
				view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				Check(vkCreateImageView(info.device, &view, nullptr, &image.view), "Screen normal/roughness image view creation failed.");
				candidate->requiresNormalsProducer = true;
			}
			result.resources = std::move(candidate);
			result.state = ScreenSpaceInputPreparationState::PreparedResources;
		}
		catch (const std::exception& error) { result.reason = error.what(); }
		return result;
	}
}
