#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RendererRecoveryEditorTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryAutomationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.RendererRecoverySnapshotHasStableFields", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(summary, MakeEditorRendererRecoverySnapshot());

			context.Expect(!summary.rendererRecovery.incidentId.empty(), "Automation should receive a stable recovery incident id.");
			context.Expect(summary.rendererRecoveryDiagnostics.safeActions.empty(), "Terminal device loss should not expose unreachable recovery actions to automation.");
			context.ExpectEqual(summary.rendererRecovery.incidentId, summary.rendererRecoveryDiagnostics.incident.incidentId, "Automation compact and expanded recovery ids should match.");
		});
	}
}
