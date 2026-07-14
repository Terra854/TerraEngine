// PhysicsStalePublicationTests.cpp - regression coverage for world replacement.
//
// These tests deliberately repeat replacement one hundred times. A stale public
// handle must never regain authority merely because a compact slot is reused by
// a later world lifetime.

#include "../TestHarness.h"

#include "Physics/PhysicsCommands.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsQueries.h"
#include "Physics/PhysicsSettings.h"
#include "Physics/PhysicsWorld.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <span>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Physics;

		PhysicsQueryRequest MakeRayRequest(PhysicsWorldHandle world, PhysicsTick tick) {
			PhysicsQueryRequest request;
			request.world = world;
			request.kind = PhysicsQueryKind::RayCast;
			request.pose.position = {-1.0F, 0.0F, 0.0F};
			request.direction = {1.0F, 0.0F, 0.0F};
			request.distance = 10.0F;
			request.maximumHits = 1U;
			request.submittedTick = tick;
			request.correlationId = "stale-publication";
			return request;
		}

		PhysicsEvent MakeCollisionEvent(
			PhysicsWorldHandle world,
			PhysicsBodyHandle firstBody,
			PhysicsBodyHandle secondBody,
			PhysicsTick tick) {
			PhysicsEvent event;
			event.type = PhysicsEventType::CollisionEnter;
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

		bool ReplaceWorld(PhysicsWorld& world) {
			return world.BeginStopping()
				&& world.BeginDraining() == 0U
				&& world.CompleteTeardown()
				&& world.Create(PhysicsSettings::Defaults());
		}
	}

	void RegisterPhysicsStalePublicationTests(TestRegistry& registry) {
		registry.Add("Physics.StalePublication.RejectsStaleCommandsAcrossOneHundredWorldReplacements", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The initial world must be created before replacement stress begins.");

			for(std::uint32_t iteration = 0U; iteration < 100U; ++iteration) {
				const PhysicsWorldHandle oldWorld = world.Handle();
				const PhysicsBodyHandle oldBody = world.AllocateBody();
				context.Expect(oldWorld.IsValid() && oldBody.IsValid(), "Each old world must allocate a valid body before it is replaced.");
				context.Expect(ReplaceWorld(world), "Replacement must drain the old command queue before exposing the next generation.");
				const PhysicsWorldHandle replacement = world.Handle();
				const PhysicsBodyHandle replacementBody = world.AllocateBody();
				context.Expect(replacement.worldGeneration != oldWorld.worldGeneration
					&& replacementBody.slotIndex == oldBody.slotIndex,
					"The stress case must reuse a compact slot under a distinct world generation.");
				context.Expect(world.Status(oldBody) == PhysicsHandleStatus::StaleWorld
					&& world.Status(replacementBody) == PhysicsHandleStatus::Alive,
					"A removed body's handle must not acquire authority over the replacement body.");

				PhysicsCommandMetadata staleMetadata;
				staleMetadata.effectiveTick = 1U;
				staleMetadata.producerId = 1U;
				staleMetadata.producerSequence = iteration + 1U;
				const PhysicsBodyCommandTarget staleTarget{oldWorld, TerraEngine::GenerateUUID(), 1U, 1U};
				context.Expect(world.EnqueueCommand(PhysicsCommand::MakeWake(staleTarget, staleMetadata)).accepted,
					"Admission may retain stale producer data until the protected replacement boundary validates it.");
				const PhysicsCommandBatch staleBatch = world.MergeCommands(1U);
				context.Expect(staleBatch.commands.empty() && staleBatch.staleWorldGenerationCount == 1U,
					"A stale command must be rejected atomically instead of mutating the replacement world.");

				PhysicsCommandMetadata freshMetadata;
				freshMetadata.effectiveTick = 1U;
				freshMetadata.producerId = 2U;
				freshMetadata.producerSequence = iteration + 1U;
				const PhysicsBodyCommandTarget freshTarget{replacement, TerraEngine::GenerateUUID(), 1U, 1U};
				const PhysicsCommandBatch freshBatch = [&world, &freshTarget, &freshMetadata]() {
					const PhysicsCommandReceipt receipt = world.EnqueueCommand(PhysicsCommand::MakeWake(freshTarget, freshMetadata));
					return receipt.accepted ? world.MergeCommands(1U) : PhysicsCommandBatch{};
				}();
				context.Expect(freshBatch.commands.size() == 1U
					&& freshBatch.commands.front().worldGeneration == replacement.worldGeneration,
					"Rejecting the stale command must not partially stop a valid replacement command.");
				context.Expect(world.Release(replacementBody),
					"The replacement body must retire before the next reuse iteration begins.");
			}
		});

		registry.Add("Physics.StalePublication.RejectsStaleQueryHandlesAndPreparedResultsAcrossOneHundredReplacements", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The initial world must exist for query replacement stress.");

			for(std::uint32_t iteration = 0U; iteration < 100U; ++iteration) {
				const PhysicsWorldHandle oldWorld = world.Handle();
				const PhysicsQueryReceipt oldReceipt = world.SubmitQuery(MakeRayRequest(oldWorld, 1U));
				context.Expect(oldReceipt.accepted, "The old world must issue a query handle before replacement.");
				const PhysicsPreparedQueryBatch oldPreparedBatch = world.PrepareQueries(1U);
				context.Expect(oldPreparedBatch.queries.size() == 1U, "The old query must prepare before the world lifetime changes.");
				const PhysicsPreparedQuery oldPrepared = oldPreparedBatch.queries.front();
				const PhysicsBodyHandle oldBody = world.AllocateBody();

				context.Expect(ReplaceWorld(world), "The old query manager must retire with its world generation.");
				const PhysicsWorldHandle replacement = world.Handle();
				const PhysicsBodyHandle replacementBody = world.AllocateBody();
				context.Expect(replacementBody.slotIndex == oldBody.slotIndex
					&& replacementBody.worldGeneration != oldBody.worldGeneration,
					"The replacement world must exercise both slot reuse and world-generation invalidation.");
				context.Expect(!world.CompleteQuery(oldPrepared, 1U, PhysicsResultStatus::Success, {}),
					"A worker result prepared by an old world must not publish after replacement.");
				const PhysicsQueryResult staleRead = world.ReadQuery(oldReceipt.handle);
				context.Expect(staleRead.status == PhysicsResultStatus::StaleTarget
					&& staleRead.rejectionReason == PhysicsQueryRejectionReason::StaleWorld,
					"An old query handle must never read a replacement manager slot.");
				context.Expect(!world.SubmitQuery(MakeRayRequest(oldWorld, 1U)).accepted,
					"A request carrying an old world identity must be rejected before replacement allocation.");

				const PhysicsQueryReceipt freshReceipt = world.SubmitQuery(MakeRayRequest(replacement, 1U));
				const PhysicsPreparedQueryBatch freshPreparedBatch = world.PrepareQueries(1U);
				context.Expect(freshReceipt.accepted && freshPreparedBatch.queries.size() == 1U,
					"The replacement manager must accept independent fresh work after stale work is rejected.");
				PhysicsQueryHit staleCandidate;
				staleCandidate.body = oldBody;
				staleCandidate.objectId = TerraEngine::GenerateUUID();
				staleCandidate.objectGeneration = 1U;
				staleCandidate.componentGeneration = 1U;
				staleCandidate.position = {1.0F, 0.0F, 0.0F};
				staleCandidate.normal = {-1.0F, 0.0F, 0.0F};
				staleCandidate.distance = 1.0F;
				staleCandidate.fraction = 0.1F;
				context.Expect(world.CompleteQuery(freshPreparedBatch.queries.front(), 1U, PhysicsResultStatus::Success, {&staleCandidate, 1U}),
					"A replacement query may complete while filtering stale provider candidates.");
				const PhysicsQueryResult freshRead = world.ReadQuery(freshReceipt.handle);
				context.Expect(freshRead.hits != nullptr && freshRead.hits->empty()
					&& freshRead.world == replacement,
					"No old query hit may leak into the immutable replacement result packet.");
				context.Expect(world.Release(replacementBody),
					"The replacement query fixture body must retire before the next reuse iteration.");
			}
		});

		registry.Add("Physics.StalePublication.RejectsStaleEventPacketsAndParticipantsAcrossOneHundredReplacements", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The initial world must exist for event replacement stress.");

			for(std::uint32_t iteration = 0U; iteration < 100U; ++iteration) {
				const PhysicsWorldHandle oldWorld = world.Handle();
				const PhysicsBodyHandle oldFirstBody = world.AllocateBody();
				const PhysicsBodyHandle oldSecondBody = world.AllocateBody();
				world.SubscribeEvents({});
				const PhysicsEvent oldEvent = MakeCollisionEvent(oldWorld, oldFirstBody, oldSecondBody, 1U);
				const std::shared_ptr<const PhysicsEventPacket> oldPacket = world.PublishEvents(1U, {&oldEvent, 1U});
				context.Expect(oldPacket != nullptr && oldPacket->events != nullptr && oldPacket->events->size() == 1U,
					"An old immutable packet must exist before the replacement race begins.");

				context.Expect(ReplaceWorld(world), "World replacement must discard the old event history as one protected operation.");
				const PhysicsWorldHandle replacement = world.Handle();
				const PhysicsBodyHandle replacementFirstBody = world.AllocateBody();
				const PhysicsBodyHandle replacementSecondBody = world.AllocateBody();
				context.Expect(!oldPacket->IsValidFor(replacement, 1U)
					&& world.AcquireEventPacket(1U, 1U) == nullptr,
					"Old event packets must be unacquirable once a new world generation is visible.");

				world.SubscribeEvents({});
				const std::shared_ptr<const PhysicsEventPacket> stalePublication = world.PublishEvents(2U, {&oldEvent, 1U});
				context.Expect(stalePublication != nullptr && stalePublication->events != nullptr
					&& stalePublication->events->empty() && stalePublication->droppedCount == 1U,
					"Stale callback participants must be dropped rather than published into the replacement packet.");
				const PhysicsEvent freshEvent = MakeCollisionEvent(replacement, replacementFirstBody, replacementSecondBody, 3U);
				const std::shared_ptr<const PhysicsEventPacket> freshPacket = world.PublishEvents(3U, {&freshEvent, 1U});
				context.Expect(freshPacket != nullptr && freshPacket->events != nullptr && freshPacket->events->size() == 1U
					&& freshPacket->events->front().world == replacement,
					"Rejecting stale event data must leave the replacement event path fully usable without partial state.");
			}
		});
	}
}
