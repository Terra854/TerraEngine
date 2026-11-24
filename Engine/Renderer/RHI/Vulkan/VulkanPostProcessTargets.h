// VulkanPostProcessTargets.h - Resource ownership for one finished-image view.
#pragma once

#include "VulkanCommonIncludes.h"
#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	struct PostProcessPassResources;
	struct CustomPostProcessImageSet;
	// The full renderer lifetime prevents a newly opened view from reusing old
	// targets solely because its numeric view identifier was recycled.
	struct PostProcessTargetKey
	{
		std::uint32_t viewKind = 0;
		std::uint64_t viewId = 0;
		std::uint64_t rendererGeneration = 0;
		// Authored cameras may render into the same physical output. Their exact
		// copied camera identity and stable view lifetime own separate HDR images.
		std::array<std::uint8_t, 16> cameraId{};
		std::array<std::uint64_t, 4> cameraGenerations{};
		std::uint64_t viewLifetimeGeneration = 0;
		auto operator<=>(const PostProcessTargetKey&) const = default;
	};

	struct PostProcessTargetRequest
	{
		bool enabled = false;
		PostProcessTargetKey key;
		VkExtent2D internalExtent{};
		VkExtent2D outputExtent{};
		// Fixed exposure uses one destination. Longer chains can request both.
		std::uint32_t processingImageCount = 1u;
		// The recorder changes this token when compatible render-pass or pipeline
		// requirements change without changing the view's image dimensions.
		std::uint64_t passCompatibilityId = 0;
		// A settings transaction prepares all affected views before the
		// controller publishes any new effective values.
		bool prepareOnly = false;
	};

	// A copied device query makes format admission independently testable. The
	// native factory always queries the exact complete usage combination first.
	struct PostProcessFormatSupport
	{
		VkFormatFeatureFlags optimalFeatures = 0;
		VkResult imageQueryResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
		VkImageFormatProperties imageProperties{};
	};

	inline bool QualifiesPostProcessFormat(const PostProcessFormatSupport& support,
		VkExtent2D extent, VkImageUsageFlags usage) noexcept
	{
		VkFormatFeatureFlags required = 0;
		if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
			required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		if (usage & VK_IMAGE_USAGE_STORAGE_BIT) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
		if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
		if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
		if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		return extent.width != 0 && extent.height != 0
			&& (support.optimalFeatures & required) == required
			&& support.imageQueryResult == VK_SUCCESS
			&& extent.width <= support.imageProperties.maxExtent.width
			&& extent.height <= support.imageProperties.maxExtent.height
			&& support.imageProperties.maxExtent.depth >= 1
			&& support.imageProperties.maxMipLevels >= 1
			&& support.imageProperties.maxArrayLayers >= 1
			&& (support.imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
	}

	struct PostProcessTargetImage
	{
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
		VkExtent2D extent{};
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags usage = 0;
		// The recorder owns transitions. Allocation submits no initialization work.
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	struct PostProcessTargetUse
	{
		VkCommandBuffer command = VK_NULL_HANDLE;
		std::uint32_t frameSlot = 0;
		bool submittedUnfenced = false;
	};

	struct PostProcessTargetBundle
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		PostProcessTargetKey key;
		std::uint64_t generation = 0;
		VkExtent2D internalExtent{};
		VkExtent2D outputExtent{};
		std::uint64_t passCompatibilityId = 0;
		// Offscreen attachment views borrowed by framebuffer owners stay live
		// until the bundle's last recording, readback and presentation settle.
		VkImage borrowedOffscreenImage = VK_NULL_HANDLE;
		PostProcessTargetImage sceneColor;
		std::array<PostProcessTargetImage, 2> processing;
		// When a custom pass borrows the native color images, their ownership
		// moves to one independent shared set. No borrower retains the bundle.
		std::shared_ptr<const CustomPostProcessImageSet> customImages;
		std::uint32_t processingImageCount = 0u;
		VkSampler sampler = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		// Compatible pass owners retain framebuffers and pipelines that borrow the
		// target views. Their destruction precedes image and descriptor teardown.
		std::shared_ptr<PostProcessPassResources> passes;
		// Recorded and accepted unfenced scene work is retained independently of
		// fences that may still be signaled from the previous frame-slot use.
		std::vector<PostProcessTargetUse> pendingUses;
		// Record every successful borrowing submission. Completion is observed
		// before each frame fence reset, so fence-handle reuse never grants proof.
		std::array<std::uint64_t, 3> lastUseByFrame{};
		bool editorDescriptorAcknowledged = true;
		bool resourcesReady = false;

		PostProcessTargetBundle() = default;
		~PostProcessTargetBundle();
		bool RetainCustomImageSet(std::string& reason);
		PostProcessTargetBundle(const PostProcessTargetBundle&) = delete;
		PostProcessTargetBundle& operator=(const PostProcessTargetBundle&) = delete;
	};

	inline bool RecordPostProcessTargetUse(PostProcessTargetBundle& target,
		VkCommandBuffer command, std::uint32_t frameSlot) noexcept
	{
		if (!command || frameSlot >= target.lastUseByFrame.size()) return false;
		for (const auto& use : target.pendingUses)
			if (use.command == command && use.frameSlot == frameSlot) return true;
		try {
			target.pendingUses.push_back({ command, frameSlot, false });
			return true;
		} catch (...) {
			// Failure precedes native submission. The recorder must reject this
			// command admission instead of exposing a GPU borrower with no owner.
			return false;
		}
	}

	struct PostProcessFrameCompletion
	{
		std::uint64_t submitted = 0;
		std::uint64_t completed = 0;
		VkFence fence = VK_NULL_HANDLE;
	};

	inline void CompletePostProcessTargetSubmittedUses(PostProcessTargetBundle& target) noexcept
	{
		// Idle covers accepted commands. A recording can still be queued later.
		std::erase_if(target.pendingUses, [](const auto& use) { return use.submittedUnfenced; });
	}

	inline bool CanRetirePostProcessTarget(const PostProcessTargetBundle& target,
		const std::array<PostProcessFrameCompletion, 3>& completion, bool readbacksComplete) noexcept
	{
		if (!target.editorDescriptorAcknowledged || !readbacksComplete) return false;
		if (!target.pendingUses.empty()) return false;
		for (std::size_t slot = 0; slot < completion.size(); ++slot)
			if (target.lastUseByFrame[slot] > completion[slot].completed) return false;
		return true;
	}

	// A single owner-thread fault is consumed before the corresponding native
	// call. Tests can prove candidate rollback without forging GPU completion.
	enum class PostProcessTargetFailure : std::uint8_t
	{
		None, FormatQualification, ImageAllocation, ImageView, DescriptorAllocation
	};
}
