// PhysicsTransactionTests.cpp - red tests for coordinated scene/physics commits.
//
// The tests intentionally target the future PhysicsTransactions contract. They
// ensure scene and provider changes are preflighted together, staged before the
// protected boundary, and published atomically with complete inverse rollback.

#include "../TestHarness.h"

#include "Physics/PhysicsCommands.h"
#include "Physics/PhysicsTransactions.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/TransactionalMutation.h"

namespace TerraEngine::Tests {
	void RegisterPhysicsTransactionTests(TestRegistry& registry)
	{
		registry.Add("Physics.Transaction.CoordinatesSceneAndPhysicsReadSets", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Physics Transaction Target");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			Physics::PhysicsTransaction transaction("physics-scene-read-set");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 4;
			transaction.physicsReadSet.bodyGenerations.emplace(objectId, 9);
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = Physics::PhysicsWorldHandle{Physics::PhysicsWorldId{1U}, 4};
			runtime.bodyGenerations.emplace(objectId, 9);
			TerraEngine::Scene::WorldPose targetPose = objects.GetCanonicalWorldPose(object).value();
			targetPose.position.offsetMetres = {2.0, 0.0, 0.0};
			transaction.sceneWrites.push_back(TerraEngine::Scene::TransactionWriteOperation{
				.kind = TerraEngine::Scene::TransactionWriteKind::SetWorldTransform,
				.target = TerraEngine::Scene::ExistingObject(objectId),
				.worldPose = targetPose});
			transaction.physicsCommands.push_back(Physics::PhysicsCommand::MakeWake(objectId, 4, 9));

			const auto bodyGenerationsBefore = runtime.bodyGenerations;
			const auto result = Physics::PreflightPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.accepted, "Matching scene and physics read sets should preflight together.");
			context.Expect(!result.mutatedScene && !result.mutatedPhysics, "Dual preflight must not mutate either domain.");
			context.Expect(runtime.bodyGenerations == bodyGenerationsBefore, "Preflight must leave the provider registry unchanged.");
			objects.clear();
		});

		registry.Add("Physics.Transaction.StagesProviderObjectsAndInverseRecordsBeforeCommit", [](TestContext& context) {
			Physics::PhysicsTransaction transaction("physics-stage-inverses");
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = Physics::PhysicsWorldHandle{Physics::PhysicsWorldId{1U}, 3};
			transaction.physicsReadSet.worldGeneration = 3;
			transaction.physicsCommands.push_back(Physics::PhysicsCommand::MakeCreateBody(TerraEngine::GenerateUUID(), 3));
			const auto staged = Physics::StagePhysicsTransaction(runtime, transaction);
			context.Expect(staged.accepted, "A valid transaction should stage provider work.");
			context.ExpectEqual(static_cast<std::size_t>(1), staged.providerObjects.size(), "Staging should produce one provider object record.");
			context.ExpectEqual(static_cast<std::size_t>(1), staged.inverseRecords.size(), "Every staged provider mutation needs an inverse record.");
			context.Expect(staged.state == Physics::PhysicsTransactionState::Prepared, "Staging must stop before protected commit.");
		});

		registry.Add("Physics.Transaction.SceneFailureRollsBackProviderAndPublishesNothing", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Rollback Parent");
			const Object child = objects.createObject("Rollback Child");
			context.Expect(objects.setParent(child, parent), "The rollback fixture should start with a valid hierarchy.");
			const TerraEngine::UUID parentId = objects.getUUID(parent);
			const TerraEngine::UUID childId = objects.getUUID(child);
			Physics::PhysicsTransaction transaction("physics-scene-failure");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {parentId, childId});
			transaction.physicsReadSet.worldGeneration = 5;
			transaction.physicsCommands.push_back(Physics::PhysicsCommand::MakeCreateBody(parentId, 5));
			transaction.sceneWrites.push_back(TerraEngine::Scene::TransactionWriteOperation{
				.kind = TerraEngine::Scene::TransactionWriteKind::ReparentObject,
				.target = TerraEngine::Scene::ExistingObject(parentId),
				.parent = TerraEngine::Scene::ExistingObject(childId)});
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = Physics::PhysicsWorldHandle{Physics::PhysicsWorldId{1U}, 5};
			const auto bodyGenerationsBefore = runtime.bodyGenerations;
			const std::uint64_t sceneGenerationBefore = runtime.scenePublicationGeneration;
			const std::uint64_t physicsGenerationBefore = runtime.physicsPublicationGeneration;

			const auto result = Physics::CommitPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.state == Physics::PhysicsTransactionState::RolledBack, "Scene commit failure must roll back provider staging.");
			context.Expect(result.providerWorldUnchanged, "Provider state must be restored by inverse records.");
			context.Expect(!result.sceneGenerationPublished && !result.physicsGenerationPublished, "Failed mixed transactions publish no generation.");
			context.Expect(runtime.bodyGenerations == bodyGenerationsBefore, "Provider rollback must restore the exact body registry.");
			context.Expect(runtime.scenePublicationGeneration == sceneGenerationBefore
				&& runtime.physicsPublicationGeneration == physicsGenerationBefore,
				"Rollback must not advance either published generation.");
			context.Expect(objects.getParent(child) == parent && !objects.getParent(parent), "Rejected cyclic hierarchy writes must leave the scene unchanged.");
			objects.clear();
		});

		registry.Add("Physics.Transaction.CancellationBeforePublicationIsAtomic", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Physics::PhysicsTransaction transaction("physics-cancel-before-publication");
			transaction.cancelled = true;
			transaction.physicsReadSet.worldGeneration = 8;
			transaction.physicsCommands.push_back(Physics::PhysicsCommand::MakeCreateBody(TerraEngine::GenerateUUID(), 8));
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = Physics::PhysicsWorldHandle{Physics::PhysicsWorldId{1U}, 8};
			const auto result = Physics::CommitPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.state == Physics::PhysicsTransactionState::Cancelled, "Cancelled transactions must not enter protected commit.");
			context.ExpectEqual(static_cast<std::size_t>(0), result.publishedWriteCount, "Cancellation must publish zero writes.");
			objects.clear();
		});

		registry.Add("Physics.Transaction.ConflictsLeaveZeroPartialVisibility", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Conflict Target");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			Physics::PhysicsTransaction transaction("physics-conflict");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 2;
			transaction.physicsReadSet.bodyGenerations.emplace(objectId, 1);
			transaction.physicsCommands.push_back(Physics::PhysicsCommand::MakeApplyForce(objectId, 2, 1));
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = Physics::PhysicsWorldHandle{Physics::PhysicsWorldId{1U}, 2};
			runtime.bodyGenerations.emplace(objectId, 1);
			const auto bodyGenerationsBefore = runtime.bodyGenerations;
			objects.setObjectName(object, "Changed Outside Transaction");
			const auto result = Physics::CommitPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.state == Physics::PhysicsTransactionState::Conflicted, "A stale scene read set must conflict atomically.");
			context.ExpectEqual(std::string("Changed Outside Transaction"), objects.getObjectName(object), "Conflicting commits must not expose partial scene writes.");
			context.Expect(!result.sceneGenerationPublished && !result.physicsGenerationPublished, "Conflicts must publish neither generation.");
			context.Expect(runtime.bodyGenerations == bodyGenerationsBefore, "Scene conflicts must not apply physics commands.");
			objects.clear();
		});

		registry.Add("Physics.Transaction.StalePhysicsReadSetLeavesSceneUnchanged", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Physics Generation Conflict");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			Physics::PhysicsTransaction transaction("physics-generation-conflict");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 6;
			transaction.physicsReadSet.bodyGenerations.emplace(objectId, 2);
			transaction.sceneWrites.push_back(TerraEngine::Scene::TransactionWriteOperation{
				.kind = TerraEngine::Scene::TransactionWriteKind::SetName,
				.target = TerraEngine::Scene::ExistingObject(objectId),
				.stringValue = "Must Not Publish"});
			transaction.physicsCommands.push_back(Physics::PhysicsCommand::MakeWake(objectId, 6, 2));
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = Physics::PhysicsWorldHandle{Physics::PhysicsWorldId{1U}, 6};
			runtime.bodyGenerations.emplace(objectId, 3);

			const auto result = Physics::CommitPhysicsTransaction(objects, runtime, transaction);
			context.Expect(result.state == Physics::PhysicsTransactionState::Conflicted, "A stale physics target generation must conflict.");
			context.ExpectEqual(std::string("Physics Generation Conflict"), objects.getObjectName(object), "Physics conflicts must publish no scene writes.");
			context.Expect(!result.sceneGenerationPublished && !result.physicsGenerationPublished, "Dual conflict must publish neither domain generation.");
			objects.clear();
		});
	}
}
