#include "../TestHarness.h"

#include "Scripting/ScriptingSystem.h"

#include <algorithm>

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

		registry.Add("Engine.ScriptingWorkerBoundary.AnimationCallbackResourceIsRegistered", [](TestContext& context) {
			const TerraEngine::Scheduling::SystemDeclaration declaration = ScriptingSystem::Instance().SchedulerDeclaration();
			const std::vector<TerraEngine::Scheduling::SchedulerResourceDefinition> resources =
				TerraEngine::Scheduling::DefaultSchedulerResources();

			// The installed editor validates the full declaration set before entering
			// normal runtime. Check every scripting access here so a misspelled or
			// removed resource fails a focused test instead of stopping editor startup.
			for(const std::string& access : declaration.reads) {
				const bool registered = std::any_of(resources.begin(), resources.end(), [&access](const auto& resource) {
					return resource.name == access;
				});
				context.Expect(registered, "Scripting read resource must be registered: " + access);
			}
			for(const std::string& access : declaration.writes) {
				const bool registered = std::any_of(resources.begin(), resources.end(), [&access](const auto& resource) {
					return resource.name == access;
				});
				context.Expect(registered, "Scripting write resource must be registered: " + access);
			}
			context.Expect(std::find(declaration.reads.begin(), declaration.reads.end(),
				TerraEngine::Scheduling::kResourceAnimationManagedCallbackBatches) != declaration.reads.end(),
				"Scripting must declare the copied animation callback publication it consumes.");
		});
	}
}
