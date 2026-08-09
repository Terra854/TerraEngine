#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests {
	void RegisterRenderDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Diagnostics.DisabledClearsPublishedSummary", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 9u;
			TerraEngine::Renderer::PublishRenderDiagnostics(summary);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
			context.ExpectEqual(static_cast<std::uint64_t>(0u), TerraEngine::Renderer::LatestRenderDiagnostics().frameId,
				"Closing diagnostics should clear render details.");
		});
		registry.Add("Renderer.Diagnostics.EnabledPublishesPacketCounts", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 10u;
			summary.workerPacketCount = 4u;
			TerraEngine::Renderer::PublishRenderDiagnostics(summary);
			context.ExpectEqual(static_cast<std::size_t>(4u), TerraEngine::Renderer::LatestRenderDiagnostics().workerPacketCount,
				"Worker packet counts should be visible when requested.");
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
		});
	}
}
