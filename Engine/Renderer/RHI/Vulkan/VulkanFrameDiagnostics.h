// VulkanFrameDiagnostics.h - Vulkan-local frame cost collection helpers.
//
// This layer converts backend events into renderer-level categories. It keeps
// Vulkan names out of the public graph contract while still allowing Vulkan to
// explain waits, rebuilds, recording, submit, present, and readback costs.

#pragma once

#include "Renderer/Graph/RenderFrameBreakdown.h"
#include "Renderer/RendererRecovery.h"
#include "Profiling/PerformanceTelemetry.h"
#include "Core/Export.h"

#include <cstdint>
#include <optional>
#include <string>

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

	struct VulkanFirstFailureDiagnostics
	{
		// This payload is plain CPU data so recovery reporting can survive after
		// the Vulkan device is lost and backend handles are no longer safe.
		RendererDeviceFailureStage stage = RendererDeviceFailureStage::Unknown;
		RendererRecoveryAffectedView affectedView = RendererRecoveryAffectedView::None;
		std::string result;
		std::string operation;
		std::string affectedResourceId;
		std::string affectedResourceName;
		std::string hardwareName;
		std::string driverVersion;
		std::string apiVersion;
		std::uint64_t frameId = 0u;
		std::uint64_t renderSessionGeneration = 0u;
		std::uint32_t duplicateFailureCount = 0u;
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
	// Vulkan presentation waits are classified into the provider-neutral typed
	// profiler categories. This reports ownership without changing any Vulkan
	// fence, queue, acquire, or present synchronization behavior.
	TERRAENGINE_API std::optional<TerraEngine::Profiling::PresentationWaitKind> ClassifyVulkanPresentationWait(
		const VulkanFrameDiagnosticScope& scope);
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
	TERRAENGINE_API std::string FormatVulkanVersion(std::uint32_t version);
	TERRAENGINE_API VulkanFirstFailureDiagnostics BuildVulkanFirstFailureDiagnostics(
		const RendererRecoverySnapshot& snapshot,
		std::string operation,
		std::string hardwareName,
		std::string driverVersion,
		std::string apiVersion);
	TERRAENGINE_API void ApplyVulkanFirstFailureDiagnostics(
		RendererRecoverySnapshot& snapshot,
		const VulkanFirstFailureDiagnostics& diagnostics);

}
