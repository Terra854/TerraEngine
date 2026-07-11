// CheckedImGuiVulkanBridge.cpp - Checked Dear ImGui backend execution.

#include "CheckedImGuiVulkanBridge.h"

namespace TerraEngine::Editor
{
	void CheckedImGuiVulkanBridge::CheckVkResult(VkResult result)
	{
		if (result != 0)
		{
			throw BackendFailure(static_cast<std::int32_t>(result));
		}
	}

	ImGuiVulkanBridgeResult CheckedImGuiVulkanBridge::Execute(
		TerraEngine::Renderer::Vulkan::EditorUiFrameStage stage,
		const std::function<void()>& operation) noexcept
	{
		try
		{
			if (operation) operation();
			return { true, 0, stage };
		}
		catch (const BackendFailure& failure)
		{
			return { false, failure.Result(), stage };
		}
		catch (...)
		{
			// Non-Vulkan exceptions still abandon the CPU draw data safely. A
			// synthetic initialization error keeps the first-stage diagnostic typed.
			return { false, -3, stage };
		}
	}
}
