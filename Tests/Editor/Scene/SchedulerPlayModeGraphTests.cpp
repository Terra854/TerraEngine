#include "../TestHarness.h"

#include "Core/Scheduling/EcsQueryTaskGraph.h"

namespace TerraEngine::EditorTests {
	void RegisterSchedulerPlayModeGraphTests(TestRegistry& registry)
	{
		registry.Add("Editor.SchedulerPlayModeGraph.ProtectedEditorAndScriptWorkCanShareFrameSummary", [](TestContext& context) {
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
			scripts.access.workerEligible = false;
			scripts.access.mainThreadReason = "C# callbacks stay behavior-compatible.";
			graph.AddQuery(std::move(scripts));

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Editor Play Mode graph should accept protected editor and script work.");
			context.Expect(summary.formatted.find("Scripting.ManagedCallbacks") != std::string::npos, "Summary should expose protected script boundary.");
		});
	}
}
