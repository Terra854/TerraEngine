// Immutable recording-owned motion descriptors. GPU retirement belongs to caller.
#pragma once
#include "VulkanPostProcessMotionPreparation.h"
#include <volk.h>
#include <vk_mem_alloc.h>
#include <array>
#include <memory>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	// This exact layout must occupy graphics set2. Binding0 is read by both
	// vertex and fragment stages; bindings1..5 are vertex-only storage buffers.
	struct PostProcessMotionBufferLayout
	{
		VkDevice device=VK_NULL_HANDLE;
		VkDescriptorSetLayout layout=VK_NULL_HANDLE;
		~PostProcessMotionBufferLayout();
		PostProcessMotionBufferLayout()=default;
		PostProcessMotionBufferLayout(const PostProcessMotionBufferLayout&)=delete;
		PostProcessMotionBufferLayout& operator=(const PostProcessMotionBufferLayout&)=delete;
	};
	struct PostProcessMotionUploadBuffer
	{
		VkBuffer buffer=VK_NULL_HANDLE;
		VmaAllocation allocation=nullptr;
		VkDeviceSize size=0;
	};
	struct PostProcessMotionBuffers
	{
		VkDevice device=VK_NULL_HANDLE;
		VmaAllocator allocator=nullptr;
		std::shared_ptr<PostProcessMotionBufferLayout> layout;
		VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
		VkDescriptorSet descriptorSet=VK_NULL_HANDLE;
		std::array<PostProcessMotionUploadBuffer,6> buffers{};
		// Retains actual current/accepted previous roots and CPU byte owners.
		// This private copy contains the exact selector rows uploaded below.
		PreparedPostProcessMotion prepared;
		~PostProcessMotionBuffers();
		PostProcessMotionBuffers()=default;
		PostProcessMotionBuffers(const PostProcessMotionBuffers&)=delete;
		PostProcessMotionBuffers& operator=(const PostProcessMotionBuffers&)=delete;
	};
	[[nodiscard]] std::shared_ptr<PostProcessMotionBufferLayout> CreatePostProcessMotionBufferLayout(
		VkDevice device,std::string& reason) noexcept;
	// Parent first qualifies actual pipeline/index/mesh owners at its protected
	// boundary, setting only the view/draw qualification bits on a private DTO.
	// This factory never manufactures those bits or mutates immutable inputs.
	// All writes are mapped host-visible uploads with checked noncoherent flush;
	// there is no command submission, fence wait or frame-slot reuse here.
	// Caller retains the result AND matching geometry until actual GPU completion.
	[[nodiscard]] std::shared_ptr<PostProcessMotionBuffers> PreparePostProcessMotionBuffers(
		VkPhysicalDevice physicalDevice,VkDevice device,VmaAllocator allocator,
		std::shared_ptr<PostProcessMotionBufferLayout> layout,
		PreparedPostProcessMotion qualified,std::string& reason) noexcept;
}
