// Hard-break contract tests for removed authored navigation connections.

#include "../TestHarness.h"
#include "Core/Object/Component.h"

namespace TerraEngine::Tests
{
	void RegisterNavigationConnectionTests(TestRegistry& registry)
	{
		registry.Add("Navigation.HardBreak.AuthoredConnectionTypeIsRemoved", [](TestContext& context) {
			context.Expect(!ComponentAccessRegistry().contains("NavigationConnectionComponent"),
				"Authored navigation connections must not remain registered as a runtime component.");
		});
	}
}
