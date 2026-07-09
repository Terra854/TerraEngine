#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RHI/Vulkan/VulkanFrameDiagnostics.h"
#include "Renderer/RendererRecoveryTestFixtures.h"

namespace TerraEngine::Tests
{
	void RegisterRendererRecoveryDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("RendererRecovery.Diagnostics.AppliesRecoverySnapshot", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeLatchedRendererDeviceLoss();

			TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(summary, snapshot);

			context.ExpectEqual(snapshot.incidentId, summary.rendererRecovery.incidentId, "Render diagnostics should retain the compact recovery snapshot.");
			context.ExpectEqual(snapshot.incidentId, summary.rendererRecoveryDiagnostics.incident.incidentId, "Expanded diagnostics should retain the incident id.");
			context.Expect(!summary.degradedWork.empty(), "Active recovery should add a degraded-work explanation.");
			context.Expect(!summary.backendFallbacks.empty(), "Active recovery should add a first-failure backend explanation.");
		});

		registry.Add("RendererRecovery.Diagnostics.CarriesVulkanFirstFailureIdentity", [](TestContext& context) {
			TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeLatchedRendererDeviceLoss();
			TerraEngine::Renderer::Vulkan::ApplyVulkanFirstFailureDiagnostics(
				snapshot,
				TerraEngine::Renderer::Vulkan::BuildVulkanFirstFailureDiagnostics(
					snapshot,
					"test frame submit",
					"Test GPU",
					"12345",
					"1.3.290"));

			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(summary, snapshot);

			context.ExpectEqual(std::string("Test GPU"), summary.rendererRecoveryDiagnostics.incident.hardwareName, "Expanded recovery diagnostics should retain the Vulkan hardware name.");
			context.ExpectEqual(std::string("12345"), summary.rendererRecoveryDiagnostics.incident.driverVersion, "Expanded recovery diagnostics should retain the driver version.");
			context.Expect(summary.backendFallbacks.front().find("GPU=Test GPU") != std::string::npos, "Concise backend fallback should include the GPU name.");
		});

		registry.Add("RendererRecovery.Diagnostics.CapturesRecentBackendPhases", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 77u;
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::CommandRecording, RenderViewType::Game, 0.2f, 1u, "record game view", true);
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0.4f, 1u, "submit game view", true);

			TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(summary, breakdown);
			TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(summary, MakeLatchedRendererDeviceLoss());

			context.ExpectEqual(static_cast<std::size_t>(2u), summary.rendererRecoveryDiagnostics.recentRenderPhases.size(), "Recovery diagnostics should retain recent backend phases.");
			context.Expect(summary.rendererRecoveryDiagnostics.recentRenderPhases.back().find("submit game view") != std::string::npos, "Recent phase entries should retain the backend submit label.");
		});
	}
}
