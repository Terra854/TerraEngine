// VulkanFrameDiagnostics.h - Vulkan-local frame cost collection helpers.
//
// This layer converts backend events into renderer-level categories. It keeps
// Vulkan names out of the public graph contract while still allowing Vulkan to
// explain waits, rebuilds, recording, submit, present, and readback costs.

#pragma once

#include "Renderer/Graph/RenderFrameBreakdown.h"
#include "Core/Export.h"

namespace TerraEngine::Renderer::Vulkan {

	struct VulkanFrameDiagnosticScope
	{
		BackendFrameCostCategory category = BackendFrameCostCategory::RhiBeginFrame;
		RenderViewType viewType = RenderViewType::Game;
		bool shared = false;
	};

	TERRAENGINE_API BackendFrameBreakdown CreateVulkanFrameBreakdown(std::uint64_t frameId);
	TERRAENGINE_API void RecordVulkanFrameCost(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds);

}
