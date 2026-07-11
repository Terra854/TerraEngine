#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanPresentationLifecycle.h"

namespace TerraEngine::Tests
{
	void RegisterVulkanUiSubmissionFailureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanUiSubmissionFailure.EveryStageHasDiagnosticIdentity", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			for (int value = static_cast<int>(EditorUiFrameStage::WaitFrameFence);
				value <= static_cast<int>(EditorUiFrameStage::RepairFence); ++value) {
				const auto stage = static_cast<EditorUiFrameStage>(value);
				context.Expect(EditorUiFrameStageName(stage) != "Unknown", "Every fallible UI stage must retain its first-stage name.");
			}
		});
	}
}
