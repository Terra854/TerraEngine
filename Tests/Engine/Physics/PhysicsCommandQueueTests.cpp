// PhysicsCommandQueueTests.cpp - red tests for the typed physics command queue.
//
// These tests describe the queue contract before the production queue exists.
// They intentionally include the future engine-owned header so a clean build
// fails until T021 supplies the typed command envelope and queue.

#include "../TestHarness.h"

#include "Physics/PhysicsCommands.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		Physics::PhysicsCommand MakeCommand(
			std::uint64_t effectiveTick,
			std::uint64_t producerId,
			std::uint64_t producerSequence,
			Physics::PhysicsCommandType type,
			const TerraEngine::UUID& target,
			Physics::PhysicsGeneration worldGeneration)
		{
			Physics::PhysicsCommand command;
			command.type = type;
			command.worldId = Physics::PhysicsWorldId{1U};
			command.effectiveTick = effectiveTick;
			command.producerId = producerId;
			command.producerSequence = producerSequence;
			command.targetObjectId = target;
			command.worldGeneration = worldGeneration;
			command.correlationId = "physics-queue-test";
			return command;
		}
	}

	void RegisterPhysicsCommandQueueTests(TestRegistry& registry)
	{
		registry.Add("Physics.CommandQueue.MergesByTickBoundaryAndStableTieKey", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue(Physics::PhysicsCommandQueueLimits{16, 8});
			const TerraEngine::UUID firstTarget = TerraEngine::GenerateUUID();
			const TerraEngine::UUID secondTarget = TerraEngine::GenerateUUID();
			const auto first = MakeCommand(4, 20, 1, Physics::PhysicsCommandType::ApplyForce, firstTarget, 7);
			const auto second = MakeCommand(2, 10, 8, Physics::PhysicsCommandType::ApplyForce, secondTarget, 7);
			const auto tie = MakeCommand(4, 10, 9, Physics::PhysicsCommandType::DestroyBody, secondTarget, 7);

			context.Expect(queue.Enqueue(first).accepted, "The first command should be accepted.");
			context.Expect(queue.Enqueue(second).accepted, "The earlier tick should be accepted.");
			context.Expect(queue.Enqueue(tie).accepted, "The tie-break command should be accepted.");

			const auto merged = queue.MergeForProtectedBoundary(4, 7);
			context.ExpectEqual(static_cast<std::size_t>(3), merged.commands.size(), "All commands up to the effective tick should merge.");
			context.Expect(merged.commands[0].effectiveTick == 2, "Effective tick must be the primary merge key.");
			context.Expect(merged.commands[1].producerId == 10 && merged.commands[1].producerSequence == 9, "Protected-boundary sequence must precede command type for equal targets.");
			context.Expect(merged.commands[2].producerId == 20, "Global ordering must not depend on arrival timing.");
		});

		registry.Add("Physics.CommandQueue.UsesCommandTypeAndStableTargetForExactTies", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue(Physics::PhysicsCommandQueueLimits{8, 8});
			const TerraEngine::UUID target = TerraEngine::GenerateUUID();
			auto destroy = MakeCommand(3, 2, 1, Physics::PhysicsCommandType::DestroyBody, target, 2);
			destroy.protectedBoundarySequence = 3;
			auto apply = MakeCommand(3, 1, 1, Physics::PhysicsCommandType::ApplyForce, target, 2);
			apply.protectedBoundarySequence = 3;
			context.Expect(queue.Enqueue(destroy).accepted, "The destruction command should be accepted.");
			context.Expect(queue.Enqueue(apply).accepted, "The force command should be accepted.");
			const auto merged = queue.MergeForProtectedBoundary(3, 2);
			context.ExpectEqual(static_cast<std::size_t>(2), merged.commands.size(), "Both exact-tie commands should remain visible.");
			context.Expect(merged.commands[0].targetObjectId == target && merged.commands[1].targetObjectId == target, "Tie ordering must retain stable target identity.");
			context.Expect(merged.commands[0].type == Physics::PhysicsCommandType::ApplyForce
				&& merged.commands[1].type == Physics::PhysicsCommandType::DestroyBody,
				"The command-type tie policy must apply force before final destruction.");
		});

		registry.Add("Physics.CommandQueue.EnforcesBoundedMultiProducerAcceptance", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue(Physics::PhysicsCommandQueueLimits{32, 32});
			std::atomic<std::uint64_t> nextSequence = 1;
			std::vector<std::thread> producers;
			for (std::uint64_t producer = 0; producer < 4; ++producer)
			{
				producers.emplace_back([&queue, &nextSequence, producer]() {
					for (std::uint64_t index = 0; index < 16; ++index)
					{
						auto command = MakeCommand(1, producer, nextSequence.fetch_add(1), Physics::PhysicsCommandType::ApplyForce, TerraEngine::GenerateUUID(), 1);
						(void)queue.Enqueue(command);
					}
				});
			}
			for (auto& producer : producers)
			{
				producer.join();
			}
			const auto diagnostics = queue.GetDiagnostics();
			context.Expect(diagnostics.acceptedCount <= 32, "The queue must never exceed its configured capacity.");
			context.Expect(diagnostics.rejectedBackpressureCount > 0, "Overflow must report bounded backpressure rejections.");
		});

		registry.Add("Physics.CommandQueue.SupportsCancellationAndIdempotencyMetadata", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue(Physics::PhysicsCommandQueueLimits{8, 8});
			auto command = MakeCommand(2, 5, 1, Physics::PhysicsCommandType::Teleport, TerraEngine::GenerateUUID(), 9);
			command.idempotencyKey = "teleport-once";
			command.requestFingerprint = "fingerprint-v1";
			const auto receipt = queue.Enqueue(command);
			context.Expect(receipt.accepted, "The idempotent command should initially be accepted.");
			context.Expect(queue.Cancel(receipt.commandId), "Queued commands should be cancellable before publication.");
			context.Expect(queue.Enqueue(command).duplicateIdempotency, "A duplicate idempotency key must not enqueue a second action.");
			const auto merged = queue.MergeForProtectedBoundary(2, 9);
			context.Expect(merged.commands.empty(), "Cancelled commands must not reach the protected boundary.");
		});

		registry.Add("Physics.CommandQueue.RejectsStaleWorldGenerationWithoutPublication", [](TestContext& context) {
			Physics::PhysicsCommandQueue queue(Physics::PhysicsCommandQueueLimits{8, 8});
			const auto stale = MakeCommand(1, 1, 1, Physics::PhysicsCommandType::ApplyImpulse, TerraEngine::GenerateUUID(), 11);
			context.Expect(queue.Enqueue(stale).accepted, "A command may queue before the world generation is checked.");
			const auto merged = queue.MergeForProtectedBoundary(1, 12);
			context.Expect(merged.commands.empty(), "A stale world generation must be rejected at the protected boundary.");
			context.ExpectEqual(static_cast<std::size_t>(1), merged.staleWorldGenerationCount, "Stale rejection must be diagnosed.");
		});
	}
}
