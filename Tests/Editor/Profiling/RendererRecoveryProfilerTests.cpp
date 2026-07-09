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
	}
}
