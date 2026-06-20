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
		// category is the broad renderer bucket used by older diagnostics.
		// stage is the newer submit-path label that explains exactly where a
		// Vulkan wait or recording cost happened.
		BackendFrameCostCategory category = BackendFrameCostCategory::RhiBeginFrame;
		BackendSubmitStage stage = BackendSubmitStage::Other;
		RenderViewType viewType = RenderViewType::Game;
		// viewId matches the scene/game output request so the editor can tell
		// which camera or viewport produced a backend cost.
		std::uint32_t viewId = 0u;
		std::string label;
		bool shared = false;
		// viewScoped is false for frame-wide work such as acquire/present and
		// true for work owned by a single scene or game view submission.
		bool viewScoped = false;
	};

	TERRAENGINE_API BackendFrameBreakdown CreateVulkanFrameBreakdown(std::uint64_t frameId);
	TERRAENGINE_API void RecordVulkanFrameCost(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds);
	TERRAENGINE_API void RecordVulkanFrameStage(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds);
	TERRAENGINE_API void RecordVulkanFrameStall(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds,
		float thresholdMilliseconds = 0.25f,
		std::string recommendedAction = {});
	// BuildVulkanViewSubmitWorkItem records enough identity to compare scene
	// and game view submissions without exposing Vulkan objects to editor code.
	TERRAENGINE_API ViewSubmitWorkItem BuildVulkanViewSubmitWorkItem(
		std::uint64_t frameId,
		const VulkanFrameDiagnosticScope& scope,
		std::uint64_t passSetHash,
		std::uint64_t overlayStateHash,
		float recordingMilliseconds,
		float submitMilliseconds,
		std::string sharedWorkKey = {});

}
