// PhysicsStalePublicationTests.cpp - stale publication rejects whole ticks.

#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsTransactions.h"
#include "Physics/TransformPublicationPacket.h"
#include "Physics/PhysicsCommands.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsQueries.h"
#include "Physics/PhysicsSettings.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/SceneMutationQueue.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		Physics::PhysicsPose StaleTestPose(float x)
		{
			return {{x, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}};
		}

		Physics::TransformPublicationSourceRecord CaptureStaleTestRecord(
			ObjectManager& objects,
			const Object& object,
			Physics::PhysicsWorldHandle world,
			Physics::PhysicsSlotIndex slot,
			Physics::PhysicsGeneration bodyGeneration,
			float targetX)
		{
			const std::optional<Object> parent = objects.getParent(object);
			return {
				.objectId = objects.getUUID(object),
				.body = {world.worldId, world.worldGeneration, slot, bodyGeneration},
				.frame = {Physics::kPrimaryPhysicsFrameId, world.worldGeneration},
				.objectLifetimeGeneration = objects.getObjectLifetimeGeneration(object),
				.bodyGeneration = bodyGeneration,
				.componentGeneration = objects.getObjectComponentGeneration(object),
				.expectedTransformRevision = objects.GetObjectTransformRevision(object),
				.expectedHierarchyGeneration = objects.GetHierarchyRevision(),
				.parentObjectId = parent ? objects.getUUID(*parent) : TerraEngine::EMPTY_UUID,
				.expectedParentLifetimeGeneration = parent ? objects.getObjectLifetimeGeneration(*parent) : 0U,
				.expectedParentTransformRevision = parent ? objects.GetObjectTransformRevision(*parent) : 0U,
				.canonicalFrameOrigin = {},
				.localPose = StaleTestPose(targetX),
				.worldPoseReceipt = StaleTestPose(targetX),
				.poseChanged = true};
		}

		Physics::TransformPublicationPacket MakeStaleTestPacket(
			ObjectManager& objects,
			Physics::PhysicsWorldHandle world,
			std::span<const Physics::TransformPublicationSourceRecord> records,
			std::uint64_t groupId)
		{
			return Physics::PrepareTransformPublicationPacket(
				{world, {Physics::kPrimaryPhysicsFrameId, world.worldGeneration}, 9U, 2U, 3U}, objects.GetSceneGeneration(), objects.GetHierarchyRevision(),
				groupId, groupId + 20U, records, 1U,
				Physics::TransformPublicationVisibilityBoundary::FrameEnd);
		}

		Physics::PhysicsQueryRequest MakeStaleRayRequest(Physics::PhysicsWorldHandle world, Physics::PhysicsTick tick)
		{
			Physics::PhysicsQueryRequest request;
			request.world = world;
			request.kind = Physics::PhysicsQueryKind::RayCast;
			request.pose.position = {-1.0F, 0.0F, 0.0F};
			request.direction = {1.0F, 0.0F, 0.0F};
			request.distance = 10.0F;
			request.maximumHits = 1U;
			request.submittedTick = tick;
			request.correlationId = "stale-publication";
			return request;
		}

		Physics::PhysicsEvent MakeStaleCollisionEvent(
			Physics::PhysicsWorldHandle world,
			Physics::PhysicsBodyHandle firstBody,
			Physics::PhysicsBodyHandle secondBody,
			Physics::PhysicsTick tick)
		{
			Physics::PhysicsEvent event;
			event.type = Physics::PhysicsEventType::CollisionEnter;
			event.world = world;
			event.tick = tick;
			event.first.objectId = TerraEngine::GenerateUUID();
			event.first.objectGeneration = 1U;
			event.first.componentGeneration = 1U;
			event.first.colliderRecordId = TerraEngine::GenerateUUID();
			event.first.recordGeneration = 1U;
			event.first.body = firstBody;
			event.first.collisionLayer = 0U;
			event.second.objectId = TerraEngine::GenerateUUID();
			event.second.objectGeneration = 1U;
			event.second.componentGeneration = 1U;
			event.second.colliderRecordId = TerraEngine::GenerateUUID();
			event.second.recordGeneration = 1U;
			event.second.body = secondBody;
			event.second.collisionLayer = 0U;
			event.hasSecondParticipant = true;
			event.normal = {1.0F, 0.0F, 0.0F};
			return event;
		}

		bool ReplaceStaleTestWorld(Physics::PhysicsWorld& world)
		{
			return world.BeginStopping()
				&& world.BeginDraining() == 0U
				&& world.CompleteTeardown()
				&& world.Create(Physics::PhysicsSettings::Defaults());
		}
	}

	void RegisterPhysicsStalePublicationTests(TestRegistry& registry)
	{
		registry.Add("Physics.Publication.StaleTransformRejectsEveryChunkWithoutPartialMutation", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue queue;
			const Physics::PhysicsWorldHandle world{{101U}, 2U};
			std::vector<Object> targets{objects.createObject("First"), objects.createObject("Stale"), objects.createObject("Last")};
			std::vector<Physics::TransformPublicationSourceRecord> records;
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = world;
			for (std::size_t index = 0U; index < targets.size(); ++index)
			{
				const auto generation = static_cast<Physics::PhysicsGeneration>(4U + index);
				records.push_back(CaptureStaleTestRecord(objects, targets[index], world, static_cast<Physics::PhysicsSlotIndex>(index), generation, 10.0F + static_cast<float>(index)));
				runtime.bodyHandles.emplace(objects.getUUID(targets[index]), records.back().body);
				runtime.bodyGenerations.emplace(objects.getUUID(targets[index]), records.back().bodyGeneration);
			}
			const Physics::TransformPublicationPacket packet = MakeStaleTestPacket(objects, world, records, 3U);
			TerraEngine::Scene::WorldPose authoredPose = objects.GetCanonicalWorldPose(targets[1]).value();
			authoredPose.position.offsetMetres.x = 99.0;
			context.Expect(objects.SetCanonicalWorldPose(targets[1], authoredPose) == TerraEngine::Scene::WorldCoordinateError::None,
				"The stale-publication fixture must publish its newer authored pose.");
			objects.MarkObjectTransformDirty(targets[1]);
			queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(packet));

			const Scene::SceneMutationBatchResult flushed = queue.Apply(objects, "Frame End");
			context.ExpectEqual(static_cast<std::size_t>(1), flushed.publicationGroupResults.size(), "Rejected tick should still return one group receipt.");
			context.Expect(!flushed.publicationGroupResults.front().Committed(), "One stale transform must reject every preparation chunk.");
			context.Expect(std::abs(objects.GetComponent<Transform>(targets[0])->worldPose.position.offsetMetres.x) < 0.001, "An earlier valid chunk must not become partially visible.");
			context.Expect(std::abs(objects.GetComponent<Transform>(targets[1])->worldPose.position.offsetMetres.x - 99.0) < 0.001, "A rejected publication must preserve the newer authored transform.");
			context.Expect(std::abs(objects.GetComponent<Transform>(targets[2])->worldPose.position.offsetMetres.x) < 0.001, "A later valid chunk must not become partially visible.");
			const Physics::TransformPublicationReceipt receipt = Physics::MakeTransformPublicationReceipt(packet, flushed.publicationGroupResults.front());
			context.Expect(!receipt.Accepted() && receipt.acknowledgedEntries.empty(), "Rejected ticks must not acknowledge retained last-published poses.");
			objects.clear();
		});

		registry.Add("Physics.Publication.ReparentAfterPreparationRejectsTick", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object firstParent = objects.createObject("First Parent");
			const Object secondParent = objects.createObject("Second Parent");
			const Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, firstParent), "Fixture child should start under its observed parent.");
			const Physics::PhysicsWorldHandle world{{102U}, 5U};
			std::vector<Physics::TransformPublicationSourceRecord> records{
				CaptureStaleTestRecord(objects, child, world, 0U, 6U, 7.0F)};
			const Physics::TransformPublicationPacket packet = MakeStaleTestPacket(objects, world, records, 4U);
			context.Expect(objects.setParent(child, secondParent), "Fixture should reparent after worker preparation.");

			const Scene::TransactionPublicationGroupResult result = Scene::CommitTransactionPublicationGroup(objects, Physics::BuildTransformPublicationGroup(packet));
			context.Expect(!result.Committed(), "Parent identity/hierarchy changes must reject the complete publication group.");
			context.Expect(std::abs(objects.GetComponent<Transform>(child)->worldPose.position.offsetMetres.x) < 0.001, "Rejected reparent conflicts must preserve the authored local transform.");
			context.Expect(objects.getParent(child) == secondParent, "Publication rejection must not roll hierarchy back to stale worker state.");
			objects.clear();
		});

		registry.Add("Physics.Publication.StaleBodyGenerationFailsBeforeQueueing", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object target = objects.createObject("Rebuilt Body");
			const Physics::PhysicsWorldHandle world{{103U}, 7U};
			std::vector<Physics::TransformPublicationSourceRecord> records{
				CaptureStaleTestRecord(objects, target, world, 0U, 9U, 4.0F)};
			const Physics::TransformPublicationPacket packet = MakeStaleTestPacket(objects, world, records, 5U);
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = world;
			runtime.bodyHandles.emplace(objects.getUUID(target), Physics::PhysicsBodyHandle{world.worldId, world.worldGeneration, 0U, 10U});
			runtime.bodyGenerations.emplace(objects.getUUID(target), records.front().bodyGeneration);

			const Physics::TransformPublicationValidationResult validation = Physics::ValidatePhysicsTransformPublication(objects, runtime, packet);
			context.Expect(!validation.accepted, "A rebuilt provider body must invalidate the extracted publication record.");
			context.Expect(validation.reason == Physics::TransformPublicationValidationReason::StaleBody, "Body mismatch should report a bounded stale-body reason.");
			const Scene::TransactionPublicationGroupResult result = Physics::CommitPhysicsTransformPublication(objects, runtime, packet);
			context.Expect(!result.Committed() && result.appliedWriteCount == 0U, "Stale body publication must perform zero scene writes.");
			context.Expect(std::abs(objects.GetComponent<Transform>(target)->worldPose.position.offsetMetres.x) < 0.001, "Stale body publication must leave live transforms unchanged.");
			objects.clear();
		});

		registry.Add("Physics.Publication.RecreatedObjectLifetimeAndSceneSwitchAreObserved", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object target = objects.createObject("Original Lifetime");
			const TerraEngine::UUID targetId = objects.getUUID(target);
			const Physics::PhysicsWorldHandle world{{104U}, 8U};
			std::vector<Physics::TransformPublicationSourceRecord> records{
				CaptureStaleTestRecord(objects, target, world, 0U, 3U, 5.0F)};
			const Physics::TransformPublicationPacket packet = MakeStaleTestPacket(objects, world, records, 6U);
			objects.removeObject(target);
			const Object replacement = objects.createObject("Replacement Lifetime");
			objects.UpdateObjectUUID(replacement, targetId);

			const Scene::TransactionPublicationGroupResult lifetimeResult = Scene::CommitTransactionPublicationGroup(objects, Physics::BuildTransformPublicationGroup(packet));
			const bool foundLifetimeConflict = std::any_of(
				lifetimeResult.aggregateResult.conflicts.begin(),
				lifetimeResult.aggregateResult.conflicts.end(),
				[](const Scene::TransactionConflictRecord& conflict) {
					return conflict.reason == Scene::TransactionConflictReason::StaleObjectIdentity;
				});
			context.Expect(!lifetimeResult.Committed() && foundLifetimeConflict, "UUID reuse must not let a replacement object satisfy the old lifetime generation.");

			objects.clear();
			const Scene::TransactionPublicationGroupResult sceneResult = Scene::CommitTransactionPublicationGroup(objects, Physics::BuildTransformPublicationGroup(packet));
			const bool foundSceneConflict = std::any_of(
				sceneResult.aggregateResult.conflicts.begin(),
				sceneResult.aggregateResult.conflicts.end(),
				[](const Scene::TransactionConflictRecord& conflict) {
					return conflict.reason == Scene::TransactionConflictReason::SceneUnloadedOrSwitched;
				});
			context.Expect(!sceneResult.Committed() && foundSceneConflict, "A scene clear/swap generation must invalidate every packet from the prior scene lifetime.");
			objects.clear();
		});

		// Keep the original world-lifetime regression matrix beside the new scene
		// publication checks; both prove stale data never gains authority after reuse.
		registry.Add("Physics.StalePublication.RejectsStaleCommandsAcrossOneHundredWorldReplacements", [](TestContext& context) {
			Physics::PhysicsWorld world;
			context.Expect(world.Create(Physics::PhysicsSettings::Defaults()), "The initial world must be created before replacement stress begins.");
			for (std::uint32_t iteration = 0U; iteration < 100U; ++iteration)
			{
				const Physics::PhysicsWorldHandle oldWorld = world.Handle();
				const Physics::PhysicsBodyHandle oldBody = world.AllocateBody();
				context.Expect(oldWorld.IsValid() && oldBody.IsValid(), "Each old world must allocate a valid body before it is replaced.");
				context.Expect(ReplaceStaleTestWorld(world), "Replacement must drain the old command queue before exposing the next generation.");
				const Physics::PhysicsWorldHandle replacement = world.Handle();
				const Physics::PhysicsBodyHandle replacementBody = world.AllocateBody();
				context.Expect(replacement.worldGeneration != oldWorld.worldGeneration && replacementBody.slotIndex == oldBody.slotIndex, "The stress case must reuse a compact slot under a distinct world generation.");
				context.Expect(world.Status(oldBody) == Physics::PhysicsHandleStatus::StaleWorld && world.Status(replacementBody) == Physics::PhysicsHandleStatus::Alive, "A removed body's handle must not acquire authority over the replacement body.");

				Physics::PhysicsCommandMetadata staleMetadata;
				staleMetadata.effectiveTick = 1U;
				staleMetadata.producerId = 1U;
				staleMetadata.producerSequence = iteration + 1U;
				const Physics::PhysicsBodyCommandTarget staleTarget{oldWorld, TerraEngine::GenerateUUID(), 1U, 1U};
				context.Expect(world.EnqueueCommand(Physics::PhysicsCommand::MakeWake(staleTarget, staleMetadata)).accepted, "Admission may retain stale producer data until the protected replacement boundary validates it.");
				const Physics::PhysicsCommandBatch staleBatch = world.MergeCommands(1U);
				context.Expect(staleBatch.commands.empty() && staleBatch.staleWorldGenerationCount == 1U, "A stale command must be rejected atomically instead of mutating the replacement world.");

				Physics::PhysicsCommandMetadata freshMetadata;
				freshMetadata.effectiveTick = 1U;
				freshMetadata.producerId = 2U;
				freshMetadata.producerSequence = iteration + 1U;
				const Physics::PhysicsBodyCommandTarget freshTarget{replacement, TerraEngine::GenerateUUID(), 1U, 1U};
				const Physics::PhysicsCommandReceipt freshReceipt = world.EnqueueCommand(Physics::PhysicsCommand::MakeWake(freshTarget, freshMetadata));
				const Physics::PhysicsCommandBatch freshBatch = freshReceipt.accepted ? world.MergeCommands(1U) : Physics::PhysicsCommandBatch{};
				context.Expect(freshBatch.commands.size() == 1U && freshBatch.commands.front().worldGeneration == replacement.worldGeneration, "Rejecting the stale command must not partially stop a valid replacement command.");
				context.Expect(world.Release(replacementBody), "The replacement body must retire before the next reuse iteration begins.");
			}
		});

		registry.Add("Physics.StalePublication.RejectsStaleQueryHandlesAndPreparedResultsAcrossOneHundredReplacements", [](TestContext& context) {
			Physics::PhysicsWorld world;
			context.Expect(world.Create(Physics::PhysicsSettings::Defaults()), "The initial world must exist for query replacement stress.");
			for (std::uint32_t iteration = 0U; iteration < 100U; ++iteration)
			{
				const Physics::PhysicsWorldHandle oldWorld = world.Handle();
				const Physics::PhysicsQueryReceipt oldReceipt = world.SubmitQuery(MakeStaleRayRequest(oldWorld, 1U));
				context.Expect(oldReceipt.accepted, "The old world must issue a query handle before replacement.");
				const Physics::PhysicsPreparedQueryBatch oldPreparedBatch = world.PrepareQueries(1U);
				context.Expect(oldPreparedBatch.queries.size() == 1U, "The old query must prepare before the world lifetime changes.");
				const Physics::PhysicsPreparedQuery oldPrepared = oldPreparedBatch.queries.front();
				const Physics::PhysicsBodyHandle oldBody = world.AllocateBody();

				context.Expect(ReplaceStaleTestWorld(world), "The old query manager must retire with its world generation.");
				const Physics::PhysicsWorldHandle replacement = world.Handle();
				const Physics::PhysicsBodyHandle replacementBody = world.AllocateBody();
				context.Expect(replacementBody.slotIndex == oldBody.slotIndex && replacementBody.worldGeneration != oldBody.worldGeneration, "The replacement world must exercise both slot reuse and world-generation invalidation.");
				context.Expect(!world.CompleteQuery(oldPrepared, 1U, Physics::PhysicsResultStatus::Success, {}), "A worker result prepared by an old world must not publish after replacement.");
				const Physics::PhysicsQueryResult staleRead = world.ReadQuery(oldReceipt.handle);
				context.Expect(staleRead.status == Physics::PhysicsResultStatus::StaleTarget && staleRead.rejectionReason == Physics::PhysicsQueryRejectionReason::StaleWorld, "An old query handle must never read a replacement manager slot.");
				context.Expect(!world.SubmitQuery(MakeStaleRayRequest(oldWorld, 1U)).accepted, "A request carrying an old world identity must be rejected before replacement allocation.");

				const Physics::PhysicsQueryReceipt freshReceipt = world.SubmitQuery(MakeStaleRayRequest(replacement, 1U));
				const Physics::PhysicsPreparedQueryBatch freshPreparedBatch = world.PrepareQueries(1U);
				context.Expect(freshReceipt.accepted && freshPreparedBatch.queries.size() == 1U, "The replacement manager must accept independent fresh work after stale work is rejected.");
				Physics::PhysicsQueryHit staleCandidate;
				staleCandidate.body = oldBody;
				staleCandidate.objectId = TerraEngine::GenerateUUID();
				staleCandidate.objectGeneration = 1U;
				staleCandidate.componentGeneration = 1U;
				staleCandidate.position = {1.0F, 0.0F, 0.0F};
				staleCandidate.normal = {-1.0F, 0.0F, 0.0F};
				staleCandidate.distance = 1.0F;
				staleCandidate.fraction = 0.1F;
				context.Expect(world.CompleteQuery(freshPreparedBatch.queries.front(), 1U, Physics::PhysicsResultStatus::Success, {&staleCandidate, 1U}), "A replacement query may complete while filtering stale provider candidates.");
				const Physics::PhysicsQueryResult freshRead = world.ReadQuery(freshReceipt.handle);
				context.Expect(freshRead.hits != nullptr && freshRead.hits->empty() && freshRead.world == replacement, "No old query hit may leak into the immutable replacement result packet.");
				context.Expect(world.Release(replacementBody), "The replacement query fixture body must retire before the next reuse iteration.");
			}
		});

		registry.Add("Physics.StalePublication.RejectsStaleEventPacketsAndParticipantsAcrossOneHundredReplacements", [](TestContext& context) {
			Physics::PhysicsWorld world;
			context.Expect(world.Create(Physics::PhysicsSettings::Defaults()), "The initial world must exist for event replacement stress.");
			for (std::uint32_t iteration = 0U; iteration < 100U; ++iteration)
			{
				const Physics::PhysicsWorldHandle oldWorld = world.Handle();
				const Physics::PhysicsBodyHandle oldFirstBody = world.AllocateBody();
				const Physics::PhysicsBodyHandle oldSecondBody = world.AllocateBody();
				world.SubscribeEvents({});
				const Physics::PhysicsEvent oldEvent = MakeStaleCollisionEvent(oldWorld, oldFirstBody, oldSecondBody, 1U);
				const std::shared_ptr<const Physics::PhysicsEventPacket> oldPacket = world.PublishEvents(1U, {&oldEvent, 1U});
				context.Expect(oldPacket != nullptr && oldPacket->events != nullptr && oldPacket->events->size() == 1U, "An old immutable packet must exist before the replacement race begins.");

				context.Expect(ReplaceStaleTestWorld(world), "World replacement must discard the old event history as one protected operation.");
				const Physics::PhysicsWorldHandle replacement = world.Handle();
				const Physics::PhysicsBodyHandle replacementFirstBody = world.AllocateBody();
				const Physics::PhysicsBodyHandle replacementSecondBody = world.AllocateBody();
				context.Expect(!oldPacket->IsValidFor(replacement, 1U) && world.AcquireEventPacket(1U, 1U) == nullptr, "Old event packets must be unacquirable once a new world generation is visible.");

				world.SubscribeEvents({});
				const std::shared_ptr<const Physics::PhysicsEventPacket> stalePublication = world.PublishEvents(2U, {&oldEvent, 1U});
				context.Expect(stalePublication != nullptr && stalePublication->events != nullptr && stalePublication->events->empty() && stalePublication->droppedCount == 1U, "Stale callback participants must be dropped rather than published into the replacement packet.");
				const Physics::PhysicsEvent freshEvent = MakeStaleCollisionEvent(replacement, replacementFirstBody, replacementSecondBody, 3U);
				const std::shared_ptr<const Physics::PhysicsEventPacket> freshPacket = world.PublishEvents(3U, {&freshEvent, 1U});
				context.Expect(freshPacket != nullptr && freshPacket->events != nullptr && freshPacket->events->size() == 1U && freshPacket->events->front().world == replacement, "Rejecting stale event data must leave the replacement event path fully usable without partial state.");
			}
		});
	}
}
