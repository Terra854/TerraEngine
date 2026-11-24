// Vulkan resources retained by one camera's qualified temporal pass.
#pragma once

#include "VulkanPostProcessMotionBuffers.h"
#include "VulkanPostProcessMotionPipeline.h"
#include "VulkanPostProcessTemporalFrameState.h"

namespace TerraEngine::Renderer::Vulkan
{
	struct TemporalViewResources
	{
		VkDevice device = VK_NULL_HANDLE;
		std::shared_ptr<MotionRasterTargets> motion;
		std::shared_ptr<TemporalPass> resolve;
		std::shared_ptr<PostProcessMotionBufferLayout> motionLayout;
		std::shared_ptr<TemporalViewFrameState> frameState;
		std::uint32_t resolveOutput = 0u;
		std::vector<std::uint32_t> motionVertexShader;
		std::vector<std::uint32_t> motionFragmentShader;
		struct MotionVariant
		{
			std::array<VkDescriptorSetLayout, 2> sourceLayouts{};
			VkCullModeFlags cullMode = 0u;
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			std::shared_ptr<MotionRasterPipeline> owner;
		};
		std::vector<MotionVariant> variants;
	};
	struct TemporalChainRecording
	{
		const TemporalViewFramePlan& frame;
		std::shared_ptr<const PostProcessMotionBuffers> motion;
		float sharpening = 0.0f;
	};

	// Borrow exact scene color/depth and the output consumed by the following
	// finished-image stage. Defer indexed surface layout-specific pipelines to
	// the protected geometry preflight, before the settings certificate.
	std::shared_ptr<TemporalViewResources> PrepareTemporalViewResources(
		VkPhysicalDevice physicalDevice, PostProcessTargetBundle& targets,
		const PostProcessTargetImage& sceneColor, const PostProcessTargetImage& sceneDepth,
		const PostProcessTargetImage& resolvedColor, bool cameraOnlyMotion, std::string& reason);

	const MotionRasterPipeline* PrepareTemporalMotionVariant(TemporalViewResources& view,
		std::array<VkDescriptorSetLayout, 2> sourceLayouts, VkCullModeFlags cullMode,
		VkFrontFace frontFace, VkPipelineCache cache, std::string& reason);

	glm::vec2 TemporalJitter(std::uint32_t phase, VkExtent2D extent) noexcept;
	glm::mat4 ApplyTemporalJitter(glm::mat4 viewProjection, glm::vec2 jitterUV) noexcept;
}
