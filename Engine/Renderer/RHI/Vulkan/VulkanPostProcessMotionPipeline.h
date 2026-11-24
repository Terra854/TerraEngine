// Owned graphics state for the depth-tested indexed motion pass.
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
		// Borrow the actual global, qualified indexed-mesh, and immutable motion
		// layouts, in that order. Their owners must outlive this pipeline's use.
		std::array<VkDescriptorSetLayout, 3> setLayouts{};
		std::span<const std::uint32_t> vertexWords;
		std::span<const std::uint32_t> fragmentWords;
		// Copy these values from the accepted opaque draw, including two-sided
		// and mirrored geometry. A guessed default can lose equal-depth coverage.
		VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		VkPipelineCache cache = VK_NULL_HANDLE;
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

	// Bounded ABI admission, not a substitute for the shader compiler's SPIR-V
	// validation. This pure check requires vertex pulling, no push constants,
	// and exactly the selected one- or two-color fragment interface.
	bool ValidateMotionRasterShaders(std::span<const std::uint32_t> vertexWords,
		std::span<const std::uint32_t> fragmentWords, bool cameraMotion, std::string& reason);

	// Protected native preparation only: no commands or submissions. Keep the
	// returned owner through every borrowing command's completion. Recording must
	// bind its three descriptor sets, positive viewport and scissor matching the
	// source depth, then issue the exact accepted indexed draws (vertexOffset=0,
	// instanceCount=1, firstInstance=motion row). Rebind the subsequent graphics
	// pipeline explicitly; Vulkan has no implicit restore operation. No push
	// range is exposed because these exact shaders have no push-constant block.
	std::shared_ptr<MotionRasterPipeline> PrepareMotionRasterPipeline(VkDevice device,
		const MotionRasterPipelineRequest& request, std::string& reason);
}
