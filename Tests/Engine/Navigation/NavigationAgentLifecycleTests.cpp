// NavigationAgentLifecycleTests.cpp - bounded command and runtime ownership contracts.
//
// These tests are intentionally written against the value-only store/queue seams. They
// prove ordering, generation safety, and bounded retention without constructing a live
// scene or allowing a worker to resolve an ECS object.

#include "../TestHarness.h"

#include "Navigation/Agents/NavigationAgentCommandQueue.h"
#include "Navigation/Agents/NavigationAgentStore.h"

#include <cstdint>
#include <string_view>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Navigation;

	namespace {
		NavigationAgentIdentity Identity(std::uint64_t objectValue = 1U,
			NavigationGeneration attachment = 3U, NavigationSlotIndex slot = 5U,
			NavigationGeneration slotGeneration = 7U)
		{
			NavigationAgentIdentity identity{};
			identity.world = {NavigationWorldId{41U}, 2U};
			identity.object.objectId = *ParseUUID(objectValue == 1U
				? "8a5f72b749bc4b6ab9a8000000000001"
				: "8a5f72b749bc4b6ab9a8000000000002");
			identity.object.lifetimeGeneration = 9U;
			identity.attachmentGeneration = attachment;
			identity.runtimeSlot = slot;
			identity.runtimeSlotGeneration = slotGeneration;
			return identity;
		}

		NavigationAgentCommand Destination(const NavigationAgentIdentity& identity,
			std::uint64_t correlation, double x)
		{
			NavigationAgentCommand command{};
			command.identity = identity;
			command.kind = NavigationAgentCommandKind::SetLocationDestination;
			command.destination.kind = NavigationDestinationKind::Location;
			command.destination.location.world = identity.world;
			command.destination.location.position.offsetMetres = {x, 0.0, 0.0};
			command.correlationId = correlation;
			return command;
		}

		NavigationAgentCommand Stop(const NavigationAgentIdentity& identity,
			std::uint64_t correlation)
		{
			NavigationAgentCommand command{};
			command.identity = identity;
			command.kind = NavigationAgentCommandKind::Stop;
			command.correlationId = correlation;
			return command;
		}

		NavigationAgentRuntimeRecord Record(const NavigationAgentIdentity& identity)
		{
			NavigationAgentRuntimeRecord record{};
			record.identity = identity;
			return record;
		}
	}

	void RegisterNavigationAgentLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Navigation.AgentLifecycle.DestinationCoalescingPublishesSupersededReceipt", [](TestContext& context) {
			NavigationAgentCommandQueue queue(8U);
			const NavigationAgentIdentity identity = Identity();
			const auto first = queue.Enqueue(Destination(identity, 11U, 4.0));
			const auto second = queue.Enqueue(Destination(identity, 12U, 8.0));
			const auto firstRead = queue.ReadReceipt(identity, first.correlationId);
			const auto drained = queue.Drain();
			context.Expect(first.state == NavigationAgentReceiptState::Queued
				&& second.state == NavigationAgentReceiptState::Queued,
				"Admitted destination commands must first report queued visibility.");
			context.Expect(firstRead.has_value() && firstRead->state == NavigationAgentReceiptState::Superseded,
				"A coalesced destination must retain an explicit superseded terminal receipt.");
			context.Expect(drained.size() == 1U && drained.front().correlationId == 12U,
				"Only the newest pending destination may reach protected capture for one agent.");
		});

		registry.Add("Navigation.AgentLifecycle.StopRemainsOrderedAfterDestination", [](TestContext& context) {
			NavigationAgentCommandQueue queue(8U);
			const NavigationAgentIdentity identity = Identity();
			queue.Enqueue(Destination(identity, 21U, 4.0));
			queue.Enqueue(Stop(identity, 22U));
			const auto drained = queue.Drain();
			context.Expect(drained.size() == 2U
				&& drained[0].kind == NavigationAgentCommandKind::SetLocationDestination
				&& drained[1].kind == NavigationAgentCommandKind::Stop,
				"Stop must remain an ordered intent after an earlier destination, never be bypassed by it.");
		});

		registry.Add("Navigation.AgentLifecycle.CapacityRejectionPreservesValidIntent", [](TestContext& context) {
			NavigationAgentCommandQueue queue(1U);
			const NavigationAgentIdentity identity = Identity();
			const NavigationAgentIdentity otherIdentity = Identity(2U);
			const auto accepted = queue.Enqueue(Destination(identity, 31U, 4.0));
			const auto rejected = queue.Enqueue(Destination(otherIdentity, 32U, 8.0));
			const auto drained = queue.Drain();
			context.Expect(accepted.state == NavigationAgentReceiptState::Queued
				&& rejected.state == NavigationAgentReceiptState::Rejected
				&& rejected.outcome == NavigationOutcome::RejectedCapacity
				&& rejected.reason == NavigationReason::OutputLimit,
				"A full command queue must reject admission explicitly.");
			context.Expect(drained.size() == 1U && drained.front().correlationId == 31U,
				"Rejected admission must preserve the previously valid destination intent.");
		});

		registry.Add("Navigation.AgentLifecycle.InvalidIdentityRejectsWithoutQueueMutation", [](TestContext& context) {
			NavigationAgentCommandQueue queue(4U);
			NavigationAgentIdentity invalid{};
			const auto rejected = queue.Enqueue(Destination(invalid, 41U, 4.0));
			context.Expect(rejected.state == NavigationAgentReceiptState::Rejected
				&& queue.Drain().empty(),
				"An invalid generation-qualified identity must reject without mutating pending command storage.");
		});

		registry.Add("Navigation.AgentLifecycle.ReceiptExpirationDoesNotAliasNewCommands", [](TestContext& context) {
			NavigationAgentCommandQueue queue(4U);
			const NavigationAgentIdentity identity = Identity();
			const auto receipt = queue.Enqueue(Destination(identity, 51U, 4.0));
			queue.Drain();
			context.Expect(queue.ExpireReceipts(1U) == 1U,
				"Receipt retention must expose bounded expiration rather than growing without limit.");
			const auto expired = queue.ReadReceipt(identity, receipt.correlationId);
			context.Expect(expired.has_value() && expired->state == NavigationAgentReceiptState::Expired
				&& expired->outcome == NavigationOutcome::Expired && expired->reason == NavigationReason::Expired,
				"An expired receipt must remain distinguishable from a future command result.");
		});

		registry.Add("Navigation.AgentLifecycle.StoreReleasesAndReusesGenerationQualifiedSlots", [](TestContext& context) {
			NavigationAgentStore store(1U);
			const NavigationAgentIdentity prototype = Identity();
			const auto first = store.Create(prototype, Record(prototype));
			context.Expect(first.Succeeded() && store.AliveCount() == 1U,
				"A bounded store must admit one valid runtime record and report ownership.");
			context.Expect(store.Release(first.identity) && store.AliveCount() == 0U,
				"Releasing an agent must reclaim its runtime slot.");
			context.Expect(store.Snapshot().empty(),
				"A released agent must not remain in copied runtime snapshots.");
			const auto stale = store.Read(first.identity);
			const auto second = store.Create(prototype, Record(prototype));
			context.Expect(stale.status != NavigationHandleStatus::Alive && second.Succeeded()
				&& second.identity.runtimeSlot == first.identity.runtimeSlot
				&& second.identity.runtimeSlotGeneration != first.identity.runtimeSlotGeneration,
				"Slot reuse must advance generation so a removed identity cannot alias its replacement.");
		});

		registry.Add("Navigation.AgentLifecycle.StoreRestartNeverRevivesCachedIdentity", [](TestContext& context) {
			const NavigationAgentIdentity prototype = Identity();
			NavigationAgentIdentity beforeRestart{};
			{
				// A system shutdown destroys this bounded store. Keep only the copied
				// identity a managed wrapper would retain across that lifecycle edge.
				NavigationAgentStore firstStore(1U);
				const auto first = firstStore.Create(prototype, Record(prototype));
				beforeRestart = first.identity;
				context.Expect(first.Succeeded(), "The first runtime incarnation must admit its valid attachment.");
			}

			NavigationAgentStore restartedStore(1U);
			const auto afterRestart = restartedStore.Create(prototype, Record(prototype));
			context.Expect(afterRestart.Succeeded()
				&& afterRestart.identity.runtimeSlot == beforeRestart.runtimeSlot
				&& afterRestart.identity.runtimeSlotGeneration != beforeRestart.runtimeSlotGeneration
				&& restartedStore.Read(beforeRestart).status != NavigationHandleStatus::Alive,
				"A new system/store incarnation must reject a cached identity even when world and component generations match.");
		});

		registry.Add("Navigation.AgentLifecycle.StoreCapacityAndWorldRevocationAreBounded", [](TestContext& context) {
			NavigationAgentStore store(1U);
			const NavigationAgentIdentity firstIdentity = Identity();
			const auto first = store.Create(firstIdentity, Record(firstIdentity));
			const NavigationAgentIdentity secondIdentity = Identity(2U);
			const auto rejected = store.Create(secondIdentity, Record(secondIdentity));
			context.Expect(first.Succeeded() && rejected.status == NavigationHandleStatus::CapacityExceeded,
				"A full agent store must reject admission without replacing its existing record.");
			context.Expect(store.RevokeWorld(firstIdentity.world) == 1U && store.AliveCount() == 0U,
				"World teardown must revoke all matching runtime records atomically from the bounded store.");
		});

		registry.Add("Navigation.AgentLifecycle.StoreUpdatePreservesGenerationQualifiedOwnership", [](TestContext& context) {
			NavigationAgentStore store(1U);
			const NavigationAgentIdentity identity = Identity();
			const auto created = store.Create(identity, Record(identity));
			const bool updated = store.Update(created.identity, [](NavigationAgentRuntimeRecord& record) {
				record.state = NavigationAgentState::Stopped;
				record.intentSequence = 9U;
			});
			const auto read = store.Read(created.identity);
			context.Expect(updated && read.status == NavigationHandleStatus::Alive && read.record.has_value()
				&& read.record->state == NavigationAgentState::Stopped && read.record->intentSequence == 9U,
				"Store updates must mutate only the generation-qualified record selected by the caller.");
		});

		registry.Add("Navigation.AgentLifecycle.QueueClearSupersedesPendingCommands", [](TestContext& context) {
			NavigationAgentCommandQueue queue(4U);
			const NavigationAgentIdentity identity = Identity();
			const auto receipt = queue.Enqueue(Destination(identity, 71U, 4.0));
			queue.Clear();
			const auto cleared = queue.ReadReceipt(identity, receipt.correlationId);
			context.Expect(queue.PendingCount() == 0U && cleared.has_value()
				&& cleared->state == NavigationAgentReceiptState::Superseded,
				"Clearing a queue must cancel pending intent with a terminal receipt before teardown.");
		});

		registry.Add("Navigation.AgentLifecycle.DuplicateCorrelationPreservesOriginalReceipt", [](TestContext& context) {
			NavigationAgentCommandQueue queue(4U);
			const NavigationAgentIdentity identity = Identity();
			const auto original = queue.Enqueue(Destination(identity, 81U, 4.0));
			const auto duplicate = queue.Enqueue(Destination(identity, 81U, 8.0));
			queue.CompleteReceipt({identity, NavigationAgentReceiptState::Applied,
				NavigationOutcome::Complete, NavigationReason::None, original.correlationId, 1U});
			const auto read = queue.ReadReceipt(identity, original.correlationId);
			context.Expect(original.state == NavigationAgentReceiptState::Queued
				&& duplicate.state == NavigationAgentReceiptState::Rejected
				&& read.has_value() && read->state == NavigationAgentReceiptState::Applied
				&& read->admittedSequence == 1U,
				"A duplicate correlation must not hide the original receipt or prevent its terminal update.");
		});

	}
}
