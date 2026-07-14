// PhysicsGameplayCommandTests.cpp - native gameplay command contract tests.
//
// These tests stay at the provider-neutral queue boundary. Provider simulation
// tests verify physical movement separately; this suite proves gameplay calls
// retain deterministic scheduling, generation data, and receipt metadata.

#include "../TestHarness.h"

#include "Physics/PhysicsCommands.h"
#include "Utilities/UUID.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		Physics::PhysicsBodyCommandTarget MakeTarget(const TerraEngine::UUID& objectId) {
			return {
				.world = {{17U}, 3U},
				.objectId = objectId,
				.objectGeneration = 11U,
				.componentGeneration = 19U};
		}

		Physics::PhysicsCommandMetadata MakeMetadata(
			Physics::PhysicsSequence producer,
			Physics::PhysicsSequence sequence,
			Physics::PhysicsTick tick = 0U) {
			Physics::PhysicsCommandMetadata metadata;
			metadata.effectiveTick = tick;
			metadata.producerId = producer;
			metadata.producerSequence = sequence;
			metadata.correlationId = "gameplay-command-" + std::to_string(producer) + "-" + std::to_string(sequence);
			metadata.cancellationToken = producer * 1000U + sequence;
			return metadata;
		}
	}

	void RegisterPhysicsGameplayCommandTests(TestRegistry& registry)
	{
		registry.Add("Physics.GameplayCommand.FactoriesPreserveForceAndMotionSemantics", [](TestContext& context) {
			const Physics::PhysicsBodyCommandTarget target = MakeTarget(TerraEngine::GenerateUUID());
			const auto force = Physics::PhysicsCommand::MakeContinuousForce(target, {1.0F, 2.0F, 3.0F}, MakeMetadata(1U, 1U));
			const auto acceleration = Physics::PhysicsCommand::MakeAcceleration(target, {4.0F, 5.0F, 6.0F}, MakeMetadata(1U, 2U));
			const auto impulse = Physics::PhysicsCommand::MakeImpulse(target, {7.0F, 8.0F, 9.0F}, MakeMetadata(1U, 3U));
			const auto pointImpulse = Physics::PhysicsCommand::MakePointImpulse(
				target, {3.0F, 2.0F, 1.0F}, {9.0F, 8.0F, 7.0F}, MakeMetadata(1U, 4U));
			const auto velocityChange = Physics::PhysicsCommand::MakeVelocityChange(target, {2.0F, 0.0F, 0.0F}, MakeMetadata(1U, 5U));
			const auto velocity = Physics::PhysicsCommand::MakeSetVelocity(
				target, {5.0F, 4.0F, 3.0F}, {2.0F, 1.0F, 0.0F}, MakeMetadata(1U, 6U));
			Physics::PhysicsPose pose;
			pose.position = {10.0F, 20.0F, 30.0F};
			const auto teleport = Physics::PhysicsCommand::MakeTeleport(target, pose, {}, {}, MakeMetadata(1U, 7U));
			const auto kinematic = Physics::PhysicsCommand::MakeKinematicTarget(target, pose, 1.0F / 60.0F, MakeMetadata(1U, 8U));
			const auto sleep = Physics::PhysicsCommand::MakeSleep(target, MakeMetadata(1U, 9U));
			const auto wake = Physics::PhysicsCommand::MakeWake(target, MakeMetadata(1U, 10U));

			const auto expectForce = [&context](const Physics::PhysicsCommand& command,
				Physics::PhysicsCommandType type, Physics::PhysicsForceMode mode, bool point) {
				const auto* payload = std::get_if<Physics::PhysicsForcePayload>(&command.payload);
				context.Expect(command.type == type && payload != nullptr && payload->mode == mode
					&& payload->applyAtPoint == point,
					"Every native force factory must preserve its provider-neutral force-mode semantics.");
			};
			expectForce(force, Physics::PhysicsCommandType::ApplyForce, Physics::PhysicsForceMode::Force, false);
			expectForce(acceleration, Physics::PhysicsCommandType::ApplyAcceleration, Physics::PhysicsForceMode::Acceleration, false);
			expectForce(impulse, Physics::PhysicsCommandType::ApplyImpulse, Physics::PhysicsForceMode::Impulse, false);
			expectForce(pointImpulse, Physics::PhysicsCommandType::ApplyImpulse, Physics::PhysicsForceMode::Impulse, true);
			expectForce(velocityChange, Physics::PhysicsCommandType::ApplyVelocityChange, Physics::PhysicsForceMode::VelocityChange, false);
			const auto* velocityPayload = std::get_if<Physics::PhysicsMotionPayload>(&velocity.payload);
			const auto* teleportPayload = std::get_if<Physics::PhysicsMotionPayload>(&teleport.payload);
			const auto* kinematicPayload = std::get_if<Physics::PhysicsMotionPayload>(&kinematic.payload);
			context.Expect(velocity.type == Physics::PhysicsCommandType::SetVelocity && velocityPayload != nullptr
				&& velocityPayload->linearVelocity == Physics::PhysicsVector3{5.0F, 4.0F, 3.0F},
				"Explicit velocity must remain distinct from a force or impulse command.");
			context.Expect(teleport.type == Physics::PhysicsCommandType::Teleport && teleportPayload != nullptr
				&& teleportPayload->pose == pose,
				"Teleport must carry an exact provider-neutral pose.");
			context.Expect(kinematic.type == Physics::PhysicsCommandType::SetKinematicTarget && kinematicPayload != nullptr
				&& kinematicPayload->duration == 1.0F / 60.0F,
				"Kinematic targets must carry the fixed duration used by the protected step boundary.");
			context.Expect(sleep.type == Physics::PhysicsCommandType::SleepBody && wake.type == Physics::PhysicsCommandType::WakeBody,
				"Sleep and wake must be explicit lifecycle commands rather than hidden velocity mutations.");
			context.Expect(force.worldId == target.world.worldId && force.worldGeneration == target.world.worldGeneration
				&& force.objectGeneration == target.objectGeneration && force.componentGeneration == target.componentGeneration,
				"All factory commands must retain world and body generations for stale-target validation.");
		});

		registry.Add("Physics.GameplayCommand.MergesConcurrentProducersByCanonicalKeys", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue({64U, 16U});
			const std::array<TerraEngine::UUID, 4U> targets{
				TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID()};
			std::atomic<std::uint32_t> accepted{0U};
			std::vector<std::thread> producers;
			for(Physics::PhysicsSequence producer = 1U; producer <= targets.size(); ++producer) {
				producers.emplace_back([&queue, &targets, &accepted, producer]() {
					const auto target = MakeTarget(targets[producer - 1U]);
					for(Physics::PhysicsSequence sequence = 1U; sequence <= 4U; ++sequence) {
						auto command = Physics::PhysicsCommand::MakeContinuousForce(
							target, {static_cast<float>(producer), 0.0F, 0.0F}, MakeMetadata(producer, sequence, 8U));
						if(queue.Enqueue(std::move(command)).accepted) ++accepted;
					}
				});
			}
			for(auto& producer : producers) producer.join();
			const Physics::PhysicsCommandBatch batch = queue.MergeForProtectedBoundary(8U, 3U);
			context.ExpectEqual(static_cast<std::size_t>(accepted.load()), batch.commands.size(),
				"Every accepted concurrent producer command must reach exactly one protected boundary.");
			for(std::size_t index = 1U; index < batch.commands.size(); ++index) {
				const auto& previous = batch.commands[index - 1U];
				const auto& current = batch.commands[index];
				context.Expect(previous.producerId < current.producerId
					|| (previous.producerId == current.producerId && previous.producerSequence < current.producerSequence),
					"Worker arrival timing must not change the deterministic producer/sequence merge order.");
				context.Expect(current.protectedBoundarySequence == previous.protectedBoundarySequence + 1U,
					"The merge must assign a monotonic protected-boundary sequence after canonical sorting.");
			}
		});

		registry.Add("Physics.GameplayCommand.ReceiptsBoundMetadataAndSupportCancellation", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue({8U, 8U, 2U, 32U, 16U, 32U});
			const auto target = MakeTarget(TerraEngine::GenerateUUID());
			auto metadata = MakeMetadata(6U, 1U, 12U);
			metadata.correlationId = "teleport-receipt";
			metadata.idempotencyKey = "tp";
			metadata.requestFingerprint = "pose-v1";
			metadata.cancellationToken = 77U;
			const auto command = Physics::PhysicsCommand::MakeTeleport(target, {}, {}, {}, metadata);
			const Physics::PhysicsCommandReceipt accepted = queue.Enqueue(command);
			context.Expect(accepted.accepted && accepted.commandId != 0U && accepted.effectiveTick == 12U
				&& accepted.correlationId == metadata.correlationId && accepted.cancellationToken == 77U,
				"An accepted command receipt must preserve scheduled tick and bounded caller correlation metadata.");
			const Physics::PhysicsCommandReceipt duplicate = queue.Enqueue(command);
			context.Expect(duplicate.duplicateIdempotency && duplicate.commandId == accepted.commandId
				&& duplicate.correlationId == metadata.correlationId,
				"The same idempotency fingerprint must return the original receipt without enqueueing duplicate work.");
			const Physics::PhysicsCommandReceipt cancelled = queue.CancelWithReceipt(accepted.commandId);
			context.Expect(cancelled.cancelled && cancelled.commandId == accepted.commandId
				&& cancelled.correlationId == metadata.correlationId && cancelled.cancellationToken == 77U,
				"Cancellation receipts must let asynchronous callers correlate the removed command.");
			context.Expect(queue.MergeForProtectedBoundary(12U, 3U).commands.empty(),
				"A cancelled command must not become visible at the protected boundary.");

			auto tooLarge = MakeMetadata(6U, 2U, 12U);
			tooLarge.correlationId.assign(33U, 'x');
			const Physics::PhysicsCommandReceipt rejected = queue.Enqueue(
				Physics::PhysicsCommand::MakeWake(target, std::move(tooLarge)));
			context.Expect(!rejected.accepted && rejected.rejectionReason == Physics::PhysicsCommandRejectionReason::MetadataTooLarge,
				"Oversized correlation, idempotency, and fingerprint data must be rejected before queue retention.");
		});

		registry.Add("Physics.GameplayCommand.RejectsStaleWorldGenerationAtBoundary", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue({8U, 8U});
			const auto staleTarget = MakeTarget(TerraEngine::GenerateUUID());
			const Physics::PhysicsCommandReceipt receipt = queue.Enqueue(
				Physics::PhysicsCommand::MakeImpulse(staleTarget, {0.0F, 1.0F, 0.0F}, MakeMetadata(2U, 1U, 4U)));
			context.Expect(receipt.accepted, "A command may be accepted before a later world replacement invalidates its generation.");
			const Physics::PhysicsCommandBatch batch = queue.MergeForProtectedBoundary(4U, 4U);
			context.Expect(batch.commands.empty() && batch.staleWorldGenerationCount == 1U,
				"The protected boundary must reject stale world generations without publishing the body command.");
		});
	}
}
