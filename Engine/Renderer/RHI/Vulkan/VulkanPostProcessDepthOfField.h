// VulkanPostProcessDepthOfField.h - real linear view-depth and HDR DOF ownership.
#pragma once
#include "VulkanPostProcessTargets.h"
#include <memory>
#include <string>

namespace TerraEngine::Renderer { struct RenderFramePublication; }
namespace TerraEngine::Renderer::GraphicsSettings { struct DepthOfFieldControls; }
namespace TerraEngine::Renderer::Vulkan
{
	struct DepthOfFieldPass;
	struct DepthOfFieldInputs
	{
		PostProcessTargetImage sceneColor, sceneDepth, output;
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		std::uint64_t backendResourceGeneration = 0;
		std::uint64_t viewLifetimeGeneration = 0;
	};
	struct DepthOfFieldImageState
	{
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkAccessFlags access = 0;
	};
	struct DepthOfFieldFrameInput
	{
		std::shared_ptr<const RenderFramePublication> publication;
		std::uint32_t presentationIndex = 0;
		// Parent source ownership proves these exact borrowed images came from
		// the consumed frame. Request admission/current camera state is not proof.
		std::uint64_t sceneFrameId = 0, depthFrameId = 0;
		DepthOfFieldImageState sceneColor, sceneDepth, output;
		VkImageLayout linearDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};
	struct DepthOfFieldRecordedUse
	{
		// Recording proof only; parent admits/retains this on actual queue submit.
		std::uint64_t frameId = 0, passGeneration = 0;
		std::uint32_t presentationIndex = 0;
		VkImage linearDepthImage = VK_NULL_HANDLE, outputImage = VK_NULL_HANDLE;
		DepthOfFieldImageState linearDepthState, outputState;
	};
	enum class DepthOfFieldRecordResult { Rejected, Skipped, Recorded };
	struct DepthOfFieldStyle { float relativeDepthTolerance = 0.02f; };

	// No commands/submission/waits. Only sampled single-sample standardZ D32/D16
	// at exactly the native HDR scene/output extent qualifies. Parent retirement
	// retains the pass and every borrowed target until real completion.
	std::shared_ptr<DepthOfFieldPass> PrepareDepthOfFieldPass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const DepthOfFieldInputs& inputs, std::string& reason);
	[[nodiscard]] DepthOfFieldRecordResult RecordDepthOfFieldPass(VkCommandBuffer command,
		DepthOfFieldPass& pass, const GraphicsSettings::DepthOfFieldControls& controls,
		const DepthOfFieldFrameInput& frame, DepthOfFieldRecordedUse& recorded,
		const DepthOfFieldStyle& style = {}) noexcept;

	// Allocated target metadata only, not a readiness/frame claim. Sharing it
	// requires the matching RecordedUse and retained pass, then actual upstream
	// production/submission proof. Disabled DOF creates no fresh linear-depth data.
	[[nodiscard]] PostProcessTargetImage ReadDepthOfFieldLinearDepthTarget(const DepthOfFieldPass& pass) noexcept;
}
