#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Core/CheckedImGuiVulkanBridge.h"

namespace TerraEngine::EditorTests
{
	void RegisterCheckedImGuiVulkanBridgeTests(TestRegistry& registry)
	{
		registry.Add("Editor.CheckedImGuiVulkanBridge.AbortsAtFirstFailure", [](TestContext& context) {
			bool afterFailureReached = false;
			const auto result = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
				TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
				[&]() {
					TerraEngine::Editor::CheckedImGuiVulkanBridge::CheckVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY);
					afterFailureReached = true;
				});
			context.Expect(!result.success, "A backend failure must escape as a typed result.");
			context.Expect(!afterFailureReached, "No later backend stage may execute after failure.");
			context.ExpectEqual(static_cast<std::int32_t>(VK_ERROR_OUT_OF_DEVICE_MEMORY), result.nativeResult, "The native result must be retained.");
		});
	}
}
