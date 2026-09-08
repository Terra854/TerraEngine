// Test-first contract tests for the durable ground-navigation components.
//
// The navigation component headers are intentionally included directly. Until
// Until the component implementation exists, this translation unit must fail to compile; a fallback
// test that merely reports "headers missing" would hide an incomplete feature.

#include "../TestHarness.h"

#include "Components/Navigation/NavigationAgentComponent.h"
#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Components/Navigation/NavigationSourceComponent.h"
#include "Core/Object/Component.h"
#include "Navigation/NavigationSettings.h"
#include "Scene/Serializer.h"

#include <array>
#include <string>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Tests
{
	using NavigationSource = ::NavigationSourceComponent;
	using NavigationAgent = ::NavigationAgentComponent;
	using NavigationConnection = ::NavigationConnectionComponent;

	void RegisterNavigationComponentTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Components.UseCRTPStableNamesAndReflection", [](TestContext& context) {
			context.Expect((std::is_base_of_v<Component<NavigationSource>, NavigationSource>),
				"NavigationSourceComponent must derive from Component<NavigationSourceComponent>.");
			context.Expect((std::is_base_of_v<Component<NavigationAgent>, NavigationAgent>),
				"NavigationAgentComponent must derive from Component<NavigationAgentComponent>.");
			context.Expect((std::is_base_of_v<Component<NavigationConnection>, NavigationConnection>),
				"NavigationConnectionComponent must derive from Component<NavigationConnectionComponent>.");
			context.ExpectEqual(std::string_view("NavigationSourceComponent"), NavigationSource::kComponentName,
				"Source component name is a stable authored contract.");
			context.ExpectEqual(std::string_view("NavigationAgentComponent"), NavigationAgent::kComponentName,
				"Agent component name is a stable authored contract.");
			context.ExpectEqual(std::string_view("NavigationConnectionComponent"), NavigationConnection::kComponentName,
				"Connection component name is a stable authored contract.");

			NavigationSource::registerType();
			NavigationAgent::registerType();
			NavigationConnection::registerType();
			context.Expect(ComponentAccessRegistry().contains("NavigationSourceComponent"),
				"Source component must self-register its reflection metadata.");
			context.Expect(ComponentAccessRegistry().contains("NavigationAgentComponent"),
				"Agent component must self-register its reflection metadata.");
			context.Expect(ComponentAccessRegistry().contains("NavigationConnectionComponent"),
				"Connection component must self-register its reflection metadata.");
		});

		registry.Add("Navigation.Components.CopyPreservesDurableSerializedState", [](TestContext& context) {
			NavigationSource source{};
			NavigationAgent agent{};
			NavigationConnection connection{};

			// These fields are durable authoring state. Runtime requests, routes,
			// timers, and provider references must not be introduced by copying.
			source.enabled = false;
			source.deduplicationGroup = 17U;
			agent.active = false;
			connection.enabled = false;

			const NavigationSource sourceCopy = source;
			const NavigationAgent agentCopy = agent;
			const NavigationConnection connectionCopy = connection;
			context.Expect(sourceCopy.enabled == source.enabled
				&& sourceCopy.deduplicationGroup == source.deduplicationGroup,
				"Source copy must preserve durable selection and enable state for serialization.");
			context.Expect(agentCopy.active == agent.active,
				"Agent copy must preserve durable authored state without runtime request state.");
			context.Expect(connectionCopy.enabled == connection.enabled,
				"Connection copy must preserve durable enable state for serialization.");
		});

		registry.Add("Navigation.Components.ValidateAuthoringAndRejectDuplicateSourceSelection", [](TestContext& context) {
			NavigationSource valid{};
			context.Expect(valid.Validate().accepted, "Default navigation source authoring must be valid.");

			NavigationSource invalid = valid;
			invalid.deduplicationGroup = 0U;
			invalid.enabled = true;
			context.Expect(!invalid.Validate().accepted,
				"An enabled source without a valid deduplication selection must be rejected.");

			valid.deduplicationGroup = 9U;
			NavigationSource duplicate = valid;
			const std::array sources{valid, duplicate};
			const auto duplicateResult = NavigationSource::ValidateSelection(sources);
			context.Expect(!duplicateResult.accepted,
				"Two physical source representations in one deduplication group must be rejected.");
			context.ExpectEqual(TerraEngine::Navigation::NavigationSourceValidationCode::DuplicateSelection,
				duplicateResult.code,
				"Duplicate source selection must have a stable diagnostic code.");

			NavigationAgent agent{};
			NavigationConnection connection{};
			context.Expect(agent.Validate().accepted, "Default navigation agent authoring must be valid.");
			context.Expect(connection.Validate().accepted, "Default ground connection authoring must be valid.");
		});

		registry.Add("Navigation.Components.RenderVisibilityDoesNotChangePhysicalEligibility", [](TestContext& context) {
			NavigationSource source{};
			source.enabled = true;
			source.renderVisible = true;
			const bool eligibleWhileVisible = source.IsPhysicallyEligible();

			// Rendering is a view concern. Hiding an object must not remove its
			// physically eligible ground geometry from navigation preparation.
			source.renderVisible = false;
			const bool eligibleWhileHidden = source.IsPhysicallyEligible();
			context.Expect(eligibleWhileVisible && eligibleWhileHidden,
				"Render visibility alone must not change physical navigation eligibility.");
		});

		registry.Add("Navigation.Components.DurableProfilesAndSettingsRoundTripThroughCheckedJson", [](TestContext& context) {
			using TerraEngine::Navigation::NavigationProfile;
			using TerraEngine::Navigation::NavigationSettings;

			NavigationProfile profile = NavigationProfile::Defaults();
			profile.profileId = 41U;
			profile.revision = 7U;
			profile.radiusMetres = 0.55;
			profile.areaPenalties[3] = 2.5;
			const nlohmann::json encodedProfile = Serializer::SerializeNavigationProfile(profile);
			const auto decodedProfile = Serializer::DeserializeNavigationProfile(encodedProfile);
			context.Expect(decodedProfile.has_value()
				&& decodedProfile->profileId == profile.profileId
				&& decodedProfile->revision == profile.revision
				&& decodedProfile->radiusMetres == profile.radiusMetres
				&& decodedProfile->areaPenalties[3] == profile.areaPenalties[3],
				"A valid navigation profile must preserve every durable semantic value through JSON.");

			NavigationSettings settings = NavigationSettings::Defaults();
			settings.maxActiveAgents = 128U;
			settings.maxPublishedAndRetiredBytes = 96U * TerraEngine::Navigation::kNavigationMiB;
			const nlohmann::json encodedSettings = Serializer::SerializeNavigationSettings(settings);
			const auto decodedSettings = Serializer::DeserializeNavigationSettings(encodedSettings);
			context.Expect(decodedSettings.has_value()
				&& decodedSettings->maxActiveAgents == settings.maxActiveAgents
				&& decodedSettings->maxPublishedAndRetiredBytes == settings.maxPublishedAndRetiredBytes,
				"A valid frozen navigation limit set must round-trip without becoming live provider state.");

			// Decode into a detached candidate. Rejection returns no candidate, so a
			// malformed disk value cannot partially update an existing profile or settings object.
			nlohmann::json invalidProfile = encodedProfile;
			invalidProfile["radiusMetres"] = -0.1;
			std::string error;
			context.Expect(!Serializer::DeserializeNavigationProfile(invalidProfile, &error).has_value()
				&& !error.empty(),
				"Invalid durable profile values must reject atomically with an explanation.");

			nlohmann::json invalidSettings = encodedSettings;
			invalidSettings["maxAdmittedRequests"] = 0U;
			error.clear();
			context.Expect(!Serializer::DeserializeNavigationSettings(invalidSettings, &error).has_value()
				&& !error.empty(),
				"Invalid durable capacity limits must reject before they can be admitted.");
		});
	}
}
