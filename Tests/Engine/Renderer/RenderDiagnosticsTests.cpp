#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests {

	void RegisterRenderDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Diagnostics.DisabledClearsPublishedSummary", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 9u;
			summary.workerPacketCount = 3u;
			TerraEngine::Renderer::PublishRenderDiagnostics(summary);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
			context.ExpectEqual(static_cast<std::uint64_t>(0u), TerraEngine::Renderer::LatestRenderDiagnostics().frameId, "Closing diagnostics should clear render details.");
		});

		registry.Add("Renderer.Diagnostics.EnabledPublishesPacketCounts", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 10u;
			summary.workerPacketCount = 4u;
			summary.serialPacketCount = 1u;
			summary.serializedReasons.push_back("present");
			TerraEngine::Renderer::PublishRenderDiagnostics(summary);
			const auto latest = TerraEngine::Renderer::LatestRenderDiagnostics();
			context.ExpectEqual(static_cast<std::size_t>(4u), latest.workerPacketCount, "Worker packet counts should be visible when requested.");
			context.ExpectEqual(std::string("present"), latest.serializedReasons.front(), "Serialized reasons should be visible when requested.");
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
		});

		registry.Add("Renderer.Diagnostics.BuildSummaryIncludesRequestedBackendBreakdown", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 21u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(320u, 200u), 2u });
			TerraEngine::Renderer::RenderWorkPacketPlan packetPlan;
			TerraEngine::Renderer::RenderSubmissionPlan submission;
			submission.frameId = 21u;
			submission.orderedPasses.push_back({ "draw" });
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.supportsPacketLocalDiagnostics = true;
			profile.maxRecordingWorkers = 2u;
			profile.workerSchedulingThreshold = 1u;

			const auto summary = TerraEngine::Renderer::BuildRenderDiagnosticsSummary(snapshot, packetPlan, submission, profile);
			context.Expect(summary.backendFrameBreakdown.requested, "Requested diagnostics should include backend breakdown structure.");
			context.Expect(!summary.renderWorkerGroups.empty(), "Requested diagnostics should include render worker group summaries.");
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
		});

		registry.Add("Renderer.Diagnostics.ClosedBuildKeepsDetailedWorkDisabled", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 31u;
			TerraEngine::Renderer::RenderWorkPacketPlan packetPlan;
			TerraEngine::Renderer::RenderSubmissionPlan submission;
			submission.frameId = 31u;
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			const auto summary = TerraEngine::Renderer::BuildRenderDiagnosticsSummary(snapshot, packetPlan, submission, profile);
			context.Expect(!summary.detailed, "Closed diagnostics should avoid detailed collection.");
			context.Expect(summary.topPassCosts.empty(), "Closed diagnostics should not collect expensive pass detail.");
		});
	}

}
