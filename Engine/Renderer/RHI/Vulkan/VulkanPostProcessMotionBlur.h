// VulkanPostProcessMotionBlur.h - real frame-bound HDR camera/object blur.
#pragma once
#include "VulkanPostProcessTargets.h"
#include "VulkanPostProcessTemporal.h"
#include <memory>
#include <optional>
#include <string>

namespace TerraEngine::Renderer::GraphicsSettings { struct MotionBlurControls; }
namespace TerraEngine::Renderer::Vulkan
{
	struct MotionRasterTargets;
	struct PostProcessMotionBuffers;
	struct MotionBlurPass;
	enum class MotionBlurColorGrid : std::uint32_t { Unknown, CurrentRaster, UnjitteredOutput };
	struct MotionBlurInputs
	{
		PostProcessTargetImage sceneColor, sceneDepth, output;
		std::shared_ptr<const MotionRasterTargets> motion;
		std::shared_ptr<const TemporalPass> previousDepthOwner;
		MotionBlurColorGrid colorGrid = MotionBlurColorGrid::Unknown;
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		std::uint64_t backendResourceGeneration = 0;
		// Anonymous editor views have a real view lifetime alongside their
		// reusable physical target key. History must match that lifetime too.
		std::uint64_t viewLifetimeGeneration = 0;
	};
	struct MotionBlurImageState
	{
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkAccessFlags access = 0;
	};
	struct MotionBlurFrameInput
	{
		// Exact uploaded current/accepted-previous publications and pose owners.
		std::shared_ptr<const PostProcessMotionBuffers> motionOwner;
		std::optional<TemporalAcceptedHistory> previous;
		std::uint64_t sceneFrameId = 0, depthFrameId = 0, motionFrameId = 0;
		const MotionRasterTargets* motionProducer = nullptr;
		MotionBlurImageState sceneColor, sceneDepth, totalMotion, cameraMotion, previousDepth, output;
		// Private conversion targets remain GENERAL once produced; actual
		// scheduled layout updates belong to the parent's primary transaction.
		VkImageLayout privateInputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};
	struct MotionBlurRecordedUse
	{
		std::uint64_t frameId = 0, previousFrameId = 0;
		std::shared_ptr<const PostProcessMotionBuffers> motionOwner;
		TemporalAcceptedHistory previous;
		VkImage outputImage = VK_NULL_HANDLE;
		MotionBlurImageState outputState;
		VkImageLayout privateInputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};
	enum class MotionBlurRecordResult { Rejected, SkippedDisabled, SkippedNoHistory, Recorded };
	struct MotionBlurStyle
	{
		float maximumRadiusPixels = 32.0f;
		float relativeDepthTolerance = 0.02f;
		float disocclusionAbsoluteMetres = 0.01f, disocclusionRelativeDepth = 0.01f;
	};

	// Allocates private converted velocities/linear metres/disocclusion and
	// immutable descriptors for each actual previous depth slot. No GPU work or
	// history publication occurs. Both camera AND total raw motion are required,
	// because residual object motion is total-camera even for object-only blur.
	// Initial qualification is native extent/sample1, standardZ D32/D16 only.
	// Parent retains this pass, borrowed HDR/depth targets, motion producer and
	// each recorded use until actual completion; abandoned primaries discard their
	// scheduled layout transaction without publishing any fresh output/history.
	std::shared_ptr<MotionBlurPass> PrepareMotionBlurPass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const MotionBlurInputs& inputs, std::string& reason);
	[[nodiscard]] MotionBlurRecordResult RecordMotionBlurPass(VkCommandBuffer command, MotionBlurPass& pass,
		const GraphicsSettings::MotionBlurControls& controls, const MotionBlurFrameInput& frame,
		MotionBlurRecordedUse& recorded, const MotionBlurStyle& style = {}) noexcept;
}
