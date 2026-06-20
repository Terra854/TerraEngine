#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameBreakdown.h"

namespace TerraEngine::Tests {

	void RegisterRenderFrameBreakdownTests(TestRegistry& registry)
	{
		registry.Add("Renderer.FrameBreakdown.SeparatesWaitAndRecording", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 1u;
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::FenceWait, RenderViewType::Game, 2.0f);
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::PrimaryCommandRecording, RenderViewType::Game, 3.0f);

			context.ExpectEqual(2.0f, TerraEngine::Renderer::SumBackendWaitCost(breakdown), "Wait cost should not be merged into command recording.");
			context.ExpectEqual(3.0f, TerraEngine::Renderer::SumBackendRecordingCost(breakdown), "Recording cost should stay separate from waits.");
		});

		registry.Add("Renderer.FrameBreakdown.ReportsUnavailableSections", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 42u;
			snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(640u, 480u), 1u });
			const TerraEngine::Renderer::BackendFrameBreakdown breakdown = TerraEngine::Renderer::BuildUnavailableBackendFrameBreakdown(snapshot);

			context.ExpectEqual(static_cast<std::uint64_t>(42u), breakdown.frameId, "Fallback breakdown should preserve the frame id.");
			context.Expect(!breakdown.samples.empty(), "Fallback breakdown should name unavailable sections.");
			context.Expect(breakdown.samples.front().unavailable, "Fallback sections should be explicitly unavailable.");
		});

		registry.Add("Renderer.FrameBreakdown.SeparatesVisibleAndBackgroundWork", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::VisibleFrameWork, RenderViewType::Game, 3.0f);
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::QueuedBackgroundWork, RenderViewType::Game, 2.0f);
			context.ExpectEqual(3.0f, TerraEngine::Renderer::SumVisibleBackendFrameCost(breakdown), "Visible work should exclude queued background work.");
			context.ExpectEqual(2.0f, TerraEngine::Renderer::SumQueuedBackendFrameCost(breakdown), "Queued background work should be measured separately.");
		});
	}

}
