#include "TestHarness.h"

#include "Renderer/RHI/Vulkan/VulkanRecordingContext.h"

namespace TerraEngine::Tests {

	void RegisterVulkanThreadLocalContextTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanThreadLocalContext.ResetsOnlyAfterSafeFrame", [](TestContext& context) {
			auto recordingContext = TerraEngine::Renderer::Vulkan::CreateThreadLocalRecordingContext(2u, 9u);
			TerraEngine::Renderer::Vulkan::MarkThreadLocalRecordingContextSubmitted(recordingContext);

			context.Expect(!TerraEngine::Renderer::Vulkan::CanResetThreadLocalRecordingContext(recordingContext, 9u), "Submitted contexts need coordinator approval before reset.");
			recordingContext.safeToReset = true;
			context.Expect(TerraEngine::Renderer::Vulkan::CanResetThreadLocalRecordingContext(recordingContext, 9u), "Completed frame contexts can be reset safely.");
			TerraEngine::Renderer::Vulkan::ResetThreadLocalRecordingContext(recordingContext, 10u);
			context.ExpectEqual(static_cast<std::uint64_t>(10u), recordingContext.frameId, "Reset should advance context ownership to the next frame.");
		});
	}

}
