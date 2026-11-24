// VulkanPostProcessExposure.h - Per-view automatic exposure GPU ownership.
#pragma once
#include "VulkanCommonIncludes.h"
#include <memory>
#include <string>
namespace TerraEngine::Renderer::GraphicsSettings { struct PostProcessColorSettings; }
namespace TerraEngine::Renderer::Vulkan
{
	struct PostProcessTargetBundle;
	struct PostProcessTargetImage;
	struct AutoExposurePass;
	// Preparation runs at the protected native resource boundary and allocates
	// immutable bindings and initialized private history.
	// Retain the returned owner in the target's fence-retained pass resources;
	// destruction is legal only after every borrowing primary completes.
	std::shared_ptr<AutoExposurePass> PrepareAutoExposurePass(PostProcessTargetBundle&,
		const PostProcessTargetImage& source, const PostProcessTargetImage& destination, std::string& reason);
	// The scene or reconstruction source is shader-readable with compute visibility.
	// This records GPU history advancement only: discarded commands cannot advance
	// CPU state. The caller retains a reset request until actual submit acceptance.
	// The supplied destination is fully overwritten and ends shader-readable.
	void RecordAutoExposurePass(VkCommandBuffer, AutoExposurePass&,
		const GraphicsSettings::PostProcessColorSettings&, float deltaSeconds, bool reset);
}
