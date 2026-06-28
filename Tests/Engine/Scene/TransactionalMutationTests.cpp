#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"

#include <glm/vec3.hpp>

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.CommitsObjectMetadataAndComponentWritesAtomically", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("tx-atomic", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.ObserveScene(fixture.objects)
				.ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "RenamedRoot")
				.SetActive(fixture.rootId, false)
				.UpdateComponentProperty(fixture.rootId, std::string(Transform::kComponentName), "Position", TerraEngine::Reflection::Variant{ glm::vec3(2.0f, 3.0f, 4.0f) });

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "Valid transaction should commit.");
			context.ExpectEqual(std::string("RenamedRoot"), fixture.objects.getObjectName(fixture.root.EntityId()), "Metadata write should apply.");
			context.Expect(!fixture.objects.isObjectActive(fixture.root.EntityId()), "Active state should apply.");
			Transform* transform = fixture.objects.GetComponent<Transform>(fixture.root.EntityId());
			context.Expect(transform != nullptr, "Root should still have a Transform.");
			context.ExpectEqual(2.0f, transform->position.x, "Reflected component write should use ComponentAccessRegistry.");
		});

		registry.Add("Engine.TransactionalMutation.LocalCreatedObjectsCanBeReferencedLaterInTransaction", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("tx-local", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.CreateObject(1, "Parent")
				.CreateObject(2, "Child", TerraEngine::Scene::LocalObject(1));

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "Local object transaction should commit.");
			context.Expect(result.createdObjects.contains(1), "Parent local id should resolve.");
			context.Expect(result.createdObjects.contains(2), "Child local id should resolve.");
			const auto childEntity = fixture.objects.getEntity(result.createdObjects.at(2));
			context.Expect(childEntity.has_value(), "Child entity should exist.");
			context.Expect(fixture.objects.getParentEntity(*childEntity) != entt::null, "Child should be parented to transaction-local parent.");
		});
	}
}
