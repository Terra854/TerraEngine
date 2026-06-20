#include "VulkanFrameDiagnostics.h"

#include <utility>

namespace TerraEngine::Renderer::Vulkan {

	BackendFrameBreakdown CreateVulkanFrameBreakdown(std::uint64_t frameId)
	{
		BackendFrameBreakdown breakdown;
		breakdown.frameId = frameId;
		breakdown.requested = true;
		return breakdown;
	}

	void RecordVulkanFrameCost(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds)
	{
		AddBackendFrameCost(breakdown, scope.category, scope.viewType, milliseconds, scope.label, scope.shared);
	}

	void RecordVulkanFrameStage(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds)
	{
		AddBackendStageTiming(
			breakdown,
			scope.stage,
			scope.viewType,
			milliseconds,
			scope.viewId,
			scope.label,
			scope.viewScoped,
			"vulkan");
	}

	void RecordVulkanFrameStall(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds,
		float thresholdMilliseconds,
		std::string recommendedAction)
	{
		RecordVulkanFrameStage(breakdown, scope, milliseconds);
		const BackendStageTiming& timing = breakdown.stageTimings.back();
		if (milliseconds >= thresholdMilliseconds || scope.stage == BackendSubmitStage::ForcedIdle)
		{
			AddBackendStallDiagnostic(
				breakdown,
				BuildBackendStallDiagnostic(timing, thresholdMilliseconds, std::move(recommendedAction)));
		}
	}

	ViewSubmitWorkItem BuildVulkanViewSubmitWorkItem(
		std::uint64_t frameId,
		const VulkanFrameDiagnosticScope& scope,
		std::uint64_t passSetHash,
		std::uint64_t overlayStateHash,
		float recordingMilliseconds,
		float submitMilliseconds,
		std::string sharedWorkKey)
	{
		return BuildViewSubmitWorkItem(
			frameId,
			scope.viewId,
			scope.viewType,
			passSetHash,
			overlayStateHash,
			recordingMilliseconds,
			submitMilliseconds,
			std::move(sharedWorkKey));
	}

}
