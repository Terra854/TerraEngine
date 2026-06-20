#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests {

	void RegisterRenderBackendDiagnosticsOverheadTests(TestRegistry& registry)
	{
		registry.Add("Renderer.BackendDiagnosticsOverhead.ClosedWindowSkipsBackendDetail", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 51u;
			TerraEngine::Renderer::RenderWorkPacketPlan packetPlan;
			TerraEngine::Renderer::RenderSubmissionPlan submission;
			submission.frameId = 51u;
			TerraEngine::Renderer::BackendCapabilityProfile profile;

			const TerraEngine::Renderer::RenderDiagnosticsSummary summary = TerraEngine::Renderer::BuildRenderDiagnosticsSummary(snapshot, packetPlan, submission, profile);
			context.Expect(!summary.detailed, "Closed diagnostics should keep the detailed backend path disabled.");
			context.Expect(summary.backendStalls.empty(), "Closed diagnostics should not classify backend stalls.");
			context.Expect(summary.viewSubmitWork.empty(), "Closed diagnostics should not publish view submit comparisons.");
		});

		registry.Add("Renderer.BackendDiagnosticsOverhead.OpenWindowAppliesBackendBreakdown", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.detailed = true;
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 52u;
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, RenderViewType::Game, 1.0f);
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::FenceWait, RenderViewType::Game, 0.5f);
			TerraEngine::Renderer::AddViewSubmitWorkItem(breakdown, TerraEngine::Renderer::BuildViewSubmitWorkItem(52u, 1u, RenderViewType::Game, 1u, 1u, 0.1f, 0.2f, "game"));

			TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(summary, breakdown);
			context.ExpectEqual(static_cast<std::uint64_t>(52u), summary.backendFrameBreakdown.frameId, "Applied backend breakdown should be copied into diagnostics.");
			context.ExpectEqual(static_cast<std::size_t>(1u), summary.backendStalls.size(), "Open diagnostics should classify backend stalls.");
			context.ExpectEqual(static_cast<std::size_t>(1u), summary.viewSubmitWork.size(), "Open diagnostics should expose view submit work.");
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
		});
	}

}
