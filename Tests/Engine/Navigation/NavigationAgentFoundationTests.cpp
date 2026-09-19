// NavigationAgentFoundationTests.cpp - focused value-contract tests for agents.

#include "TestHarness.h"

#include "Components/NavigationAgentComponent.h"
#include "Core/Object/Component.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentTypes.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include <limits>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Navigation;

	void RegisterNavigationAgentFoundationTests(TestRegistry& registry) {
		registry.Add("Engine.Navigation.AgentFoundation.AuthoredDefaultsRemainProviderNeutralAndValid", [](TestContext& context) {
			const NavigationAgentComponent component{};
			const NavigationProfile profile = component.ToNavigationProfile();
			context.Expect(component.ValidateAuthoredValues() && profile.Validate().Succeeded(),
				"A new Navigation Agent must copy a complete valid ground profile without a provider identity.");
			context.ExpectEqual(0.10, component.arrivalToleranceMetres,
				"Navigation Agent defaults must retain the documented arrival tolerance.");
			context.ExpectEqual(0.25, component.targetDisplacementThresholdMetres,
				"Navigation Agent defaults must retain the documented target displacement threshold.");
			context.ExpectEqual(0.50, component.minimumReplanIntervalSeconds,
				"Navigation Agent defaults must retain the documented minimum replan interval.");

			NavigationAgentComponent invalid = component;
			invalid.arrivalToleranceMetres = 0.0;
			context.Expect(!invalid.ValidateAuthoredValues(),
				"Zero arrival tolerance must reject before it can become durable authored state.");
			invalid = component;
			invalid.heightMetres = invalid.radiusMetres * 2.0 - 0.01;
			context.Expect(!invalid.ValidateAuthoredValues(),
				"Profile cross-field rules must apply to copied Navigation Agent traversal values.");
		});

		registry.Add("Engine.Navigation.AgentFoundation.IdentitiesGuidanceAndBoundsFailClosed", [](TestContext& context) {
			context.Expect(kNavigationAgentMaximumCommandCount > 0U
				&& kNavigationAgentMaximumReceiptCount >= kNavigationAgentMaximumCommandCount
				&& kNavigationAgentMaximumRuntimeRecords > 0U,
				"Agent command, receipt, and runtime storage limits must remain finite positive bounds.");

			NavigationAgentGuidanceSnapshot guidance{};
			guidance.usable = true;
			guidance.state = NavigationAgentState::Following;
			context.Expect(!guidance.IsUsable(),
				"A guidance snapshot without generation-qualified identity and world evidence must fail closed.");
			guidance.identity.world = {NavigationWorldId{17U}, 3U};
			guidance.identity.object.lifetimeGeneration = 4U;
			guidance.identity.object.objectId[0] = 0xA5U;
			guidance.identity.attachmentGeneration = 5U;
			guidance.identity.runtimeSlot = 2U;
			guidance.identity.runtimeSlotGeneration = 6U;
			guidance.configurationRevision = 7U;
			guidance.intentSequence = 8U;
			guidance.publicationGeneration = 9U;
			guidance.routeGeneration = 10U;
			guidance.nextWaypoint.world = guidance.identity.world;
			guidance.nextWaypoint.position.offsetMetres = {1.0, 0.0, 2.0};
			guidance.observation = {1U, 1U, 1U, 1U, 1U, 1U};
			guidance.suggestedDirection = {1.0, 0.0, 0.0};
			guidance.remainingDistanceMetres = 3.0;
			context.Expect(guidance.IsUsable(),
				"Following guidance with current copied evidence may provide direction without owning movement.");
			guidance.state = NavigationAgentState::Arrived;
			context.Expect(!guidance.IsUsable(),
				"Arrival must publish zero-usable guidance even if an old direction remains diagnostic data.");
		});

		registry.Add("Engine.Navigation.AgentFoundation.ComponentRegistrationAndInvalidBatchStayAtomic", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			context.Expect(ComponentAccessRegistry().contains(std::string(NavigationAgentComponent::kComponentName)),
				"NavigationAgentComponent must use the normal reflected component registry.");

			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("agent-invalid-settings", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.AddComponent(fixture.rootId, std::string(NavigationAgentComponent::kComponentName))
				.UpdateComponentProperty(fixture.rootId, std::string(NavigationAgentComponent::kComponentName),
					"arrivalToleranceMetres", TerraEngine::Reflection::Variant{0.25})
				.UpdateComponentProperty(fixture.rootId, std::string(NavigationAgentComponent::kComponentName),
					"minimumReplanIntervalSeconds", TerraEngine::Reflection::Variant{0.01});
			const TerraEngine::Scene::TransactionResult rejected = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());
			context.Expect(rejected.status == TerraEngine::Scene::TransactionStatus::Rejected,
				"An invalid complete Navigation Agent candidate must reject during transaction preflight.");
			context.Expect(fixture.objects.getRegistry().try_get<NavigationAgentComponent>(fixture.root.EntityId()) == nullptr,
				"Rejected add-and-edit batches must leave no partially attached Navigation Agent component.");
		});

		registry.Add("Engine.Navigation.AgentFoundation.MultiObjectInvalidSettingsRejectWithoutPartialAttachments", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			Object second = fixture.objects.createObject("Second Agent Target");
			const TerraEngine::UUID secondId = fixture.objects.getUUID(second);
			TerraEngine::Scene::TransactionalMutationBuilder builder("agent-multi-object-invalid-settings", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.AddComponent(fixture.rootId, std::string(NavigationAgentComponent::kComponentName))
				.AddComponent(secondId, std::string(NavigationAgentComponent::kComponentName))
				.UpdateComponentProperty(secondId, std::string(NavigationAgentComponent::kComponentName),
					"heightMetres", TerraEngine::Reflection::Variant{0.2});
			const TerraEngine::Scene::TransactionResult rejected = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());
			context.Expect(rejected.status == TerraEngine::Scene::TransactionStatus::Rejected,
				"One invalid agent profile in a multi-object batch must reject the full authored edit.");
			context.Expect(fixture.objects.getRegistry().try_get<NavigationAgentComponent>(fixture.root.EntityId()) == nullptr
				&& fixture.objects.getRegistry().try_get<NavigationAgentComponent>(second.EntityId()) == nullptr,
				"A rejected multi-object batch must publish neither valid nor invalid Navigation Agent attachments.");
		});

		registry.Add("Engine.Navigation.AgentFoundation.EntitySerializationRoundTripsCompleteAuthoredState", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Serialized Navigation Agent");
			NavigationAgentComponent* agent = objects.AddComponent<NavigationAgentComponent>(object);
			if(agent == nullptr) { context.Expect(false, "The fixture must attach a NavigationAgentComponent."); objects.clear(); return; }
			agent->radiusMetres = 0.65;
			agent->arrivalToleranceMetres = 0.22;
			agent->areaPenalties[7U] = 3.5;
			const auto& access = ComponentAccessRegistry().at(std::string(NavigationAgentComponent::kComponentName));
			const nlohmann::json snapshot = access.serialize(object);
			context.Expect(snapshot.value("radiusMetres", 0.0) == 0.65
				&& snapshot.value("arrivalToleranceMetres", 0.0) == 0.22
				&& snapshot.contains("areaPenalties") && snapshot["areaPenalties"].size() == kNavigationAreaCount,
				"Component access serialization must retain every authored double and the complete penalty array.");
			context.Expect(!snapshot.contains("destination") && !snapshot.contains("request") && !snapshot.contains("guidance"),
				"Runtime destination, request, and guidance state must never enter authored component snapshots.");
			agent->radiusMetres = 0.4;
			agent->areaPenalties[7U] = 1.0;
			access.deserialize(object, snapshot);
			context.Expect(agent->radiusMetres == 0.65 && agent->areaPenalties[7U] == 3.5,
				"A valid authored snapshot must restore the complete agent configuration.");
			objects.clear();
		});

		registry.Add("Engine.Navigation.AgentFoundation.InvalidTransactionRestoresCompleteAuthoredCandidate", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			fixture.objects.AddComponent<NavigationAgentComponent>(fixture.root);
			NavigationAgentComponent* before = fixture.objects.getRegistry().try_get<NavigationAgentComponent>(fixture.root.EntityId());
			if(before == nullptr) { context.Expect(false, "The rollback fixture must attach an agent."); return; }
			before->radiusMetres = 0.55;
			before->areaPenalties[7U] = 3.5;
			const double baselineRadius = before->radiusMetres;
			auto changedPenalties = before->areaPenalties;
			changedPenalties[7U] = 7.0;
			TerraEngine::Scene::TransactionalMutationBuilder builder("agent-authored-rollback", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.UpdateComponentProperty(fixture.rootId, std::string(NavigationAgentComponent::kComponentName),
				"radiusMetres", TerraEngine::Reflection::Variant{0.8})
				.UpdateComponentProperty(fixture.rootId, std::string(NavigationAgentComponent::kComponentName),
					"areaPenalties", TerraEngine::Reflection::Variant{changedPenalties})
				.SetName(fixture.rootId, "Must Not Publish");
			// Inject after both valid edits so this exercises actual snapshot rollback,
			// not only detached preflight rejection before any authored writes occur.
			auto transaction = builder.Build();
			transaction.injectedFailureOperationIndex = 2U;
			const auto result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			const NavigationAgentComponent* after = fixture.objects.getRegistry().try_get<NavigationAgentComponent>(fixture.root.EntityId());
			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected && after != nullptr
				&& after->radiusMetres == baselineRadius && after->areaPenalties[7U] == 3.5,
				"An invalid staged agent edit must roll back earlier authored doubles as one complete candidate.");
		});
	}
}
