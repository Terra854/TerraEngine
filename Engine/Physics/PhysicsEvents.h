// PhysicsEvents.h - provider-neutral, deterministic physics event publication.
//
// Physics callback threads write only into their own collector. A protected
// publication boundary later merges collectors, applies subscriptions, sorts the
// engine-owned records, and retains a bounded immutable packet history. This
// prevents callback timing or third-party types from becoming gameplay API.

#pragma once

#include "Core/Export.h"
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Physics {
	inline constexpr std::size_t kPhysicsEventTypeCount = 10U;

	// Event values are deliberately stable engine values rather than callback or
	// provider values, so serialized diagnostics and later managed bindings keep
	// the same meaning if the simulation implementation changes.
	enum class PhysicsEventType : std::uint8_t {
		CollisionEnter,
		CollisionStay,
		CollisionExit,
		Contact,
		TriggerEnter,
		TriggerStay,
		TriggerExit,
		Activated,
		Slept,
		ConstraintBroken
	};

	enum class PhysicsEventCategory : std::uint32_t {
		None = 0U,
		Collision = 1U << 0U,
		Contact = 1U << 1U,
		Trigger = 1U << 2U,
		Activation = 1U << 3U,
		Sleep = 1U << 4U,
		ConstraintBreak = 1U << 5U,
		All = (1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 3U) | (1U << 4U) | (1U << 5U)
	};

	constexpr PhysicsEventCategory operator|(PhysicsEventCategory left, PhysicsEventCategory right) noexcept {
		return static_cast<PhysicsEventCategory>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	constexpr PhysicsEventCategory operator&(PhysicsEventCategory left, PhysicsEventCategory right) noexcept {
		return static_cast<PhysicsEventCategory>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
	}

	constexpr bool HasAny(PhysicsEventCategory value, PhysicsEventCategory flags) noexcept {
		return static_cast<std::uint32_t>(value & flags) != 0U;
	}

	// This complete identity lets an event consumer reject a record that belongs
	// to an object, collider, or compact body slot which has since been reused.
	struct PhysicsEventParticipant {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration objectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
		TerraEngine::UUID colliderRecordId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration recordGeneration = 0U;
		PhysicsBodyHandle body;
		std::uint32_t subshapeId = 0U;
		std::uint8_t collisionLayer = 0U;

		[[nodiscard]] bool IsValid() const noexcept;
	};

	struct PhysicsEvent {
		PhysicsEventType type = PhysicsEventType::CollisionEnter;
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		PhysicsEventParticipant first;
		PhysicsEventParticipant second;
		// Unary activation, sleep, and break notifications leave second invalid.
		bool hasSecondParticipant = true;
		PhysicsVector3 point;
		PhysicsVector3 normal;
		PhysicsVector3 relativeVelocity;
		float penetration = 0.0F;
		float impulse = 0.0F;
		TerraEngine::UUID firstMaterialId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID secondMaterialId = TerraEngine::EMPTY_UUID;
		std::uint32_t firstSurfaceTag = 0U;
		std::uint32_t secondSurfaceTag = 0U;
		// Constraint breaks are unary records. These fields carry the stable
		// constraint identity and the force/torque derived from the provider's
		// per-step impulses, without requiring a collider participant or provider ID.
		PhysicsConstraintHandle constraint;
		TerraEngine::UUID constraintOwnerObjectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration constraintOwnerObjectGeneration = 0U;
		PhysicsGeneration constraintComponentGeneration = 0U;
		TerraEngine::UUID constraintRecordId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration constraintRecordGeneration = 0U;
		PhysicsGeneration constraintResetGeneration = 0U;
		float constraintBreakForce = 0.0F;
		float constraintBreakTorque = 0.0F;
		float constraintForceThreshold = 0.0F;
		float constraintTorqueThreshold = 0.0F;
		PhysicsSequence causalCommandSequence = 0U;
		std::uint32_t contactIndex = 0U;

		[[nodiscard]] bool IsFinite() const noexcept;
	};

	// A subscription contains data-only filters. It never contains a callback:
	// packet consumers decide when and where to run their own callback code.
	struct PhysicsEventSubscriptionPolicy {
		PhysicsEventCategory categories = PhysicsEventCategory::All;
		std::vector<TerraEngine::UUID> includeObjectIds;
		std::vector<TerraEngine::UUID> excludeObjectIds;
		std::uint32_t collisionLayerMask = 0xFFFFFFFFU;
		bool includeSensors = true;
		bool includeContactDetails = true;

		[[nodiscard]] bool Matches(const PhysicsEvent& event) const;
	};

	struct PhysicsEventSubscription {
		std::uint64_t id = 0U;
		PhysicsGeneration generation = 0U;
		PhysicsEventSubscriptionPolicy policy;
	};

	struct PhysicsEventSubscriptionSnapshot {
		PhysicsGeneration generation = 0U;
		std::vector<PhysicsEventSubscription> subscriptions;
	};

	struct PhysicsEventPacketLimits {
		std::size_t maximumEventsPerPacket = 4096U;
		std::size_t maximumEventsPerCollector = 1024U;
		std::size_t maximumRetainedPackets = 3U;
		PhysicsTick retainedTicks = 2U;
	};

	struct PhysicsEventPacket {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		PhysicsTick expiryTick = 0U;
		PhysicsGeneration subscriptionGeneration = 0U;
		std::shared_ptr<const std::vector<PhysicsEvent>> events;
		std::array<std::uint32_t, kPhysicsEventTypeCount> countsByType{};
		std::uint64_t filteredCount = 0U;
		std::uint64_t droppedCount = 0U;
		bool overflowed = false;

		[[nodiscard]] bool IsValidFor(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const noexcept;
	};

	// One instance is owned by exactly one callback thread for one simulation
	// interval. Record performs no locking; callers merge only after callbacks
	// have stopped, avoiding contention on high-contact ticks.
	class TERRAENGINE_API PhysicsEventThreadLocalCollector final {
	public:
		explicit PhysicsEventThreadLocalCollector(std::size_t maximumEvents = 1024U);

		bool Record(PhysicsEvent event);
		void Clear();
		[[nodiscard]] const std::vector<PhysicsEvent>& Events() const noexcept;
		[[nodiscard]] std::uint64_t DroppedCount() const noexcept;

	private:
		std::size_t maximumEvents;
		std::vector<PhysicsEvent> events;
		std::uint64_t droppedCount = 0U;
	};

	// The registry is intentionally data-only. A consumer obtains an immutable
	// snapshot at publication; subscribe/unsubscribe races cannot change which
	// filters were applied to an already constructed packet.
	class TERRAENGINE_API PhysicsEventSubscriptionRegistry final {
	public:
		PhysicsEventSubscription Subscribe(PhysicsEventSubscriptionPolicy policy);
		bool Unsubscribe(std::uint64_t id, PhysicsGeneration generation);
		[[nodiscard]] PhysicsEventSubscriptionSnapshot Snapshot() const;

	private:
		mutable std::mutex mutex;
		std::uint64_t nextId = 1U;
		PhysicsGeneration generation = 1U;
		std::vector<PhysicsEventSubscription> subscriptions;
	};

	// MergeCollectors is called only at the protected publication boundary. It
	// accepts a span of thread-confined collectors, so worker callback ordering is
	// removed by the canonical ordering key before any packet is observable.
	class TERRAENGINE_API PhysicsEventPacketBuilder final {
	public:
		[[nodiscard]] static PhysicsEventPacket MergeCollectors(
			PhysicsWorldHandle world,
			PhysicsTick tick,
			std::span<const PhysicsEventThreadLocalCollector* const> collectors,
			const PhysicsEventSubscriptionSnapshot& subscriptions,
			PhysicsEventPacketLimits limits = {});
	};

	// Retention owns only bounded packet references. Callers retain a shared
	// immutable packet, never mutable simulation-owned storage.
	class TERRAENGINE_API PhysicsEventHistory final {
	public:
		explicit PhysicsEventHistory(PhysicsEventPacketLimits limits = {});

		bool Publish(PhysicsEventPacket packet);
		[[nodiscard]] std::shared_ptr<const PhysicsEventPacket> Acquire(
			PhysicsWorldHandle activeWorld,
			PhysicsTick requestedTick,
			PhysicsTick currentTick) const;
		void Expire(PhysicsWorldHandle activeWorld, PhysicsTick currentTick);
		[[nodiscard]] std::size_t Size() const;

	private:
		PhysicsEventPacketLimits limits;
		mutable std::mutex mutex;
		std::vector<std::shared_ptr<const PhysicsEventPacket>> packets;
	};

	TERRAENGINE_API PhysicsEventCategory PhysicsEventCategoryFor(PhysicsEventType type) noexcept;
	TERRAENGINE_API std::uint8_t PhysicsEventTypePriority(PhysicsEventType type) noexcept;
	TERRAENGINE_API void CanonicalizePhysicsEventParticipants(PhysicsEvent& event) noexcept;
	TERRAENGINE_API bool PhysicsEventLess(const PhysicsEvent& left, const PhysicsEvent& right) noexcept;
	TERRAENGINE_API std::string_view ToString(PhysicsEventType type) noexcept;
}
