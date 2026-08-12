// Regression tests for atomic scene/physics publication when rigid-body work
// is invalid, stale, cancelled, or races a scene transform/version change.
#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsTransactions.h"
#include "Physics/TransformPublicationPacket.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"

#include <array>
#include <cmath>
#include <vector>

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

		registry.Add("Physics.RigidBodyTransaction.ProviderFailureRejectsRegistrationAndScenePublication", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Unavailable Provider Target");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			PhysicsTransaction transaction("rigidbody-provider-unavailable");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 6U;
			transaction.sceneWrites.push_back({.kind = TerraEngine::Scene::TransactionWriteKind::SetName,
				.target = TerraEngine::Scene::ExistingObject(objectId), .stringValue = "Must Not Publish"});
			transaction.physicsCommands.push_back(PhysicsCommand::MakeCreateBody(objectId, 6U));
			PhysicsTransactionRuntimeState runtime;
			runtime.world = PhysicsWorldHandle{PhysicsWorldId{1U}, 6U};
			runtime.providerHealthy = false;
			const auto bodyGenerationsBefore = runtime.bodyGenerations;
			const auto result = CommitPhysicsTransaction(objects, runtime, transaction);

			// Provider health is checked before staging or runtime-map replacement so a
			// contained provider failure cannot leak a scene write or a new body identity.
			context.Expect(result.state == PhysicsTransactionState::Conflicted && !result.accepted,
				"An unavailable provider must reject the complete admission transaction before registration.");
			context.Expect(result.providerWorldUnchanged && runtime.bodyGenerations == bodyGenerationsBefore
				&& !result.sceneGenerationPublished && !result.physicsGenerationPublished,
				"Provider failure must retain the prior runtime registrations and both publication generations.");
			context.ExpectEqual(std::string("Unavailable Provider Target"), objects.getObjectName(object),
				"Provider failure must not expose a prepared scene write while admission is rejected.");
			objects.clear();
		});

		registry.Add("Physics.RigidBodyTransaction.DuplicateStagedIdentityRejectsWholeAdmission", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Duplicate Staging Target");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			PhysicsTransaction transaction("rigidbody-duplicate-staging");
			transaction.sceneReadSet = TerraEngine::Scene::CaptureTransactionReadSet(objects, {objectId});
			transaction.physicsReadSet.worldGeneration = 7U;
			transaction.sceneWrites.push_back({.kind = TerraEngine::Scene::TransactionWriteKind::SetName,
				.target = TerraEngine::Scene::ExistingObject(objectId), .stringValue = "Must Not Publish"});
			transaction.physicsCommands.push_back(PhysicsCommand::MakeCreateBody(objectId, 7U));
			transaction.physicsCommands.push_back(PhysicsCommand::MakeCreateBody(objectId, 7U));
			PhysicsTransactionRuntimeState runtime;
			runtime.world = PhysicsWorldHandle{PhysicsWorldId{1U}, 7U};
			const auto bodyGenerationsBefore = runtime.bodyGenerations;
			const auto result = CommitPhysicsTransaction(objects, runtime, transaction);

			// A duplicate target is detected in the temporary staging map. The first
			// command is therefore never allowed to become a partially registered body.
			context.Expect(result.state == PhysicsTransactionState::PreflightFailed && !result.accepted,
				"A duplicate provider identity in one admission batch must reject staging atomically.");
			context.Expect(result.providerWorldUnchanged && runtime.bodyGenerations == bodyGenerationsBefore
				&& !result.sceneGenerationPublished && !result.physicsGenerationPublished,
				"Duplicate staged work must retain prior runtime registration and publication state.");
			context.ExpectEqual(std::string("Duplicate Staging Target"), objects.getObjectName(object),
				"A rejected duplicate staging batch must leave its prepared scene write invisible.");
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

		registry.Add("Physics.RigidBodyTransaction.TickWidePublicationRejectsAtEveryDeclaredVisibilityBoundary", [](TestContext& context) {
			// AfterPhysics and FrameEnd are the only two public visibility boundaries.
			// Both must keep every preparation chunk invisible if one target goes stale.
			for(const Physics::TransformPublicationVisibilityBoundary boundary :
				std::array<Physics::TransformPublicationVisibilityBoundary, 2U>{
					Physics::TransformPublicationVisibilityBoundary::AfterPhysics,
					Physics::TransformPublicationVisibilityBoundary::FrameEnd}) {
				ComponentRegistry::RegisterAllComponents();
				ObjectManager& objects = ObjectManager::instance();
				objects.clear();
				TerraEngine::Scene::SceneMutationQueue queue;
				const Physics::PhysicsWorldHandle world{{401U}, 9U};
				std::vector<Object> targets{
					objects.createObject("Boundary A"), objects.createObject("Boundary B"), objects.createObject("Boundary C")};
				std::vector<Physics::TransformPublicationSourceRecord> records;
				records.reserve(targets.size());
				for(std::size_t index = 0U; index < targets.size(); ++index) {
					Transform* transform = objects.AddComponent<Transform>(targets[index]);
					context.Expect(transform != nullptr, "Each boundary fixture target needs a Transform component.");
					objects.MarkObjectTransformDirty(targets[index]);
					records.push_back({
						.objectId = objects.getUUID(targets[index]),
						.body = {world.worldId, world.worldGeneration, static_cast<Physics::PhysicsSlotIndex>(index), 1U},
						.objectLifetimeGeneration = objects.getObjectLifetimeGeneration(targets[index]),
						.bodyGeneration = 1U,
						.componentGeneration = objects.getObjectComponentGeneration(targets[index]),
						.expectedTransformRevision = objects.GetObjectTransformRevision(targets[index]),
						.expectedHierarchyGeneration = objects.GetHierarchyRevision(),
						.localPose = {{static_cast<float>(index + 1U), 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}},
						.worldPoseReceipt = {{static_cast<float>(index + 1U), 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}},
						.poseChanged = true});
				}

				const Physics::TransformPublicationPacket packet = Physics::PrepareTransformPublicationPacket(
					{world, 33U, 5U, 7U}, objects.GetSceneGeneration(), objects.GetHierarchyRevision(),
					static_cast<std::uint64_t>(boundary) + 1U, 77U, records, 1U, boundary);
				context.Expect(packet.targetBoundary == boundary && packet.chunks.size() == targets.size(),
					"The test must prepare a distinct one-entry chunk for every declared boundary.");
				queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(packet));
				objects.removeObject(targets.back());

				const auto flushed = queue.Apply(objects,
					boundary == Physics::TransformPublicationVisibilityBoundary::AfterPhysics ? "After Physics" : "Frame End");
				context.Expect(flushed.publicationGroupResults.size() == 1U && !flushed.publicationGroupResults.front().Committed(),
					"A missing late chunk must reject the entire packet at its declared flush boundary.");
				context.Expect(flushed.publicationGroupResults.front().appliedWriteCount == 0U,
					"Neither boundary may expose an early transform before the group-wide preflight succeeds.");
				for(std::size_t index = 0U; index + 1U < targets.size(); ++index) {
					const Transform* transform = objects.GetComponent<Transform>(targets[index]);
					context.Expect(transform != nullptr && std::abs(transform->position.x) < 0.001F,
						"A rejected packet must leave every surviving target at its authored transform.");
				}
				objects.clear();
			}
		});
	}
}
