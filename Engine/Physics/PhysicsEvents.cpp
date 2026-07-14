// PhysicsEvents.cpp - deterministic event collection, filtering, and retention.

#include "PhysicsEvents.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace TerraEngine::Physics {
	namespace {
		bool StableUuidLess(const TerraEngine::UUID& left, const TerraEngine::UUID& right) noexcept {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		bool SameUuid(const TerraEngine::UUID& value, const std::vector<TerraEngine::UUID>& values) noexcept {
			return std::find(values.begin(), values.end(), value) != values.end();
		}

		bool ParticipantLess(const PhysicsEventParticipant& left, const PhysicsEventParticipant& right) noexcept {
			if(left.body.worldId != right.body.worldId) return left.body.worldId < right.body.worldId;
			if(left.body.worldGeneration != right.body.worldGeneration) return left.body.worldGeneration < right.body.worldGeneration;
			if(left.body.slotIndex != right.body.slotIndex) return left.body.slotIndex < right.body.slotIndex;
			if(left.body.slotGeneration != right.body.slotGeneration) return left.body.slotGeneration < right.body.slotGeneration;
			if(left.objectId != right.objectId) return StableUuidLess(left.objectId, right.objectId);
			if(left.objectGeneration != right.objectGeneration) return left.objectGeneration < right.objectGeneration;
			if(left.componentGeneration != right.componentGeneration) return left.componentGeneration < right.componentGeneration;
			if(left.colliderRecordId != right.colliderRecordId) return StableUuidLess(left.colliderRecordId, right.colliderRecordId);
			return std::tie(left.recordGeneration, left.subshapeId, left.collisionLayer)
				< std::tie(right.recordGeneration, right.subshapeId, right.collisionLayer);
		}

		bool AnySubscriptionMatches(const PhysicsEvent& event, const PhysicsEventSubscriptionSnapshot& snapshot) {
			return std::any_of(snapshot.subscriptions.begin(), snapshot.subscriptions.end(), [&event](const PhysicsEventSubscription& subscription) {
				return subscription.policy.Matches(event);
			});
		}

		std::size_t EventIndex(PhysicsEventType type) noexcept {
			return static_cast<std::size_t>(type);
		}

		bool IsValidConstraintBreak(const PhysicsEvent& event) noexcept {
			return !event.hasSecondParticipant
				&& event.constraint.IsValid()
				&& event.constraint.worldId == event.world.worldId
				&& event.constraint.worldGeneration == event.world.worldGeneration
				&& event.constraintOwnerObjectId != TerraEngine::EMPTY_UUID
				&& event.constraintOwnerObjectGeneration != 0U
				&& event.constraintComponentGeneration != 0U
				&& event.constraintRecordId != TerraEngine::EMPTY_UUID
				&& event.constraintRecordGeneration != 0U
				&& event.constraintResetGeneration != 0U
				&& std::isfinite(event.constraintBreakForce) && event.constraintBreakForce >= 0.0F
				&& std::isfinite(event.constraintBreakTorque) && event.constraintBreakTorque >= 0.0F
				// An unbreakable unused axis is represented by infinity, so threshold
				// validation rejects NaN/negative values but accepts that sentinel.
				&& !std::isnan(event.constraintForceThreshold) && event.constraintForceThreshold >= 0.0F
				&& !std::isnan(event.constraintTorqueThreshold) && event.constraintTorqueThreshold >= 0.0F;
		}
	}

	bool PhysicsEventParticipant::IsValid() const noexcept {
		return objectId != TerraEngine::EMPTY_UUID
			&& objectGeneration != 0U
			&& componentGeneration != 0U
			&& colliderRecordId != TerraEngine::EMPTY_UUID
			&& recordGeneration != 0U
			&& body.IsValid();
	}

	bool PhysicsEvent::IsFinite() const noexcept {
		if(!world.IsValid() || tick == 0U) return false;
		if(type == PhysicsEventType::ConstraintBroken) {
			return IsValidConstraintBreak(*this)
				&& point.IsFinite() && normal.IsFinite() && relativeVelocity.IsFinite()
				&& std::isfinite(penetration) && std::isfinite(impulse);
		}
		return first.IsValid()
			&& first.body.worldId == world.worldId
			&& first.body.worldGeneration == world.worldGeneration
			&& (!hasSecondParticipant || second.IsValid())
			&& (!hasSecondParticipant || (second.body.worldId == world.worldId
				&& second.body.worldGeneration == world.worldGeneration))
			&& point.IsFinite()
			&& normal.IsFinite()
			&& relativeVelocity.IsFinite()
			&& std::isfinite(penetration)
			&& std::isfinite(impulse);
	}

	bool PhysicsEventSubscriptionPolicy::Matches(const PhysicsEvent& event) const {
		if(!HasAny(categories, PhysicsEventCategoryFor(event.type))) return false;
		if((collisionLayerMask & (1U << event.first.collisionLayer)) == 0U) return false;
		if(event.hasSecondParticipant && (collisionLayerMask & (1U << event.second.collisionLayer)) == 0U) return false;
		if(!includeObjectIds.empty()
			&& !SameUuid(event.first.objectId, includeObjectIds)
			&& (!event.hasSecondParticipant || !SameUuid(event.second.objectId, includeObjectIds))) return false;
		if(SameUuid(event.first.objectId, excludeObjectIds)
			|| (event.hasSecondParticipant && SameUuid(event.second.objectId, excludeObjectIds))) return false;
		const bool triggerEvent = PhysicsEventCategoryFor(event.type) == PhysicsEventCategory::Trigger;
		return includeSensors || !triggerEvent;
	}

	bool PhysicsEventPacket::IsValidFor(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const noexcept {
		return world == activeWorld
			&& world.IsValid()
			&& tick != 0U
			&& expiryTick >= tick
			&& currentTick <= expiryTick
			&& events != nullptr;
	}

	PhysicsEventThreadLocalCollector::PhysicsEventThreadLocalCollector(std::size_t maximumEvents)
		: maximumEvents(std::max<std::size_t>(1U, maximumEvents)) {
		events.reserve(this->maximumEvents);
	}

	bool PhysicsEventThreadLocalCollector::Record(PhysicsEvent event) {
		if(!event.IsFinite() || events.size() >= maximumEvents) {
			++droppedCount;
			return false;
		}
		CanonicalizePhysicsEventParticipants(event);
		events.push_back(std::move(event));
		return true;
	}

	void PhysicsEventThreadLocalCollector::Clear() {
		events.clear();
		droppedCount = 0U;
	}

	const std::vector<PhysicsEvent>& PhysicsEventThreadLocalCollector::Events() const noexcept { return events; }
	std::uint64_t PhysicsEventThreadLocalCollector::DroppedCount() const noexcept { return droppedCount; }

	PhysicsEventSubscription PhysicsEventSubscriptionRegistry::Subscribe(PhysicsEventSubscriptionPolicy policy) {
		std::lock_guard lock(mutex);
		PhysicsEventSubscription result;
		result.id = nextId++;
		result.generation = generation++;
		result.policy = std::move(policy);
		subscriptions.push_back(result);
		return result;
	}

	bool PhysicsEventSubscriptionRegistry::Unsubscribe(std::uint64_t id, PhysicsGeneration expectedGeneration) {
		std::lock_guard lock(mutex);
		const auto iterator = std::find_if(subscriptions.begin(), subscriptions.end(), [id, expectedGeneration](const PhysicsEventSubscription& subscription) {
			return subscription.id == id && subscription.generation == expectedGeneration;
		});
		if(iterator == subscriptions.end()) return false;
		subscriptions.erase(iterator);
		++generation;
		return true;
	}

	PhysicsEventSubscriptionSnapshot PhysicsEventSubscriptionRegistry::Snapshot() const {
		std::lock_guard lock(mutex);
		return {generation, subscriptions};
	}

	PhysicsEventPacket PhysicsEventPacketBuilder::MergeCollectors(
		PhysicsWorldHandle world,
		PhysicsTick tick,
		std::span<const PhysicsEventThreadLocalCollector* const> collectors,
		const PhysicsEventSubscriptionSnapshot& subscriptions,
		PhysicsEventPacketLimits limits,
		bool applySubscriptionFilters)
	{
		PhysicsEventPacket packet;
		packet.world = world;
		packet.tick = tick;
		packet.expiryTick = tick > std::numeric_limits<PhysicsTick>::max() - limits.retainedTicks
			? std::numeric_limits<PhysicsTick>::max() : tick + limits.retainedTicks;
		packet.subscriptionGeneration = subscriptions.generation;
		limits.maximumEventsPerPacket = std::max<std::size_t>(1U, limits.maximumEventsPerPacket);
		limits.maximumEventsPerCollector = std::max<std::size_t>(1U, limits.maximumEventsPerCollector);
		std::vector<PhysicsEvent> merged;
		merged.reserve(limits.maximumEventsPerPacket);

		for(const PhysicsEventThreadLocalCollector* collector : collectors) {
			if(collector == nullptr) continue;
			packet.droppedCount += collector->DroppedCount();
			std::size_t collectedCount = 0U;
			for(const PhysicsEvent& source : collector->Events()) {
				if(collectedCount++ == limits.maximumEventsPerCollector) {
					++packet.droppedCount;
					packet.overflowed = true;
					continue;
				}
				if(source.world != world || source.tick != tick || !source.IsFinite()) {
					++packet.droppedCount;
					continue;
				}
				if(applySubscriptionFilters && !AnySubscriptionMatches(source, subscriptions)) {
					++packet.filteredCount;
					continue;
				}
				if(merged.size() == limits.maximumEventsPerPacket) {
					++packet.droppedCount;
					packet.overflowed = true;
					continue;
				}
				merged.push_back(source);
			}
		}

		std::sort(merged.begin(), merged.end(), PhysicsEventLess);
		for(const PhysicsEvent& event : merged) ++packet.countsByType[EventIndex(event.type)];
		packet.overflowed = packet.overflowed || packet.droppedCount != 0U;
		packet.events = std::make_shared<const std::vector<PhysicsEvent>>(std::move(merged));
		return packet;
	}

	PhysicsEventHistory::PhysicsEventHistory(PhysicsEventPacketLimits limits)
		: limits(limits) {
		this->limits.maximumRetainedPackets = std::max<std::size_t>(1U, this->limits.maximumRetainedPackets);
	}

	bool PhysicsEventHistory::Publish(PhysicsEventPacket packet) {
		if(!packet.world.IsValid() || packet.tick == 0U || packet.events == nullptr) return false;
		std::lock_guard lock(mutex);
		packets.erase(std::remove_if(packets.begin(), packets.end(), [&packet](const auto& existing) {
			return existing->world == packet.world && existing->tick == packet.tick;
		}), packets.end());
		packets.push_back(std::make_shared<const PhysicsEventPacket>(std::move(packet)));
		std::sort(packets.begin(), packets.end(), [](const auto& left, const auto& right) {
			return left->tick < right->tick;
		});
		while(packets.size() > limits.maximumRetainedPackets) packets.erase(packets.begin());
		return true;
	}

	std::shared_ptr<const PhysicsEventPacket> PhysicsEventHistory::Acquire(
		PhysicsWorldHandle activeWorld, PhysicsTick requestedTick, PhysicsTick currentTick) const
	{
		std::lock_guard lock(mutex);
		const auto iterator = std::find_if(packets.begin(), packets.end(), [=](const auto& packet) {
			return packet->tick == requestedTick && packet->IsValidFor(activeWorld, currentTick);
		});
		return iterator == packets.end() ? nullptr : *iterator;
	}

	void PhysicsEventHistory::Expire(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) {
		std::lock_guard lock(mutex);
		packets.erase(std::remove_if(packets.begin(), packets.end(), [=](const auto& packet) {
			return packet->world != activeWorld || currentTick > packet->expiryTick;
		}), packets.end());
	}

	std::size_t PhysicsEventHistory::Size() const { std::lock_guard lock(mutex); return packets.size(); }

	PhysicsEventCategory PhysicsEventCategoryFor(PhysicsEventType type) noexcept {
		switch(type) {
		case PhysicsEventType::CollisionEnter: case PhysicsEventType::CollisionStay: case PhysicsEventType::CollisionExit: return PhysicsEventCategory::Collision;
		case PhysicsEventType::Contact: return PhysicsEventCategory::Contact;
		case PhysicsEventType::TriggerEnter: case PhysicsEventType::TriggerStay: case PhysicsEventType::TriggerExit: return PhysicsEventCategory::Trigger;
		case PhysicsEventType::Activated: return PhysicsEventCategory::Activation;
		case PhysicsEventType::Slept: return PhysicsEventCategory::Sleep;
		case PhysicsEventType::ConstraintBroken: return PhysicsEventCategory::ConstraintBreak;
		}
		return PhysicsEventCategory::None;
	}

	std::uint8_t PhysicsEventTypePriority(PhysicsEventType type) noexcept {
		switch(type) {
		case PhysicsEventType::CollisionEnter: return 0U;
		case PhysicsEventType::TriggerEnter: return 1U;
		case PhysicsEventType::Contact: return 2U;
		case PhysicsEventType::CollisionStay: return 3U;
		case PhysicsEventType::TriggerStay: return 4U;
		case PhysicsEventType::Activated: return 5U;
		case PhysicsEventType::Slept: return 6U;
		case PhysicsEventType::CollisionExit: return 7U;
		case PhysicsEventType::TriggerExit: return 8U;
		case PhysicsEventType::ConstraintBroken: return 9U;
		}
		return std::numeric_limits<std::uint8_t>::max();
	}

	void CanonicalizePhysicsEventParticipants(PhysicsEvent& event) noexcept {
		if(event.hasSecondParticipant && ParticipantLess(event.second, event.first)) {
			std::swap(event.first, event.second);
			std::swap(event.firstMaterialId, event.secondMaterialId);
			std::swap(event.firstSurfaceTag, event.secondSurfaceTag);
			// A normal points from first to second, so the canonical swap reverses it.
			event.normal.x = -event.normal.x;
			event.normal.y = -event.normal.y;
			event.normal.z = -event.normal.z;
			event.relativeVelocity.x = -event.relativeVelocity.x;
			event.relativeVelocity.y = -event.relativeVelocity.y;
			event.relativeVelocity.z = -event.relativeVelocity.z;
		}
	}

	bool PhysicsEventLess(const PhysicsEvent& left, const PhysicsEvent& right) noexcept {
		if(left.tick != right.tick) return left.tick < right.tick;
		if(PhysicsEventTypePriority(left.type) != PhysicsEventTypePriority(right.type)) return PhysicsEventTypePriority(left.type) < PhysicsEventTypePriority(right.type);
		if(ParticipantLess(left.first, right.first)) return true;
		if(ParticipantLess(right.first, left.first)) return false;
		if(left.hasSecondParticipant != right.hasSecondParticipant) return left.hasSecondParticipant;
		if(left.hasSecondParticipant && ParticipantLess(left.second, right.second)) return true;
		if(left.hasSecondParticipant && ParticipantLess(right.second, left.second)) return false;
		if(left.constraint != right.constraint) return left.constraint < right.constraint;
		if(left.constraintRecordId != right.constraintRecordId) return StableUuidLess(left.constraintRecordId, right.constraintRecordId);
		return std::tie(left.constraintResetGeneration, left.first.subshapeId, left.second.subshapeId,
			left.contactIndex, left.causalCommandSequence)
			< std::tie(right.constraintResetGeneration, right.first.subshapeId, right.second.subshapeId,
				right.contactIndex, right.causalCommandSequence);
	}

	std::string_view ToString(PhysicsEventType type) noexcept {
		switch(type) {
		case PhysicsEventType::CollisionEnter: return "CollisionEnter";
		case PhysicsEventType::CollisionStay: return "CollisionStay";
		case PhysicsEventType::CollisionExit: return "CollisionExit";
		case PhysicsEventType::Contact: return "Contact";
		case PhysicsEventType::TriggerEnter: return "TriggerEnter";
		case PhysicsEventType::TriggerStay: return "TriggerStay";
		case PhysicsEventType::TriggerExit: return "TriggerExit";
		case PhysicsEventType::Activated: return "Activated";
		case PhysicsEventType::Slept: return "Slept";
		case PhysicsEventType::ConstraintBroken: return "ConstraintBroken";
		}
		return "Unknown";
	}
}
