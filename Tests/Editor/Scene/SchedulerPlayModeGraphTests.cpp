#include "../TestHarness.h"

#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::EditorTests {
	void RegisterSchedulerPlayModeGraphTests(TestRegistry& registry)
	{
		registry.Add("Editor.SchedulerPlayModeGraph.WorkerScriptWorkCanSharePlayModeFrameSummary", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "editor-play-mode", "Editor Play Mode Scheduler Graph" });
			TerraEngine::Scheduling::EcsComponentQueryDeclaration editor;
			editor.queryName = "Editor.ProtectedTools";
			editor.access.ownerSystem = "Editor";
			editor.access.queryName = editor.queryName;
			editor.access.readResources = { "Editor" };
			editor.access.writeResources = { "Editor" };
			editor.access.workerEligible = false;
			editor.access.mainThreadReason = "Editor UI remains protected.";
			graph.AddQuery(std::move(editor));
			TerraEngine::Scheduling::EcsComponentQueryDeclaration scripts;
			scripts.queryName = "Scripting.ManagedCallbacks";
			scripts.access.ownerSystem = "ScriptingSystem";
			scripts.access.queryName = scripts.queryName;
			scripts.access.readComponents = { "ScriptComponent" };
			scripts.access.writeResources = { "Scripting" };
			scripts.access.workerEligible = true;
			graph.AddQuery(std::move(scripts));

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Editor Play Mode graph should accept editor and worker-safe script work.");
			context.Expect(summary.formatted.find("Scripting.ManagedCallbacks") != std::string::npos, "Summary should expose script scheduling work.");
		});

		registry.Add("Editor.SchedulerPlayModeGraph.DiagnosticsCloseDisablesScriptingCollection", [](TestContext& context) {
			ScriptingSystem::SetScriptingDiagnosticsRequested(true);
			TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);

			ScriptingSystem::SetScriptingDiagnosticsRequested(false);
			TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);

			context.Expect(!ScriptingSystem::ScriptingDiagnosticsRequested(), "Closing diagnostics should disable scripting collection.");
		});
	}
}
