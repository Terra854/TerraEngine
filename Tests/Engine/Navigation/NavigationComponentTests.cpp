// Navigation component registration contracts.

#include "../TestHarness.h"
#include "Core/Object/Component.h"
#include "Components/NavigationAgentComponent.h"

#include <limits>

namespace TerraEngine::Tests
{
	void RegisterNavigationComponentTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Component.AgentIsRegisteredAndSourceTypesAreNot", [](TestContext& context) {
			context.Expect(ComponentAccessRegistry().contains("NavigationAgentComponent")
				&& !ComponentAccessRegistry().contains("NavigationSourceComponent")
				&& !ComponentAccessRegistry().contains("NavigationConnectionComponent"),
				"NavigationAgentComponent must be registered while source and connection authoring types remain unavailable.");
		});
		registry.Add("Navigation.Component.AgentIsAnOptionalAuthoringSurface", [](TestContext& context) {
			// Direct route requests remain valid without attaching an agent. This contract
			// keeps the component as a convenience for persistent per-object behavior.
			context.Expect(ComponentAccessRegistry().contains("NavigationAgentComponent"),
				"The optional NavigationAgentComponent must be discoverable through ordinary component reflection.");
		});
		registry.Add("Navigation.Component.DefaultsProduceValidCopiedProfile", [](TestContext& context) {
			NavigationAgentComponent component{};
			const auto profile = component.ToNavigationProfile();
			context.Expect(component.ValidateAuthoredValues() && profile.Validate().Succeeded(),
				"Ordinary agent defaults must pass the same finite traversal validation as direct navigation profiles.");
			context.Expect(profile.radiusMetres == component.radiusMetres
				&& profile.heightMetres == component.heightMetres
				&& profile.maxSlopeDegrees == component.maxSlopeDegrees,
				"Agent authored values must be copied into a provider-neutral navigation profile.");
		});
		registry.Add("Navigation.Component.InvalidAuthoredValuesRejectBeforePublication", [](TestContext& context) {
			NavigationAgentComponent component{};
			component.arrivalToleranceMetres = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!component.ValidateAuthoredValues(),
				"Non-finite arrival tolerance must be rejected before a staged component can publish.");
		});
	}
}
