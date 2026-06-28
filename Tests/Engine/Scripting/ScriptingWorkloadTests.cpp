#include "../TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	namespace {
		Object CreateScriptedObject(std::string_view className)
		{
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Scripted");
			ScriptComponent& component = *objects.AddComponent<ScriptComponent>(object.EntityId());
			component.AddBinding(TerraEngine::EMPTY_UUID, className);
			return object;
		}
	}

	void RegisterScriptingWorkloadTests(TestRegistry& registry)
	{
		registry.Add("Scripting.Workload.ExcludesDisabledAndUnresolvedBindings", [](TestContext& context) {
			ObjectManager::instance().clear();
			CreateScriptedObject("Game.Rotate");
			CreateScriptedObject("");

			Object disabled = CreateScriptedObject("Game.Disabled");
			ScriptComponent* disabledComponent = ObjectManager::instance().GetComponent<ScriptComponent>(disabled.EntityId());
			disabledComponent->GetBindings().front().enabled = false;

			ScriptingSystem::ScriptUpdateWorkload workload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			context.ExpectEqual(static_cast<std::size_t>(1), workload.items.size(), "Only enabled resolved editor bindings should enter the workload.");
			context.ExpectEqual(std::string("Game.Rotate"), workload.items.front().scriptType, "The workload should preserve the managed script type.");

			ObjectManager::instance().clear();
		});

		registry.Add("Scripting.Workload.PlanBatchesChunksWorkerScripts", [](TestContext& context) {
			ScriptingSystem::ScriptUpdateWorkload workload;
			workload.running = true;
			for (std::uint32_t index = 0; index < 40; ++index)
			{
				ScriptingSystem::ScriptWorkItem item;
				item.objectId = index + 1;
				item.bindingId = "binding-" + std::to_string(index);
				item.scriptType = "Game.Script";
				item.executionClass = ScriptingSystem::ScriptExecutionClass::WorkerSafe;
				workload.items.push_back(std::move(item));
			}

			const std::vector<ScriptingSystem::ScriptBatch> batches = ScriptingSystem::PlanScriptBatches(workload, 4);
			context.Expect(!batches.empty(), "Worker-safe scripts should produce at least one batch.");
			context.Expect(batches.size() < workload.items.size(), "Batch planning should keep job count lower than script count.");
			for (const ScriptingSystem::ScriptBatch& batch : batches)
			{
				context.ExpectEqual(static_cast<int>(ScriptingSystem::ScriptExecutionClass::WorkerSafe), static_cast<int>(batch.executionClass), "Worker workload should stay worker-safe.");
			}
		});

		registry.Add("Scripting.Workload.ReadViewCarriesDeterministicScriptRecords", [](TestContext& context) {
			ObjectManager::instance().clear();
			Object scripted = CreateScriptedObject("Game.Reader");
			ScriptComponent* scriptComponent = ObjectManager::instance().GetComponent<ScriptComponent>(scripted.EntityId());
			ScriptBinding& binding = scriptComponent->GetBindings().front();
			ScriptFieldValue field;
			field.fieldPath = "Speed";
			field.fieldName = "Speed";
			field.valueKind = "Float";
			field.value = 3.0f;
			binding.serializedValues.push_back(field);
			++binding.authoredStateVersion;
			scriptComponent->SynchronizeStorageFromBindings();

			ScriptingSystem::ScriptUpdateWorkload workload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			ScriptingSystem::ScriptFrameReadView readView = ScriptingSystem::Instance().BuildScriptFrameReadView(workload);
			Object objectHandle = ObjectManager::instance().makeObjectHandle(static_cast<entt::entity>(workload.items.front().objectId));

			context.ExpectEqual(static_cast<std::size_t>(1), readView.scriptRecords.size(), "The read view should mirror the planned scripting workload.");
			context.Expect(TerraEngine::IsResolvedObjectIdentity(ObjectManager::instance().validateObjectHandle(objectHandle)), "Scripting workload object ids should map to valid compact identities.");
			context.ExpectEqual(workload.readViewVersion, readView.readViewVersion, "The workload and read view should share the same immutable version.");
			context.ExpectEqual(binding.authoredStateVersion, readView.scriptRecords.front().authoredStateVersion, "Read-view records should carry authored binding versions for dirty hydration.");
			context.ExpectEqual(workload.items.front().bindingGeneration, readView.scriptRecords.front().bindingGeneration, "Read-view records should carry the binding generation used by worker scripts.");

			ObjectManager::instance().clear();
		});

		registry.Add("Scripting.Workload.ActiveBindingIndexRefreshesAuthoredVersions", [](TestContext& context) {
			ObjectManager::instance().clear();
			Object scripted = CreateScriptedObject("Game.CachedReader");
			ScriptComponent* scriptComponent = ObjectManager::instance().GetComponent<ScriptComponent>(scripted.EntityId());
			ScriptBinding& binding = scriptComponent->GetBindings().front();

			const ScriptingSystem::ScriptUpdateWorkload firstWorkload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			context.ExpectEqual(static_cast<std::size_t>(1), firstWorkload.items.size(), "The initial active binding index should include the scripted object.");
			const std::uint64_t initialVersion = firstWorkload.items.front().authoredStateVersion;

			ScriptFieldValue& field = binding.UpsertSerializedValue("Speed", "Float");
			field.value = 6.0f;
			scriptComponent->SynchronizeStorageFromBindings();
			ObjectManager::instance().notifyObjectStructureChanged(scripted);

			const ScriptingSystem::ScriptUpdateWorkload refreshedWorkload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			context.ExpectEqual(static_cast<std::size_t>(1), refreshedWorkload.items.size(), "Dirtying a script object should rebuild the cached binding index instead of dropping the script.");
			context.Expect(refreshedWorkload.items.front().authoredStateVersion > initialVersion, "The active binding index should refresh authored versions after script data changes.");

			ObjectManager::instance().clear();
		});

		registry.Add("Scripting.Workload.PlansHundredScriptsIntoWorkerBatches", [](TestContext& context) {
			ScriptingSystem::ScriptUpdateWorkload workload;
			workload.running = true;
			workload.readViewVersion = 42;
			for (std::uint32_t index = 0; index < 100; ++index)
			{
				ScriptingSystem::ScriptWorkItem item;
				item.objectId = index + 1;
				item.bindingId = "binding-" + std::to_string(index);
				item.scriptType = "Game.Stress";
				item.executionClass = ScriptingSystem::ScriptExecutionClass::WorkerSafe;
				item.readViewVersion = workload.readViewVersion;
				workload.items.push_back(std::move(item));
			}

			const std::vector<ScriptingSystem::ScriptBatch> batches = ScriptingSystem::PlanScriptBatches(workload, 8);
			std::size_t plannedItems = 0;
			for (const ScriptingSystem::ScriptBatch& batch : batches)
			{
				plannedItems += batch.items.size();
				context.ExpectEqual(static_cast<int>(ScriptingSystem::ScriptExecutionClass::WorkerSafe), static_cast<int>(batch.executionClass), "Stress script batches should remain worker-safe.");
			}

			context.ExpectEqual(workload.items.size(), plannedItems, "Batch planning should keep every stress script scheduled exactly once.");
			context.Expect(batches.size() < workload.items.size(), "Hundred-script workloads should be grouped into worker batches.");
		});

		registry.Add("Scripting.Workload.DiagnosticsFlagStaysClosedUntilRequested", [](TestContext& context) {
			ObjectManager::instance().clear();
			ScriptingSystem::SetScriptingDiagnosticsRequested(false);
			CreateScriptedObject("Game.NoDiagnostics");

			const ScriptingSystem::ScriptUpdateWorkload workload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);

			context.Expect(!workload.diagnosticsRequested, "Closed diagnostics windows should not enable detailed scripting collection.");
			ScriptingSystem::SetScriptingDiagnosticsRequested(true);
			const ScriptingSystem::ScriptUpdateWorkload diagnosticWorkload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			context.Expect(diagnosticWorkload.diagnosticsRequested, "Opening diagnostics should explicitly request detailed scripting collection.");
			ScriptingSystem::SetScriptingDiagnosticsRequested(false);
			ObjectManager::instance().clear();
		});
	}
}
