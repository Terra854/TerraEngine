#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameBreakdown.h"

namespace TerraEngine::Tests {

	void RegisterRenderBackendStallDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Renderer.BackendStalls.ClassifiesWaitStages", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 7u;
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::FenceWait, RenderViewType::Game, 0.5f);

			const std::vector<TerraEngine::Renderer::BackendStallDiagnostic> stalls = TerraEngine::Renderer::CollectBackendStallDiagnostics(breakdown, 0.25f);
			context.ExpectEqual(static_cast<std::size_t>(1u), stalls.size(), "A fence wait over threshold should publish one stall diagnostic.");
			context.Expect(stalls.front().waitType == TerraEngine::Renderer::BackendStallType::Fence, "Fence wait stages should map to fence stalls.");
			context.Expect(stalls.front().severity == TerraEngine::Renderer::BackendStallSeverity::Warning, "A modest threshold breach should be a warning.");
		});

		registry.Add("Renderer.BackendStalls.AvoidsDuplicateExplicitStalls", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 8u;
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, RenderViewType::Game, 2.0f);
			TerraEngine::Renderer::AddBackendStallDiagnostic(breakdown, TerraEngine::Renderer::BuildBackendStallDiagnostic(breakdown.stageTimings.back(), 0.25f));

			const std::vector<TerraEngine::Renderer::BackendStallDiagnostic> stalls = TerraEngine::Renderer::CollectBackendStallDiagnostics(breakdown, 0.25f);
			context.ExpectEqual(static_cast<std::size_t>(1u), stalls.size(), "Explicit stalls should not be duplicated by threshold collection.");
			context.Expect(stalls.front().severity == TerraEngine::Renderer::BackendStallSeverity::Error, "Large forced idle waits should be escalated.");
		});
	}

}
