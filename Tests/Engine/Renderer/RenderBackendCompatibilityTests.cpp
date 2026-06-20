#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests {

	void RegisterRenderBackendCompatibilityTests(TestRegistry& registry)
	{
		registry.Add("Renderer.BackendCompatibility.ReportsQueueReadiness", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(12u, 4u);
			auto profile = MakeThreadedRendererProfile();
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto packets = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			const auto submission = TerraEngine::Renderer::BuildRenderSubmissionPlan(snapshot, passes, packets, snapshot.frameId);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			const auto summary = TerraEngine::Renderer::BuildRenderDiagnosticsSummary(snapshot, packets, submission, profile);
			context.Expect(!summary.queueReadiness.empty(), "Compatibility diagnostics should expose queue readiness.");
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
		});
	}

}
