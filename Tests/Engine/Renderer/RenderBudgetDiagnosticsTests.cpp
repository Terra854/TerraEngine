#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests {

	void RegisterRenderBudgetDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Renderer.BudgetDiagnostics.DescribesOverflowPolicy", [](TestContext& context) {
			const auto diagnostic = TerraEngine::Renderer::BuildRenderBudgetDiagnostic({ TerraEngine::Renderer::RenderWorkBudgetCategory::Upload, 1u, 3u, TerraEngine::Renderer::RenderBudgetOverflowPolicy::Delay, "uploads" });
			context.Expect(diagnostic.overBudget, "Over-budget renderer work should be flagged.");
			context.ExpectEqual(std::string("uploads"), diagnostic.label, "Budget labels should be visible in diagnostics.");
		});
	}

}
