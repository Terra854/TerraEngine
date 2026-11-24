// Standalone custom color images and real adoption of existing native images.
#pragma once
#include "VulkanPostProcessCustom.h"
#include "VulkanPostProcessTargets.h"
#include <span>

namespace TerraEngine::Renderer::Vulkan
{
	struct CustomPostProcessImageAllocation;
	struct CustomPostProcessImageSet;
	enum class CustomCompositeDescriptorLayoutKind : std::uint8_t { SampledColor,SampledColorAndStorage };
	struct CustomPostProcessImageCreateInfo
	{
		VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=nullptr;
		VkExtent2D extent{};
		VkFormat format=VK_FORMAT_R16G16B16A16_SFLOAT;
		// The actual renderer fullscreen layout: b0 combined sampled color and,
		// when declared, b1 storage image. Caller supplies its exact typed shape.
		VkDescriptorSetLayout compositeLayout=VK_NULL_HANDLE;
		VkSampler sampler=VK_NULL_HANDLE;
		CustomCompositeDescriptorLayoutKind layoutKind=CustomCompositeDescriptorLayoutKind::SampledColorAndStorage;
		VkFilter samplingFilter=VK_FILTER_LINEAR;
		// A genuine independent owner retaining supplied sampler/layout and the
		// device/allocator domain. Never use a no-op shared_ptr or a target which
		// itself contains the resulting chain (that creates an ownership cycle).
		std::shared_ptr<const void> infrastructureOwner;
	};
	// No queue submission, clear, layout transition or producer proof occurs.
	// Descriptor/image destruction waits for actual GPU retirement by caller.
	[[nodiscard]] std::shared_ptr<const CustomPostProcessImageAllocation> PrepareCustomPostProcessImage(
		const CustomPostProcessImageCreateInfo& info,std::string& reason) noexcept;
	// Returned handles are borrowed; retain allocation and never delete them.
	[[nodiscard]] PostProcessTargetImage DescribeCustomPostProcessImage(
		const std::shared_ptr<const CustomPostProcessImageAllocation>& allocation) noexcept;
	// Real ownership transfer of up to three preexisting VMA image/view pairs.
	// Failure transfers NOTHING and destroys NOTHING. Success owns images/views
	// only; descriptors/pools/samplers remain the actual outer target's property.
	// Caller must then suppress every original image/view deletion path, including
	// failed target setup, while keeping normal queue/readback retirement gates.
	// All adopted-set/binding references must retire before device/VMA teardown.
	[[nodiscard]] std::shared_ptr<const CustomPostProcessImageSet> AdoptCustomPostProcessImageSet(
		VkDevice device,VmaAllocator allocator,std::span<const PostProcessTargetImage> images,std::string& reason) noexcept;
	// Both binding helpers retain actual allocation owners, not fake stand-ins
	// for an outer target. They attach caller's explicit current/planned metadata;
	// neither produces pixels nor certifies that a command was submitted.
	[[nodiscard]] std::optional<CustomPostProcessImage> BindCustomPostProcessImage(
		std::shared_ptr<const CustomPostProcessImageAllocation> allocation,const PostProcessHistoryScope& scope,
		std::uint64_t frameId,std::uint64_t targetGeneration,VkImageLayout layout,std::string& reason) noexcept;
	[[nodiscard]] std::optional<CustomPostProcessImage> BindCustomPostProcessImageSetImage(
		std::shared_ptr<const CustomPostProcessImageSet> images,std::uint32_t index,const PostProcessHistoryScope& scope,
		std::uint64_t frameId,std::uint64_t targetGeneration,VkImageLayout layout,std::string& reason) noexcept;
}
