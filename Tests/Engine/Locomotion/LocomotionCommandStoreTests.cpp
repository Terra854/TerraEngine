// LocomotionCommandStoreTests.cpp - bounded runtime admission and authority tests.

#include "TestHarness.h"

#include "Locomotion/LocomotionCommands.h"

#include <array>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;

		LocomotionHandle MakeIdentity(std::uint8_t objectByte) {
			LocomotionHandle identity{};
			identity.sceneLifetime = 7U;
			identity.objectId[0] = objectByte;
			identity.objectGeneration = 3U;
			identity.attachmentGeneration = 5U;
			return identity;
		}

		ResolvedLocomotionBindings MakeBindings(std::uint8_t objectByte) {
			ResolvedLocomotionBindings bindings{};
			bindings.configurationRevision = 1U;
			bindings.controller.world = {{8U}, 2U};
			bindings.controller.character = {{8U}, 2U, objectByte, 4U};
			bindings.controller.objectId[0] = objectByte;
			bindings.controller.objectGeneration = 3U;
			bindings.controller.componentGeneration = 5U;
			bindings.frame = {Physics::kPrimaryPhysicsFrameId, 1U};
			bindings.controllerSupported = true;
			return bindings;
		}

		LocomotionCommand Acquire(LocomotionHandle handle, std::uint64_t producer,
			std::uint64_t sequence) {
			LocomotionCommand command{};
			command.kind = CommandKind::Acquire;
			command.handle = handle;
			command.expectedConfigurationRevision = 1U;
			command.producerId = producer;
			command.sequence = sequence;
			command.correlationId = sequence + 100U;
			return command;
		}
	}

	void RegisterLocomotionCommandStoreTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Commands.AcquireRetryAndSequenceConflict", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto handle = store.Bind(MakeIdentity(1U), MakeBindings(1U)).handle;
			const auto command = Acquire(handle, 11U, 1U);
			const auto queued = store.SubmitBatch(std::span{&command, 1U}, 1U);
			context.Expect(queued.queued && queued.receipts.size() == 1U
				&& queued.receipts[0].state == ReceiptState::Queued,
				"A valid acquisition first returns a queued receipt.");
			auto prepared = store.PrepareNextBatch(1U, {});
			context.Expect(prepared && prepared->accepted && prepared->transitions.size() == 1U,
				"Protected preparation must propose one owner transition.");
			if(!prepared) return;
			context.Expect(store.CommitPreparedBatch(std::move(*prepared)),
				"A prepared acquisition must atomically publish its authority root.");
			const auto applied = store.ReadReceipt(handle, 11U, 1U, 101U);
			context.Expect(applied.state == ReceiptState::Applied && applied.ownerToken != 0U
				&& applied.authorityEpoch == 1U,
				"Only a protected commit grants an opaque native owner token.");
			const auto retry = store.SubmitBatch(std::span{&command, 1U}, 2U);
			context.Expect(retry.queued && retry.receipts.size() == 1U
				&& retry.receipts[0].ownerToken == applied.ownerToken && store.PendingCount() == 0U,
				"An exact retry returns the original receipt without executing twice.");
			auto conflict = command;
			conflict.mode = MotionMode::RootMotion;
			const auto rejected = store.SubmitBatch(std::span{&conflict, 1U}, 2U);
			context.Expect(!rejected.queued && rejected.reason == LocomotionReason::SequenceConflict,
				"A changed payload cannot reuse a producer sequence.");
		});

		registry.Add("Engine.Locomotion.Commands.RejectedBatchLeavesAuthorityUnchanged", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto first = store.Bind(MakeIdentity(1U), MakeBindings(1U)).handle;
			const auto second = store.Bind(MakeIdentity(2U), MakeBindings(2U)).handle;
			const auto firstCommand = Acquire(first, 11U, 1U);
			auto secondCommand = Acquire(second, 12U, 1U);
			secondCommand.expectedConfigurationRevision = 2U;
			const std::array commands{firstCommand, secondCommand};
			context.Expect(store.SubmitBatch(commands, 1U).queued,
				"Complete batches may queue before protected binding checks.");
			auto prepared = store.PrepareNextBatch(1U, {});
			context.Expect(prepared && !prepared->accepted
				&& prepared->reason == LocomotionReason::StaleConfiguration,
				"One stale configuration must reject the complete candidate batch.");
			if(!prepared) return;
			context.Expect(store.RejectPreparedBatch(std::move(*prepared), LocomotionReason::StaleConfiguration),
				"A failed physical or binding reservation must complete every batch receipt.");
			const auto root = store.CopyAuthorityRoot();
			context.Expect(root && root->entries.size() == 2U
				&& root->entries[0].ownerToken == 0U && root->entries[1].ownerToken == 0U,
				"Rejected batches publish no partial owner acquisition.");
		});

		registry.Add("Engine.Locomotion.Commands.ReservedStopBypassesOrdinaryPerActorLimit", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto handle = store.Bind(MakeIdentity(1U), MakeBindings(1U)).handle;
			const auto acquire = Acquire(handle, 11U, 1U);
			context.Expect(store.SubmitBatch(std::span{&acquire, 1U}, 1U).queued,
				"Acquire must queue for the safety fixture.");
			auto first = store.PrepareNextBatch(1U, {});
			if(!first || !store.CommitPreparedBatch(std::move(*first))) {
				context.Expect(false, "Acquire must complete for the safety fixture.");
				return;
			}
			const auto token = store.ReadReceipt(handle, 11U, 1U, 101U).ownerToken;
			for(std::uint64_t sequence = 2U; sequence <= 65U; ++sequence) {
				auto intent = Acquire(handle, 11U, sequence);
				intent.kind = CommandKind::SetIntent;
				intent.ownerToken = token;
				intent.expectedAuthorityEpoch = 1U;
				context.Expect(store.SubmitBatch(std::span{&intent, 1U}, 2U).queued,
					"The ordinary per-actor command budget must admit its exact bound.");
			}
			auto overflow = Acquire(handle, 11U, 66U);
			overflow.kind = CommandKind::SetIntent;
			overflow.ownerToken = token;
			overflow.expectedAuthorityEpoch = 1U;
			context.Expect(!store.SubmitBatch(std::span{&overflow, 1U}, 2U).queued
				&& store.ReadReceipt(handle, 11U, 66U, 166U).reason == LocomotionReason::CapacityExceeded,
				"A rejected ordinary overflow retains its exact terminal receipt.");
			auto stop = Acquire(handle, 11U, 67U);
			stop.kind = CommandKind::Stop;
			stop.ownerToken = token;
			stop.expectedAuthorityEpoch = 1U;
			context.Expect(store.SubmitBatch(std::span{&stop, 1U}, 2U).queued
				&& store.PendingCount() == 65U,
				"The reserved per-slot safety cell must admit Stop despite a full ordinary queue.");
			auto prepared = store.PrepareNextBatch(2U, {});
			context.Expect(prepared && prepared->accepted && prepared->receipts[0].sequence == 67U,
				"The safety Stop must preempt older ordinary work at the protected boundary.");
			if(prepared) context.Expect(store.CommitPreparedBatch(std::move(*prepared)),
				"The reserved safety Stop must commit without ordinary queue capacity.");
		});

		registry.Add("Engine.Locomotion.Commands.InvalidStopCannotReplaceReservedStop", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto handle = store.Bind(MakeIdentity(3U), MakeBindings(3U)).handle;
			const auto acquire = Acquire(handle, 31U, 1U);
			context.Expect(store.SubmitBatch(std::span{&acquire, 1U}, 1U).queued,
				"Acquire must queue for Stop ownership validation.");
			auto preparedAcquire = store.PrepareNextBatch(1U, {});
			if(!preparedAcquire || !store.CommitPreparedBatch(std::move(*preparedAcquire))) {
				context.Expect(false, "Acquire must complete for Stop ownership validation.");
				return;
			}
			const auto token = store.ReadReceipt(handle, 31U, 1U, 101U).ownerToken;
			auto validStop = Acquire(handle, 31U, 2U);
			validStop.kind = CommandKind::Stop;
			validStop.ownerToken = token;
			validStop.expectedAuthorityEpoch = 1U;
			context.Expect(store.SubmitBatch(std::span{&validStop, 1U}, 2U).queued,
				"The current owner may reserve its Stop cell.");
			auto invalidStop = validStop;
			invalidStop.producerId = 32U;
			invalidStop.correlationId = 202U;
			context.Expect(!store.SubmitBatch(std::span{&invalidStop, 1U}, 2U).queued,
				"A different producer cannot replace the valid pending Stop.");
			auto preparedStop = store.PrepareNextBatch(2U, {});
			context.Expect(preparedStop && preparedStop->accepted
				&& preparedStop->receipts[0].producerId == 31U,
				"The original owner Stop must survive the competing invalid submission.");
			if(preparedStop) context.Expect(store.CommitPreparedBatch(std::move(*preparedStop))
				&& store.Read(handle)->authority.state == AuthorityState::Stopped,
				"The valid Stop must still remove movement contribution.");
		});
	}
}
