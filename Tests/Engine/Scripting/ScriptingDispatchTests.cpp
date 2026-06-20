#include "../TestHarness.h"

#include "Scripting/ScriptingSystem.h"

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
	}
}
