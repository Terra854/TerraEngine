#include "../TestHarness.h"

#include "EditorAutomationService.h"

namespace TerraEngine::EditorTests {
	void RegisterEditorReflectionWorkerBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Editor.ReflectionWorkerBoundary.PreflightReportsProtectedMutationBoundary", [](TestContext& context) {
			TerraEngine::Editor::Automation::EditorAutomationService service;
			const auto result = service.PreflightReflectionMutation(nlohmann::json::object());
			const nlohmann::json diagnostics = service.BuildDiagnosticsSummary(true);

			context.Expect(!result.valid, "Empty preflight should fail validation without applying mutation.");
			context.ExpectEqual(std::string("protected"), result.classification, "Reflection mutation should remain protected.");
			context.Expect(result.safeSubwork.find("validation") != std::string::npos, "Preflight should name worker-safe validation.");
			context.Expect(result.protectedCommit.find("Reflected setters") != std::string::npos, "Preflight should name protected reflected setters.");
			context.Expect(diagnostics.dump().find("EditorReflectionMutation") != std::string::npos, "Diagnostics should include the editor reflection boundary finding.");
		});
	}
}
