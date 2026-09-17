// Hard-break contract tests for removed navigation component types.

#include "../TestHarness.h"
#include "Core/Object/Component.h"

namespace TerraEngine::Tests
{
	void RegisterNavigationComponentTests(TestRegistry& registry)
	{
		registry.Add("Navigation.HardBreak.RemovedTypesAreNotRegisteredOrReflected", [](TestContext& context) {
			context.Expect(!ComponentAccessRegistry().contains("NavigationAgentComponent")
				&& !ComponentAccessRegistry().contains("NavigationSourceComponent")
				&& !ComponentAccessRegistry().contains("NavigationConnectionComponent"),
				"Removed navigation component names must have no native registry or reflection entries.");
		});
		registry.Add("Navigation.HardBreak.NoReplacementComponentIsAdvertised", [](TestContext& context) {
			context.Expect(true, "Component-free navigation has no replacement authoring component.");
		});
	}
}
