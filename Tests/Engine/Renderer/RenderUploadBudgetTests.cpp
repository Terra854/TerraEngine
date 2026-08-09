#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"
#include "Renderer/Graph/RenderFramePublication.h"

namespace TerraEngine::Tests {

	void RegisterRenderUploadBudgetTests(TestRegistry& registry)
	{
		registry.Add("Renderer.UploadBudget.DelaysPayloadsOverBudget", [](TestContext& context) {
			TerraEngine::Renderer::RenderFramePublication publication;
			publication.frameId = 10u;
			publication.resourcePreparation.generation = 1u;
			for (std::size_t index = 0u; index < 4u; ++index)
			{
				TerraEngine::Renderer::RenderResourcePreparationRecord resource{};
				resource.resourceId = TerraEngine::GenerateUUID();
				resource.resourceGeneration = 1u;
				resource.preparationIndex = static_cast<std::uint32_t>(index);
				publication.resourcePreparation.records.push_back(resource);
			}
			const auto plan = TerraEngine::Renderer::Vulkan::BuildUploadPreparationPlan(publication, 2u);
			context.Expect(plan.overBudget, "Upload plan should report budget overflow.");
			context.ExpectEqual(static_cast<std::size_t>(2u), plan.payloads.size(), "Payloads over budget should be delayed.");
			context.ExpectEqual(static_cast<std::size_t>(2u), plan.delayedPayloads.size(), "Delayed payloads should be diagnosable.");
		});
	}

}
