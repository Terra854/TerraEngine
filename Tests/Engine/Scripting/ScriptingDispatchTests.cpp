#include "../TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scripting/ScriptingSystem.h"
#include "ScriptingRuntimeTestFixtures.h"

namespace TerraEngine::Tests {
	void RegisterScriptingDispatchTests(TestRegistry& registry)
	{
		registry.Add("Scripting.Dispatch.ThreadLocalContextRoundTrips", [](TestContext& context) {
			ScriptingSystem::ClearCurrentScriptDispatchContext();
			context.Expect(!ScriptingSystem::CurrentScriptDispatchContext().has_value(), "Dispatch context should be empty before a script job begins.");

			ScriptingSystem::ScriptDispatchContext dispatchContext;
			dispatchContext.workerDispatch = true;
			dispatchContext.sourceObjectId = 42;
			dispatchContext.sourceBindingId = "binding";
			dispatchContext.frameOrder = 100;
			ScriptingSystem::SetCurrentScriptDispatchContext(dispatchContext);

			const std::optional<ScriptingSystem::ScriptDispatchContext> captured = ScriptingSystem::CurrentScriptDispatchContext();
			context.Expect(captured.has_value(), "Dispatch context should be visible to native glue while a callback runs.");
			context.ExpectEqual(static_cast<std::uint32_t>(42), captured->sourceObjectId, "Dispatch context should preserve source object id.");
			context.Expect(captured->workerDispatch, "Dispatch context should preserve worker eligibility.");

			ScriptingSystem::ClearCurrentScriptDispatchContext();
			context.Expect(!ScriptingSystem::CurrentScriptDispatchContext().has_value(), "Dispatch context should be cleared after a callback.");
		});

		registry.Add("Scripting.Dispatch.AuthoredStateVersionChangesOnlyForAuthoredEdits", [](TestContext& context) {
			ScriptBinding binding;
			const std::uint64_t initialVersion = binding.authoredStateVersion;
			ScriptFieldValue& value = binding.UpsertSerializedValue("Speed", "Float");
			value.value = 2.0f;
			context.Expect(binding.authoredStateVersion > initialVersion, "Adding an authored value should dirty the binding version.");

			const std::uint64_t editedVersion = binding.authoredStateVersion;
			context.Expect(binding.RemoveSerializedValue("Speed"), "Removing an authored value should succeed.");
			context.Expect(binding.authoredStateVersion > editedVersion, "Removing an authored value should dirty the binding version.");
		});

		registry.Add("Scripting.Dispatch.WorkItemsCarryCacheGenerations", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = CreateScriptedRuntimeObject("Generation Reader", "Game.GenerationReader");

			const ScriptingSystem::ScriptUpdateWorkload workload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);

			context.ExpectEqual(static_cast<std::size_t>(1), workload.items.size(), "The scripted object should produce one dispatch item.");
			context.ExpectEqual(ObjectManager::instance().getObjectMetadataGeneration(object), workload.items.front().objectGeneration, "Dispatch items should carry object metadata generation for Object.Name caching.");
			context.ExpectEqual(ObjectManager::instance().getObjectComponentGeneration(object), workload.items.front().componentGeneration, "Dispatch items should carry component generation for GetComponent caching.");
			context.ExpectEqual(ObjectManager::instance().getObjectBindingGeneration(object), workload.items.front().bindingGeneration, "Dispatch items should carry binding generation for HasComponent script checks.");
		});
	}
}
