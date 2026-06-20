#include "../TestHarness.h"

#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	void RegisterScriptingDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Scripting.Diagnostics.ToggleControlsCollection", [](TestContext& context) {
			ScriptingSystem::SetScriptingDiagnosticsRequested(false);
			context.Expect(!ScriptingSystem::ScriptingDiagnosticsRequested(), "Diagnostics should start disabled for the cheap runtime path.");
			ScriptingSystem::SetScriptingDiagnosticsRequested(true);
			context.Expect(ScriptingSystem::ScriptingDiagnosticsRequested(), "Opening the diagnostics window should enable scripting diagnostics.");
			ScriptingSystem::SetScriptingDiagnosticsRequested(false);
		});

		registry.Add("Scripting.Diagnostics.FormatsFrameSummary", [](TestContext& context) {
			ScriptingSystem::ScriptingFrameResult result;
			result.frameId = 7;
			result.updatedScripts = 3;
			result.workerScripts = 2;
			result.protectedScripts = 1;
			result.acceptedMutations = 1;
			result.diagnosticsCollected = true;
			result.accessReport.messages.push_back("sample");

			const std::string formatted = ScriptingSystem::FormatScriptingFrameResult(result);
			context.Expect(formatted.find("updated=3") != std::string::npos, "Formatted diagnostics should include updated script count.");
			context.Expect(formatted.find("diagnostics=on") != std::string::npos, "Formatted diagnostics should report the collection state.");
		});
	}
}
