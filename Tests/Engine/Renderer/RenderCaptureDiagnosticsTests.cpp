#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"

#include <algorithm>

namespace TerraEngine::Tests {

	void RegisterRenderCaptureDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Renderer.CaptureDiagnostics.DeclaresSchedulerBoundaries", [](TestContext& context) {
			const TerraEngine::Renderer::RenderCaptureSchedulerDiagnostics diagnostics =
				TerraEngine::Renderer::BuildRenderCaptureSchedulerDiagnostics(
					"RenderSystem",
					"Render",
					"custom_camera",
					false,
					true);

			const auto hasRead = [&](const std::string& name) {
				return std::find(diagnostics.readResources.begin(), diagnostics.readResources.end(), name) != diagnostics.readResources.end();
			};
			const auto hasWrite = [&](const std::string& name) {
				return std::find(diagnostics.writeResources.begin(), diagnostics.writeResources.end(), name) != diagnostics.writeResources.end();
			};

			context.ExpectEqual(std::string("RenderSystem"), diagnostics.owningSystem, "Capture diagnostics should name the owning renderer.");
			context.ExpectEqual(std::string("Render"), diagnostics.schedulerPhase, "Capture diagnostics should expose the render scheduler phase.");
			context.Expect(hasRead("CaptureQueue"), "Capture diagnostics should declare queue reads.");
			context.Expect(hasRead("ResolvedCameraDescriptor"), "Capture diagnostics should declare camera descriptor reads.");
			context.Expect(hasWrite("CaptureResultQueue"), "Capture diagnostics should declare capture result writes.");
			context.Expect(diagnostics.protectedBoundary.find("LiveGpuSubmission") != std::string::npos, "Capture diagnostics should name the protected GPU boundary.");
			context.Expect(diagnostics.visibilityBoundary.find("visible") != std::string::npos, "Capture diagnostics should describe result visibility timing.");
			context.Expect(diagnostics.deterministicFallback, "Capture diagnostics should disclose deterministic fallback usage.");
		});
	}

}
