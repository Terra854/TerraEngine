// PhysicsEventTests.cpp - contract tests for deterministic, bounded events.

#include "../TestHarness.h"

#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
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

		// A compact deterministic fingerprint lets tests compare retained records
		// without coupling to vector allocation addresses or callback arrival order.
		std::uint64_t EventFingerprint(const PhysicsEventPacket& packet) {
			std::uint64_t fingerprint = 1469598103934665603ULL;
			for(const PhysicsEvent& event : *packet.events) {
				fingerprint ^= static_cast<std::uint64_t>(event.type);
				fingerprint *= 1099511628211ULL;
				fingerprint ^= event.first.body.slotIndex;
				fingerprint *= 1099511628211ULL;
				fingerprint ^= event.hasSecondParticipant ? event.second.body.slotIndex : 0U;
				fingerprint *= 1099511628211ULL;
			}
			return fingerprint;
		}

		PhysicsEvent RebindToWorld(PhysicsEvent event, PhysicsWorldHandle world) {
			event.world = world;
			event.first.body.worldId = world.worldId;
			event.first.body.worldGeneration = world.worldGeneration;
			if(event.hasSecondParticipant) {
				event.second.body.worldId = world.worldId;
				event.second.body.worldGeneration = world.worldGeneration;
			}
			return event;
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

		registry.Add("Physics.Events.OverCapRetentionUsesOneCanonicalOrder", [](TestContext& context) {
			// The two collectors deliberately receive inverse portions of the canonical
			// sequence. A bounded packet must retain the same earliest records either
			// way around; callback shard scheduling is not gameplay-observable.
			PhysicsEventThreadLocalCollector early(8U);
			PhysicsEventThreadLocalCollector late(8U);
			early.Record(MakeEvent(PhysicsEventType::CollisionEnter, 1U, 2U));
			early.Record(MakeEvent(PhysicsEventType::Contact, 1U, 2U));
			late.Record(MakeEvent(PhysicsEventType::CollisionExit, 1U, 2U));
			const PhysicsEventThreadLocalCollector* firstOrder[] = {&late, &early};
			const PhysicsEventThreadLocalCollector* secondOrder[] = {&early, &late};
			PhysicsEventSubscriptionRegistry subscriptions;
			subscriptions.Subscribe({});
			const PhysicsEventPacketLimits limits{2U, 8U, 3U, 2U};
			const PhysicsEventPacket first = PhysicsEventPacketBuilder::MergeCollectors(
				{{9U}, 3U}, 12U, firstOrder, subscriptions.Snapshot(), limits);
			const PhysicsEventPacket second = PhysicsEventPacketBuilder::MergeCollectors(
				{{9U}, 3U}, 12U, secondOrder, subscriptions.Snapshot(), limits);
			context.Expect(first.events->size() == 2U && second.events->size() == 2U
				&& first.droppedCount == 1U && second.droppedCount == 1U,
				"A bounded contact packet must retain two records and account for exactly one over-cap record.");
			context.Expect(EventFingerprint(first) == EventFingerprint(second)
				&& first.events->front().type == PhysicsEventType::CollisionEnter,
				"Over-cap retention must be selected from the canonical event order rather than collector arrival order.");
		});

		registry.Add("Physics.Events.FiltersBeforePacketCapAndSharesCanonicalWorldResult", [](TestContext& context) {
			PhysicsEventThreadLocalCollector collector(8U);
			collector.Record(MakeEvent(PhysicsEventType::Contact, 1U, 2U));
			collector.Record(MakeEvent(PhysicsEventType::CollisionEnter, 1U, 2U));
			collector.Record(MakeEvent(PhysicsEventType::CollisionStay, 1U, 2U));
			const PhysicsEventThreadLocalCollector* collectorPointer = &collector;
			PhysicsEventSubscriptionRegistry filteredSubscriptions;
			PhysicsEventSubscriptionPolicy collisionsOnly;
			collisionsOnly.categories = PhysicsEventCategory::Collision;
			filteredSubscriptions.Subscribe(collisionsOnly);
			const PhysicsEventPacket filtered = PhysicsEventPacketBuilder::MergeCollectors(
				{{9U}, 3U}, 12U, {&collectorPointer, 1U}, filteredSubscriptions.Snapshot(), {1U, 8U, 3U, 2U});
			context.Expect(filtered.events->size() == 1U && filtered.events->front().type == PhysicsEventType::CollisionEnter
				&& filtered.filteredCount == 1U && filtered.droppedCount == 1U,
				"Subscription filtering must occur before bounded retention so excluded contacts cannot consume gameplay capacity.");

			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The canonical event fixture requires an active isolated physics world.");
			world.SubscribeEvents({});
			const PhysicsWorldHandle active = world.Handle();
			const std::array<PhysicsEvent, 2U> events{
				RebindToWorld(MakeEvent(PhysicsEventType::CollisionStay, 4U, 5U), active),
				RebindToWorld(MakeEvent(PhysicsEventType::CollisionEnter, 4U, 5U), active)};
			const auto gameplay = world.PublishEvents(12U, events);
			const auto diagnostic = world.AcquireDiagnosticEventPacket(12U, 12U);
			context.Expect(gameplay != nullptr && diagnostic != nullptr
				&& gameplay->events == diagnostic->events
				&& gameplay->events->front().type == PhysicsEventType::CollisionEnter,
				"Gameplay and diagnostic consumers with identical visibility must share one sorted canonical event result.");
		});
	}
}
