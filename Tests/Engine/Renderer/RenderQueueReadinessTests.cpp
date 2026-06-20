#include "TestHarness.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"

namespace TerraEngine::Tests {

	void RegisterRenderQueueReadinessTests(TestRegistry& registry)
	{
		registry.Add("Renderer.QueueReadiness.RequiresHandoffReason", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 14u;
			plan.orderedPasses.push_back({ "draw" });
			plan.queueReadiness.push_back({ TerraEngine::Renderer::RenderQueueCategory::Transfer, TerraEngine::Renderer::RenderQueueMode::Parallel, true, {} });
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 14u);
			context.Expect(!plan.valid, "Parallel queue handoff should require a diagnostic reason.");
		});
	}

}
