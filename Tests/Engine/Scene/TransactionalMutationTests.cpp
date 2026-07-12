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
			context.ExpectEqual(std::string("RenamedRoot"), fixture.objects.getObjectName(fixture.root), "Metadata write should apply.");
			context.Expect(!fixture.objects.isObjectActive(fixture.root), "Active state should apply.");
			Transform* transform = fixture.objects.GetComponent<Transform>(fixture.root);
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
			const auto childObject = fixture.objects.getObject(result.createdObjects.at(2));
			context.Expect(childObject.has_value(), "Child object should exist.");
			context.Expect(childObject && fixture.objects.getParent(*childObject).has_value(), "Child should be parented to transaction-local parent.");
		});

		registry.Add("Engine.TransactionalMutation.CapturesTypedInverseDeltasForCommittedWrites", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("tx-inverse", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "Inverse Target")
				.SetActive(fixture.rootId, false);

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "The inverse-delta fixture should commit.");
			context.ExpectEqual(static_cast<std::size_t>(2), result.inverseDeltas.size(), "Every applied scalar write should publish one typed inverse delta.");
			context.Expect(result.inverseDeltas[0].operation.kind == TerraEngine::Scene::TransactionWriteKind::SetName, "The first inverse should restore object name.");
			context.ExpectEqual(std::string("TransactionRoot"), result.inverseDeltas[0].operation.stringValue, "The name inverse should contain the exact prior value.");
			context.Expect(result.inverseDeltas[1].operation.kind == TerraEngine::Scene::TransactionWriteKind::SetActive, "The second inverse should restore active state.");
			context.Expect(result.inverseDeltas[1].operation.boolValue, "The active-state inverse should contain the exact prior value.");
		});

		registry.Add("Engine.TransactionalMutation.InvalidReflectedWriteRejectsBeforeAnyPublication", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("tx-invalid-property", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "Must Not Publish")
				.UpdateComponentProperty(
					fixture.rootId,
					std::string(Transform::kComponentName),
					"PropertyThatDoesNotExist",
					TerraEngine::Reflection::Variant{glm::vec3(1.0f)});

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected, "An invalid reflected write should reject during preflight.");
			context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root), "A rejected later write must leave earlier valid writes unapplied.");
			context.ExpectEqual(static_cast<std::size_t>(0), result.inverseDeltas.size(), "Preflight rejection should not publish rollback deltas for unapplied work.");
			context.Expect(!result.conflicts.empty(), "Rejection should include machine-readable conflict evidence.");
			if (!result.conflicts.empty())
			{
				context.ExpectEqual(static_cast<std::size_t>(1), result.conflicts.front().operationIndex, "Conflict diagnostics should identify the rejected operation.");
				context.ExpectEqual(std::string("PropertyThatDoesNotExist"), result.conflicts.front().propertyName, "Conflict diagnostics should identify the rejected property.");
			}
		});
	}
}
