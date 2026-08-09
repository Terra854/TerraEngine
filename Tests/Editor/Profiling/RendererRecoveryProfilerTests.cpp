#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RendererRecoveryEditorTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryProfilerTests(TestRegistry& registry)
	{
		registry.Add("Editor.Profiler.RendererRecoveryDoesNotRequireGpuTiming", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(summary, MakeEditorRendererRecoverySnapshot());

			context.ExpectEqual(0.0f, summary.visibleFrameMilliseconds, "Recovery diagnostics should not invent GPU/viewport timing.");
			context.Expect(!summary.degradedWork.empty(), "Profiler should still have recovery state text while GPU timing is unavailable.");
		});

		registry.Add("Editor.Profiler.RendererRecoveryKeepsVisualEvidenceUnavailable", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			const auto recovery = MakeEditorRendererRecoverySnapshot();
			TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(summary, recovery);
			// Recovery diagnostics explain why rendering stopped; they must not turn
			// missing presents into fabricated non-black or GPU timing evidence.
			context.Expect(summary.visibleFrameMilliseconds == 0.0f
				&& TerraEngine::Renderer::IsRendererRecoveryActive(summary.rendererRecovery.state),
				"Profiler recovery rows must keep visual/GPU evidence unavailable until a real post-recovery present is captured.");
		});
	}
}
