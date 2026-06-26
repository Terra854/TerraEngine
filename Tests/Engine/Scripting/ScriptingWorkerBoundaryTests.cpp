#include "../TestHarness.h"

#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	void RegisterScriptingWorkerBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.ScriptingWorkerBoundary.ManagedCallbacksArePartialWorkerSafe", [](TestContext& context) {
			const TerraEngine::Scheduling::SystemDeclaration declaration = ScriptingSystem::Instance().SchedulerDeclaration();

			context.Expect(declaration.hardBoundary, "Scripting should advertise a hard boundary.");
			context.Expect(declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Managed callbacks should be partial worker-safe.");
			context.Expect(declaration.mainThread, "Arbitrary managed invocation should remain protected.");
			context.Expect(declaration.safeSubwork.find("batch") != std::string::npos, "Safe subwork should describe native batch preparation.");
			context.Expect(declaration.protectedCommit.find("managed") != std::string::npos, "Protected commit should describe managed invocation.");
		});
	}
}
