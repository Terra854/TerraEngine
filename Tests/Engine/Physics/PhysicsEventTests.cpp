// PhysicsEventTests.cpp - contract tests for deterministic, bounded events.

#include "../TestHarness.h"

#include "Physics/PhysicsEvents.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	namespace {
		PhysicsEvent MakeEvent(PhysicsEventType type, std::uint32_t firstSlot, std::uint32_t secondSlot = 0U) {
			PhysicsEvent event;
			event.type = type;
			event.world = {{9U}, 3U};
			event.tick = 12U;
			event.first.objectId[15] = static_cast<std::uint8_t>(firstSlot);
			event.first.objectGeneration = 1U;
			event.first.componentGeneration = 2U;
			event.first.colliderRecordId[15] = static_cast<std::uint8_t>(firstSlot);
			event.first.recordGeneration = 3U;
			event.first.body = {{9U}, 3U, firstSlot, 1U};
			event.first.collisionLayer = 1U;
			event.hasSecondParticipant = secondSlot != 0U;
			if(event.hasSecondParticipant) {
				event.second.objectId[15] = static_cast<std::uint8_t>(secondSlot);
				event.second.objectGeneration = 1U;
				event.second.componentGeneration = 2U;
				event.second.colliderRecordId[15] = static_cast<std::uint8_t>(secondSlot);
				event.second.recordGeneration = 3U;
				event.second.body = {{9U}, 3U, secondSlot, 1U};
				event.second.collisionLayer = 2U;
			}
			event.normal = {1.0F, 0.0F, 0.0F};
			return event;
		}

		PhysicsEventPacket BuildPacket(std::vector<PhysicsEvent> events, PhysicsEventSubscriptionPolicy policy = {}) {
			PhysicsEventThreadLocalCollector collector(32U);
			for(PhysicsEvent& event : events) collector.Record(std::move(event));
			PhysicsEventSubscriptionRegistry subscriptions;
			subscriptions.Subscribe(std::move(policy));
			const PhysicsEventThreadLocalCollector* collectorPointer = &collector;
			return PhysicsEventPacketBuilder::MergeCollectors({{9U}, 3U}, 12U, {&collectorPointer, 1U}, subscriptions.Snapshot());
		}
	}

	void RegisterPhysicsEventTests(TestRegistry& registry) {
		registry.Add("Physics.Events.CanonicalPairsAndTransitions", [](TestContext& context) {
			const PhysicsEventPacket packet = BuildPacket({
				MakeEvent(PhysicsEventType::CollisionExit, 9U, 2U),
				MakeEvent(PhysicsEventType::CollisionStay, 9U, 2U),
				MakeEvent(PhysicsEventType::CollisionEnter, 9U, 2U),
				MakeEvent(PhysicsEventType::Contact, 9U, 2U),
				MakeEvent(PhysicsEventType::TriggerEnter, 9U, 2U),
				MakeEvent(PhysicsEventType::TriggerStay, 9U, 2U),
				MakeEvent(PhysicsEventType::TriggerExit, 9U, 2U)
			});
			context.Expect(packet.events->size() == 7U, "Each collision and trigger transition must survive packet construction.");
			context.Expect(packet.events->front().type == PhysicsEventType::CollisionEnter, "The engine ordering key must publish pair entry before later pair notifications.");
			for(const PhysicsEvent& event : *packet.events) {
				context.Expect(event.first.body.slotIndex < event.second.body.slotIndex, "Pair records must be canonical regardless of callback participant order.");
			}
		});

		registry.Add("Physics.Events.ContactActivationSleepAndDeterministicMerge", [](TestContext& context) {
			PhysicsEventThreadLocalCollector left(8U);
			PhysicsEventThreadLocalCollector right(8U);
			left.Record(MakeEvent(PhysicsEventType::Slept, 8U));
			left.Record(MakeEvent(PhysicsEventType::Contact, 8U, 3U));
			right.Record(MakeEvent(PhysicsEventType::Activated, 4U));
			const PhysicsEventThreadLocalCollector* collectors[] = {&right, &left};
			PhysicsEventSubscriptionRegistry subscriptions;
			subscriptions.Subscribe({});
			const PhysicsEventPacket first = PhysicsEventPacketBuilder::MergeCollectors({{9U}, 3U}, 12U, collectors, subscriptions.Snapshot());
			std::reverse(std::begin(collectors), std::end(collectors));
			const PhysicsEventPacket second = PhysicsEventPacketBuilder::MergeCollectors({{9U}, 3U}, 12U, collectors, subscriptions.Snapshot());
			context.Expect(first.events->size() == 3U && second.events->size() == 3U, "Activation, sleep, and contact records must be publishable.");
			for(std::size_t index = 0U; index < first.events->size(); ++index) {
				context.Expect((*first.events)[index].type == (*second.events)[index].type
					&& (*first.events)[index].first.body == (*second.events)[index].first.body,
					"Collector arrival order must not affect deterministic packet ordering.");
			}
		});

		registry.Add("Physics.Events.SubscriptionFiltersAndOverflowDiagnostics", [](TestContext& context) {
			PhysicsEventThreadLocalCollector collector(2U);
			collector.Record(MakeEvent(PhysicsEventType::CollisionEnter, 1U, 2U));
			collector.Record(MakeEvent(PhysicsEventType::TriggerEnter, 1U, 2U));
			collector.Record(MakeEvent(PhysicsEventType::Contact, 1U, 2U));
			PhysicsEventSubscriptionPolicy policy;
			policy.categories = PhysicsEventCategory::Collision;
			policy.collisionLayerMask = (1U << 1U) | (1U << 2U);
			PhysicsEventSubscriptionRegistry subscriptions;
			subscriptions.Subscribe(policy);
			const PhysicsEventThreadLocalCollector* collectorPointer = &collector;
			const PhysicsEventPacket packet = PhysicsEventPacketBuilder::MergeCollectors(
				{{9U}, 3U}, 12U, {&collectorPointer, 1U}, subscriptions.Snapshot(), {1U, 2U, 3U, 2U});
			context.Expect(packet.events->size() == 1U && packet.events->front().type == PhysicsEventType::CollisionEnter,
				"Category and layer filters must hide unrelated notifications.");
			context.Expect(packet.filteredCount == 1U, "Filtered event traffic must be diagnosed without entering the packet.");
			context.Expect(packet.droppedCount == 1U && packet.overflowed, "Collector and packet overflow must remain bounded and observable.");

			PhysicsEvent sensor = MakeEvent(PhysicsEventType::TriggerEnter, 3U, 4U);
			PhysicsEventSubscriptionPolicy noSensors;
			noSensors.includeSensors = false;
			noSensors.includeObjectIds.push_back(sensor.first.objectId);
			const PhysicsEventPacket hiddenSensor = BuildPacket({sensor}, noSensors);
			context.Expect(hiddenSensor.events->empty() && hiddenSensor.filteredCount == 1U,
				"Sensor policy and object inclusion filters must be evaluated from immutable event data.");

			PhysicsEventThreadLocalCollector packetOverflowCollector(8U);
			packetOverflowCollector.Record(MakeEvent(PhysicsEventType::CollisionEnter, 1U, 2U));
			packetOverflowCollector.Record(MakeEvent(PhysicsEventType::CollisionStay, 1U, 2U));
			packetOverflowCollector.Record(MakeEvent(PhysicsEventType::CollisionExit, 1U, 2U));
			const PhysicsEventThreadLocalCollector* overflowCollector = &packetOverflowCollector;
			const PhysicsEventPacket packetOverflow = PhysicsEventPacketBuilder::MergeCollectors(
				{{9U}, 3U}, 12U, {&overflowCollector, 1U}, subscriptions.Snapshot(), {2U, 8U, 3U, 2U});
			context.Expect(packetOverflow.events->size() == 2U && packetOverflow.droppedCount == 1U && packetOverflow.overflowed,
				"Packet capacity must bound merged traffic even when an individual collector has spare capacity.");
		});

		registry.Add("Physics.Events.RetentionRejectsExpiredAndStaleGenerations", [](TestContext& context) {
			PhysicsEventHistory history({16U, 16U, 2U, 2U});
			context.Expect(history.Publish(BuildPacket({MakeEvent(PhysicsEventType::CollisionEnter, 1U, 2U)})), "A valid immutable packet should enter bounded retention.");
			context.Expect(history.Acquire({{9U}, 3U}, 12U, 13U) != nullptr, "Packets remain readable through their configured retention interval.");
			context.Expect(history.Acquire({{9U}, 4U}, 12U, 12U) == nullptr, "A recreated world generation must reject old events.");
			context.Expect(history.Acquire({{9U}, 3U}, 12U, 15U) == nullptr, "Expired packets must not be exposed to consumers.");
			history.Expire({{9U}, 3U}, 15U);
			context.Expect(history.Size() == 0U, "Expiry must release retained packet ownership.");
		});

		registry.Add("Physics.Events.RejectsBodyFromStaleWorldGeneration", [](TestContext& context) {
			PhysicsEvent stale = MakeEvent(PhysicsEventType::CollisionEnter, 1U, 2U);
			stale.second.body.worldGeneration = 4U;
			PhysicsEventThreadLocalCollector collector;
			context.Expect(!collector.Record(stale), "A callback record with a stale participant world generation must be rejected before publication.");
			context.Expect(collector.DroppedCount() == 1U, "Stale generation rejection must remain observable in collector diagnostics.");
		});
	}
}
