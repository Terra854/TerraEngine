#include "../TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	namespace {
		Object CreateScriptedObject(std::string_view className)
		{
			Object object = ObjectManager::instance().createObject("Scripted");
			ScriptComponent& component = object.addComponent<ScriptComponent>();
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
			ScriptComponent* disabledComponent = disabled.getComponent<ScriptComponent>();
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
	}
}
