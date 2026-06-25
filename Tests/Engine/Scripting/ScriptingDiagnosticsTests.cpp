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
			result.cacheHits = 5;
			result.cacheMisses = 1;
			result.bridgeCallsAvoided = 4;
			result.typedMutations = 2;
			result.jsonMutationFallbacks = 1;
			result.coalescedMutations = 3;
			result.readViewVersion = 33;
			result.diagnosticsCollected = true;
			result.accessReport.messages.push_back("sample");

			const std::string formatted = ScriptingSystem::FormatScriptingFrameResult(result);
			context.Expect(formatted.find("updated=3") != std::string::npos, "Formatted diagnostics should include updated script count.");
			context.Expect(formatted.find("readView=33") != std::string::npos, "Formatted diagnostics should include immutable read-view generation.");
			context.Expect(formatted.find("cacheHits=5") != std::string::npos, "Formatted diagnostics should include cache hit counts.");
			context.Expect(formatted.find("bridgeAvoided=4") != std::string::npos, "Formatted diagnostics should include avoided bridge-call counts.");
			context.Expect(formatted.find("typedMutations=2") != std::string::npos, "Formatted diagnostics should include typed mutation counts.");
			context.Expect(formatted.find("jsonFallbacks=1") != std::string::npos, "Formatted diagnostics should include JSON fallback counts.");
			context.Expect(formatted.find("coalescedMutations=3") != std::string::npos, "Formatted diagnostics should include coalesced mutation counts.");
			context.Expect(formatted.find("diagnostics=on") != std::string::npos, "Formatted diagnostics should report the collection state.");
		});

		registry.Add("Scripting.Diagnostics.ClassifiesProtectedSkippedAndFailedWork", [](TestContext& context) {
			ScriptingSystem::ScriptUpdateWorkload workload;
			workload.running = true;
			ScriptingSystem::ScriptWorkItem worker;
			worker.objectId = 1;
			worker.bindingId = "worker";
			worker.scriptType = "Game.Worker";
			worker.executionClass = ScriptingSystem::ScriptExecutionClass::WorkerSafe;
			ScriptingSystem::ScriptWorkItem protectedItem = worker;
			protectedItem.bindingId = "protected";
			protectedItem.executionClass = ScriptingSystem::ScriptExecutionClass::Protected;
			ScriptingSystem::ScriptWorkItem skippedItem = worker;
			skippedItem.bindingId = "skipped";
			skippedItem.executionClass = ScriptingSystem::ScriptExecutionClass::Skipped;
			workload.items = { worker, protectedItem, skippedItem };

			const std::vector<ScriptingSystem::ScriptBatch> batches = ScriptingSystem::PlanScriptBatches(workload, 2);
			bool sawWorker = false;
			bool sawProtected = false;
			bool sawSkipped = false;
			for (const ScriptingSystem::ScriptBatch& batch : batches)
			{
				sawWorker = sawWorker || batch.executionClass == ScriptingSystem::ScriptExecutionClass::WorkerSafe;
				sawProtected = sawProtected || batch.executionClass == ScriptingSystem::ScriptExecutionClass::Protected;
				sawSkipped = sawSkipped || batch.executionClass == ScriptingSystem::ScriptExecutionClass::Skipped;
			}

			context.Expect(sawWorker, "Worker-safe scripts should be attributed to worker batches.");
			context.Expect(sawProtected, "Protected scripts should keep a separate diagnostics classification.");
			context.Expect(sawSkipped, "Skipped scripts should keep a separate diagnostics classification.");
		});

		registry.Add("Scripting.Diagnostics.FormatsLifecycleState", [](TestContext& context) {
			context.ExpectEqual("Created", ScriptingSystem::FormatScriptLifecycleState(ScriptingSystem::ScriptLifecycleState::Created), "Lifecycle state should format correctly.");
			context.ExpectEqual("Started", ScriptingSystem::FormatScriptLifecycleState(ScriptingSystem::ScriptLifecycleState::Started), "Lifecycle state should format correctly.");
			context.ExpectEqual("Updating", ScriptingSystem::FormatScriptLifecycleState(ScriptingSystem::ScriptLifecycleState::Updating), "Lifecycle state should format correctly.");
			context.ExpectEqual("Disabled", ScriptingSystem::FormatScriptLifecycleState(ScriptingSystem::ScriptLifecycleState::Disabled), "Lifecycle state should format correctly.");
			context.ExpectEqual("Destroyed", ScriptingSystem::FormatScriptLifecycleState(ScriptingSystem::ScriptLifecycleState::Destroyed), "Lifecycle state should format correctly.");
		});

		registry.Add("Scripting.Diagnostics.ClassifiesInstanceStatus", [](TestContext& context) {
			ScriptingSystem::ScriptInstanceRecord record;
			record.authoredStateVersion = 2;
			record.runtimeObservedVersion = 2;
			record.lifecycleState = ScriptingSystem::ScriptLifecycleState::Updating;
			context.ExpectEqual("Ready", ScriptingSystem::ClassifyScriptInstanceStatus(record), "LifecycleState::Updating and clean should be classified as Ready.");

			record.lifecycleState = ScriptingSystem::ScriptLifecycleState::Disabled;
			context.ExpectEqual("Disabled", ScriptingSystem::ClassifyScriptInstanceStatus(record), "LifecycleState::Disabled should be classified as Disabled.");

			record.authoredStateVersion = 3;
			context.ExpectEqual("Stale (Update Pending)", ScriptingSystem::ClassifyScriptInstanceStatus(record), "Version mismatch should result in Stale (Update Pending) status.");
		});
	}
}
