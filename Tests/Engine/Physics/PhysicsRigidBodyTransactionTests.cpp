// Regression tests for atomic scene/physics publication when rigid-body work
// is invalid, stale, cancelled, or races a scene transform/version change.
#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsTransactions.h"
#include "Scene/TransactionalMutation.h"

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	void RegisterPhysicsRigidBodyTransactionTests(TestRegistry& registry)
	{
		registry.Add("Physics.RigidBodyTransaction.CancelledBodyCreationPublishesNothing", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			PhysicsTransaction transaction("rigidbody-invalid-collider");
			transaction.cancelled = true;
			transaction.physicsReadSet.worldGeneration = 1;
			transaction.physicsCommands.push_back(PhysicsCommand::MakeCreateBody(TerraEngine::GenerateUUID(), 1));
			PhysicsTransactionRuntimeState runtime;
			runtime.world = PhysicsWorldHandle{PhysicsWorldId{1U}, 1};
			const auto result = CommitPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.state == PhysicsTransactionState::Cancelled, "Cancelled rigid-body transaction must be rejected atomically.");
			context.Expect(result.providerWorldUnchanged && result.publishedWriteCount == 0U, "Rejected body creation must leave provider state unchanged.");
		});

		registry.Add("Physics.RigidBodyTransaction.StaleObjectAndBodyGenerationConflict", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Rigid Body Conflict");
			const auto objectId = objects.getUUID(object);
			PhysicsTransaction transaction("rigidbody-stale-target");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 2;
			transaction.physicsReadSet.bodyGenerations.emplace(objectId, 4);
			transaction.sceneWrites.push_back({.kind = TerraEngine::Scene::TransactionWriteKind::SetName,
				.target = TerraEngine::Scene::ExistingObject(objectId), .stringValue = "Must Not Publish"});
			transaction.physicsCommands.push_back(PhysicsCommand::MakeApplyForce(objectId, 2, 4));
			PhysicsTransactionRuntimeState runtime;
			runtime.world = PhysicsWorldHandle{PhysicsWorldId{1U}, 2};
			runtime.bodyGenerations.emplace(objectId, 5); // stale body generation
			const auto result = CommitPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.state == PhysicsTransactionState::Conflicted, "Stale rigid-body generation must conflict.");
			context.ExpectEqual(std::string("Rigid Body Conflict"), objects.getObjectName(object), "Conflicts must not publish scene transforms/name writes.");
			context.Expect(result.providerWorldUnchanged && !result.sceneGenerationPublished && !result.physicsGenerationPublished, "Conflicts must publish neither domain.");
			objects.clear();
		});

		registry.Add("Physics.RigidBodyTransaction.TransformVersionConflictRollsBackProviderStaging", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Physics Transform Version Conflict");
			Transform* transform = objects.AddComponent<Transform>(object);
			context.Expect(transform != nullptr, "The transaction fixture needs a Transform to observe.");
			objects.MarkObjectTransformDirty(object);
			const TerraEngine::UUID objectId = objects.getUUID(object);

			PhysicsTransaction transaction("rigidbody-transform-version-conflict");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 3U;
			transaction.physicsReadSet.bodyGenerations.emplace(objectId, 7U);
			transaction.sceneWrites.push_back({
				.kind = TerraEngine::Scene::TransactionWriteKind::UpdateComponentProperty,
				.target = TerraEngine::Scene::ExistingObject(objectId),
				.componentName = std::string(Transform::kComponentName),
				.propertyName = "Position",
				.propertyValue = TerraEngine::Reflection::Variant{glm::vec3(9.0F, 0.0F, 0.0F)}});
			transaction.physicsCommands.push_back(PhysicsCommand::MakeWake(objectId, 3U, 7U));

			// A later Transform write must advance the observed component generation
			// before the physics transaction reaches its protected commit boundary.
			transform->position = {2.0F, 0.0F, 0.0F};
			objects.notifyObjectStructureChanged(object);
			PhysicsTransactionRuntimeState runtime;
			runtime.world = PhysicsWorldHandle{PhysicsWorldId{1U}, 3U};
			runtime.bodyGenerations.emplace(objectId, 7U);
			const auto bodyGenerationsBefore = runtime.bodyGenerations;
			const auto result = CommitPhysicsTransaction(objects, runtime, transaction);

			context.Expect(result.state == PhysicsTransactionState::Conflicted,
				"A stale Transform observation must reject the mixed transaction before publication.");
			context.Expect(transform->position == glm::vec3(2.0F, 0.0F, 0.0F),
				"Rejected Transform publication must preserve the newer authored pose.");
			context.Expect(runtime.bodyGenerations == bodyGenerationsBefore && result.providerWorldUnchanged,
				"A scene version conflict must roll back or avoid every provider-side staged mutation.");
			context.Expect(!result.sceneGenerationPublished && !result.physicsGenerationPublished,
				"A Transform version conflict must expose no new scene or physics generation.");
			objects.clear();
		});
	}
}
