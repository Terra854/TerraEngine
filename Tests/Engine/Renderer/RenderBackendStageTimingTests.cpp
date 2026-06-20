#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameBreakdown.h"

namespace TerraEngine::Tests {

	void RegisterRenderBackendStageTimingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.BackendStageTiming.AggregatesSubmitStages", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 42u;
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, RenderViewType::Game, 1.0f);
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, RenderViewType::Game, 2.0f);
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, RenderViewType::Game, 1.0f);
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::ImageAcquire, RenderViewType::Game, 0.5f);
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::CommandRecording, RenderViewType::Scene, 2.0f, 3u, "Scene commands", true);
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 1.5f);

			context.ExpectEqual(2.0f, TerraEngine::Renderer::SumBackendStageDuration(breakdown, TerraEngine::Renderer::BackendSubmitStage::CommandRecording), "Command recording stages should aggregate by stage.");
			context.ExpectEqual(100.0f, TerraEngine::Renderer::CalculateBackendStageAttributionPercent(breakdown), "Named backend stages should explain the broad submit cost.");
			context.ExpectEqual(static_cast<std::uint64_t>(42u), breakdown.stageTimings.front().frameId, "Stage timings inherit the backend frame id.");
		});
	}

}
