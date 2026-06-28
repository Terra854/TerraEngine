#include "../TestHarness.h"

#include "Engine/Scene/RuntimeLayout.h"

namespace TerraEngine::EditorTests {
	void RegisterRuntimeLayoutDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Editor.RuntimeLayout.Diagnostics.SeparatesAuthoredAndRuntimeCounts", [](TestContext& context) {
			TerraEngine::Scene::LayoutDiagnosticsSummary summary;
			summary.totalObjectCount = 4;
			summary.activeObjectCount = 2;
			summary.transformRecordCount = 2;
			summary.renderRecordCount = 3;
			summary.metadataScanFallbackCount = 1;
			summary.staleCompactReferenceCount = 2;

			context.Expect(summary.totalObjectCount > summary.activeObjectCount, "Editor diagnostics should preserve authored inactive count separately from runtime active count.");
			context.Expect(TerraEngine::Scene::FormatLayoutDiagnosticsSummary(summary).find("total=4") != std::string::npos, "Formatted diagnostics should expose authored total count.");
			context.Expect(TerraEngine::Scene::FormatLayoutDiagnosticsSummary(summary).find("staleRefs=2") != std::string::npos, "Formatted diagnostics should expose stale compact reference count.");
		});
	}
}
