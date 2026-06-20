#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"

namespace TerraEngine::Tests {

	void RegisterRenderUploadBudgetTests(TestRegistry& registry)
	{
		registry.Add("Renderer.UploadBudget.DelaysPayloadsOverBudget", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(10u, 0u);
			for (std::size_t index = 0u; index < 4u; ++index)
			{
				snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "streaming", false, false });
			}
			const auto plan = TerraEngine::Renderer::Vulkan::BuildUploadPreparationPlan(snapshot, 2u);
			context.Expect(plan.overBudget, "Upload plan should report budget overflow.");
			context.ExpectEqual(static_cast<std::size_t>(2u), plan.payloads.size(), "Payloads over budget should be delayed.");
			context.ExpectEqual(static_cast<std::size_t>(2u), plan.delayedPayloads.size(), "Delayed payloads should be diagnosable.");
		});
	}

}
