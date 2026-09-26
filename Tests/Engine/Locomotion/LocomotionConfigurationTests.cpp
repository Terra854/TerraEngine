// LocomotionConfigurationTests.cpp - authored values and atomic transaction coverage.

#include "TestHarness.h"

#include "Components/CharacterLocomotionComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include <limits>

namespace TerraEngine::Tests {
	void RegisterLocomotionConfigurationTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Configuration.DefaultsAndInvalidValues", [](TestContext& context) {
			const CharacterLocomotionComponent defaults{};
			context.Expect(defaults.ValidateAuthoredValues(), "Default locomotion authoring must be valid.");
			context.ExpectEqual(5.0, defaults.maximumSpeed, "Default maximum speed uses metres per second.");
			context.ExpectEqual(20.0, defaults.acceleration, "Default acceleration uses metres per second squared.");
			context.ExpectEqual(25.0, defaults.braking, "Default braking uses metres per second squared.");
			CharacterLocomotionComponent invalid = defaults;
			invalid.acceleration = 0.0;
			context.Expect(!invalid.ValidateAuthoredValues(), "Zero acceleration is invalid.");
			invalid = defaults;
			invalid.maximumSpeed = std::numeric_limits<double>::infinity();
			context.Expect(!invalid.ValidateAuthoredValues(), "Infinite speed is invalid.");
			invalid = defaults;
			invalid.speedParameter = "speed";
			invalid.localDirectionXParameter = "speed";
			context.Expect(!invalid.ValidateAuthoredValues(), "Duplicate animation mappings are invalid.");
		});

		registry.Add("Engine.Locomotion.Configuration.FinalCandidateAndMultiObjectRollback", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			Object second = fixture.objects.createObject("Second Locomotion Target");
			const UUID secondId = fixture.objects.getUUID(second);
			const std::string componentName(CharacterLocomotionComponent::kComponentName);
			Scene::TransactionalMutationBuilder valid("locomotion-final-candidate", Scene::TransactionOrigin::Test, "test");
			valid.AddComponent(fixture.rootId, componentName)
				.UpdateComponentProperty(fixture.rootId, componentName, "acceleration", Reflection::Variant{0.0})
				.UpdateComponentProperty(fixture.rootId, componentName, "acceleration", Reflection::Variant{30.0});
			const auto committed = Scene::CommitTransaction(fixture.objects, valid.Build());
			const auto* authored = fixture.objects.getRegistry().try_get<CharacterLocomotionComponent>(fixture.root.EntityId());
			context.Expect(committed.status == Scene::TransactionStatus::Committed && authored != nullptr
				&& authored->acceleration == 30.0,
				"Only the final complete candidate controls acceptance, including temporary invalid intermediate values.");

			Scene::TransactionalMutationBuilder invalid("locomotion-multi-object-invalid", Scene::TransactionOrigin::Test, "test");
			invalid.UpdateComponentProperty(fixture.rootId, componentName, "maximumSpeed", Reflection::Variant{7.0})
				.AddComponent(secondId, componentName)
				.UpdateComponentProperty(secondId, componentName, "braking", Reflection::Variant{0.0});
			const auto rejected = Scene::CommitTransaction(fixture.objects, invalid.Build());
			authored = fixture.objects.getRegistry().try_get<CharacterLocomotionComponent>(fixture.root.EntityId());
			context.Expect(rejected.status == Scene::TransactionStatus::Rejected && authored != nullptr
				&& authored->maximumSpeed == 5.0
				&& fixture.objects.getRegistry().try_get<CharacterLocomotionComponent>(second.EntityId()) == nullptr,
				"A bad candidate must leave all selected objects at their old authored values.");
		});

		registry.Add("Engine.Locomotion.Configuration.DeserializationRejectsWithoutChangingTarget", [](TestContext& context) {
			CharacterLocomotionComponent component{};
			context.Expect(CharacterLocomotionComponent::DeserializeAuthored(component,
				nlohmann::json{{"maximumSpeed", 8.0}})
				&& component.maximumSpeed == 8.0 && component.acceleration == 20.0
				&& component.initialMotionMode == CharacterLocomotionComponent::kVelocityMode,
				"Missing serialized fields must take documented defaults.");
			const auto original = CharacterLocomotionComponent::SerializeAuthored(component);
			auto invalid = original;
			invalid["maximumSpeed"] = -1.0;
			context.Expect(!CharacterLocomotionComponent::DeserializeAuthored(component, invalid)
				&& CharacterLocomotionComponent::SerializeAuthored(component) == original,
				"Invalid detached loading must not partially replace authored values.");
			invalid = original;
			invalid["speedParameter"] = "\xC0\xAF";
			context.Expect(!CharacterLocomotionComponent::DeserializeAuthored(component, invalid)
				&& CharacterLocomotionComponent::SerializeAuthored(component) == original,
				"Malformed UTF-8 animation names must reject without changing the target.");
		});
	}
}
