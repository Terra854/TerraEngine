#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderGraphThreadingContractTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ThreadingContract.BudgetsDetectOverflow", [](TestContext& context) {
			TerraEngine::Renderer::RenderWorkBudget budget{ TerraEngine::Renderer::RenderWorkBudgetCategory::DrawPreparation, 4u, 6u, TerraEngine::Renderer::RenderBudgetOverflowPolicy::Serialize, "draw" };
			context.Expect(TerraEngine::Renderer::IsRenderWorkOverBudget(budget), "Renderer work budgets should detect overflow before queues grow without limit.");
		});

		registry.Add("Renderer.ThreadingContract.PresentationMustBeCoordinatorOwned", [](TestContext& context) {
			auto pass = MakeRendererPass("present", 7u, TerraEngine::Renderer::RenderAccessMode::Present);
			pass.ownership = TerraEngine::Renderer::RenderWorkerOwnership::WorkerSafe;
			const auto result = TerraEngine::Renderer::ValidateRenderGraph({ pass });
			context.Expect(!result.valid, "Presentation resources must be rejected when worker-owned.");
		});
	}

}
