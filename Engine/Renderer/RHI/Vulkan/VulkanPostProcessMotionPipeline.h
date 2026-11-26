// Owned graphics state for the depth-tested mesh motion pass.
#pragma once

#include <array>
#include <span>
#include <string>
#include "VulkanPostProcessMotionTargets.h"

namespace TerraEngine::Renderer::Vulkan
{
	struct MotionRasterPipelineRequest
	{
		std::shared_ptr<const MotionRasterTargets> targets;
		// Borrow the actual global, qualified source mesh, and immutable motion
		// layouts, in that order. Their owners must outlive this pipeline's use.
		std::array<VkDescriptorSetLayout, 3> setLayouts{};
		std::span<const std::uint32_t> fragmentWords;
		// Copy these values from the accepted opaque draw, including two-sided
		// and mirrored geometry. A guessed default can lose equal-depth coverage.
		VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		VkPipelineCache cache = VK_NULL_HANDLE;
		// Mesh replay keeps the task and push ABI of the color pipeline.
		std::span<const std::uint32_t> meshWords;
		std::span<const std::uint32_t> taskWords;
		std::span<const VkPushConstantRange> pushConstants;
		const char* meshEntry = "main";
		const char* taskEntry = "main";
	};

	struct MotionRasterPipeline
	{
		VkDevice device = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		std::shared_ptr<const MotionRasterTargets> targets;
		MotionRasterPipeline() = default;
		MotionRasterPipeline(const MotionRasterPipeline&) = delete;
		MotionRasterPipeline& operator=(const MotionRasterPipeline&) = delete;
		~MotionRasterPipeline();
	};

	// Prepare native state without submitting work. Retain the owner until
	// completion and replay the source coverage with the same viewport/scissor
	// and source layouts. The push ranges describe the chosen mesh variant.
	std::shared_ptr<MotionRasterPipeline> PrepareMotionRasterPipeline(VkDevice device,
		const MotionRasterPipelineRequest& request, std::string& reason);
}
