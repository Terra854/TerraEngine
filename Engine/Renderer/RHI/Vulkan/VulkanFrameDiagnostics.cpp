#include "VulkanFrameDiagnostics.h"

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
		AddBackendFrameCost(breakdown, scope.category, scope.viewType, milliseconds, {}, scope.shared);
	}

}
