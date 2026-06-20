#include "../TestHarness.h"

#include "Core/Object/Component.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	void RegisterScriptingComponentAccessTests(TestRegistry& registry)
	{
		registry.Add("Scripting.ComponentAccess.ManagedCallbacksAreWorkerScheduled", [](TestContext& context) {
			const ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			context.Expect(!scriptingSystem.ManagedCallbacksRunProtected(), "Managed callbacks should no longer be forced through the protected boundary.");
			context.Expect(!scriptingSystem.ManagedCallbacksRequireSchedulerAnnotations(), "C# scripts should not require author-side scheduler annotations.");
		});
	}
}
