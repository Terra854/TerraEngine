// PhysicsTypes.h - provider-neutral identities and fixed-width physics values.
//
// Gameplay, editor, managed, and export code include this file without learning
// which third-party provider implements simulation. Provider IDs and pointers
// deliberately do not appear in any type below.

#pragma once

#include "Core/Export.h"
#include "Scene/WorldCoordinate.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <compare>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Physics {
	using PhysicsGeneration = std::uint32_t;
	using PhysicsSlotIndex = std::uint32_t;
	using PhysicsTick = std::uint64_t;
	using PhysicsSequence = std::uint64_t;
	using PhysicsCommandId = std::uint64_t;

	inline constexpr PhysicsSlotIndex kInvalidPhysicsSlot = std::numeric_limits<PhysicsSlotIndex>::max();

	// A logical world ID survives an atomic world replacement. Its generation,
	// carried by PhysicsWorldHandle, distinguishes each concrete runtime lifetime.
	struct PhysicsWorldId {
		std::uint64_t value = 0;

		constexpr bool IsValid() const noexcept {
			return value != 0;
		}

		auto operator<=>(const PhysicsWorldId&) const = default;
	};

	struct PhysicsWorldHandle {
		PhysicsWorldId worldId;
		PhysicsGeneration worldGeneration = 0;

		constexpr bool IsValid() const noexcept {
			return worldId.IsValid() && worldGeneration != 0;
		}

		auto operator<=>(const PhysicsWorldHandle&) const = default;
	};

	// Distinct tags prevent accidentally passing a body handle to a constraint or
	// character API even though all compact handles share the same binary shape.
	struct PhysicsBodyHandleTag;
	struct PhysicsConstraintHandleTag;
	struct PhysicsCharacterHandleTag;
	struct PhysicsShapeHandleTag;

	template<typename Tag>
	struct PhysicsHandle {
		PhysicsWorldId worldId;
		PhysicsGeneration worldGeneration = 0;
		PhysicsSlotIndex slotIndex = kInvalidPhysicsSlot;
		PhysicsGeneration slotGeneration = 0;

		constexpr bool IsValid() const noexcept {
			return worldId.IsValid()
				&& worldGeneration != 0
				&& slotIndex != kInvalidPhysicsSlot
				&& slotGeneration != 0;
		}

		auto operator<=>(const PhysicsHandle&) const = default;
	};

	using PhysicsBodyHandle = PhysicsHandle<PhysicsBodyHandleTag>;
	using PhysicsConstraintHandle = PhysicsHandle<PhysicsConstraintHandleTag>;
	using PhysicsCharacterHandle = PhysicsHandle<PhysicsCharacterHandleTag>;
	using PhysicsShapeHandle = PhysicsHandle<PhysicsShapeHandleTag>;

	struct PhysicsQueryHandle {
		PhysicsWorldId worldId;
		PhysicsGeneration worldGeneration = 0;
		PhysicsSlotIndex resultSlot = kInvalidPhysicsSlot;
		PhysicsGeneration resultGeneration = 0;
		PhysicsTick submittedTick = 0;
		PhysicsTick expiryTick = 0;

		constexpr bool IsValid() const noexcept {
			return worldId.IsValid()
				&& worldGeneration != 0
				&& resultSlot != kInvalidPhysicsSlot
				&& resultGeneration != 0;
		}

		auto operator<=>(const PhysicsQueryHandle&) const = default;
	};

	enum class PhysicsHandleStatus : std::uint8_t {
		Invalid,
		Alive,
		Destroyed,
		StaleSlot,
		StaleWorld
	};

	enum class PhysicsResultStatus : std::uint8_t {
		Success,
		Accepted,
		Rejected,
		Cancelled,
		StaleTarget,
		Expired,
		LimitExceeded,
		Conflict,
		ProviderFailure,
		InternalFailure
	};

	enum class PhysicsMotionType : std::uint8_t {
		Static,
		Dynamic,
		Kinematic
	};

	enum class PhysicsMotionQuality : std::uint8_t {
		Discrete,
		LinearCast
	};

	// A frame is an independently simulated local island. It is deliberately
	// distinct from PhysicsWorldId: a frame can be recreated at the same logical
	// identity while its generation prevents old poses, queries, and events from
	// being applied to its replacement.
	struct PhysicsFrameId {
		std::uint64_t value = 0U;

		constexpr bool IsValid() const noexcept { return value != 0U; }
		auto operator<=>(const PhysicsFrameId&) const = default;
	};

	inline constexpr PhysicsFrameId kPrimaryPhysicsFrameId{1U};

	struct PhysicsFrameHandle {
		PhysicsFrameId frameId;
		PhysicsGeneration frameGeneration = 0U;

		constexpr bool IsValid() const noexcept {
			return frameId.IsValid() && frameGeneration != 0U;
		}
		auto operator<=>(const PhysicsFrameHandle&) const = default;
	};

	// Provider extraction uses an explicit field mask so a transform publication
	// does not also pay for velocities, mass, and contact data it will not consume.
	// Identity and generation fields are mandatory and are therefore not mask bits.
	enum class PhysicsBodyStateFieldMask : std::uint32_t {
		None = 0U,
		Pose = 1U << 0U,
		LinearVelocity = 1U << 1U,
		AngularVelocity = 1U << 2U,
		Mass = 1U << 3U,
		ContactCount = 1U << 4U,
		Awake = 1U << 5U,
		Constraints = 1U << 6U,
		Characters = 1U << 7U,
		Events = 1U << 8U,
		BodyState = (1U << 6U) - 1U,
		All = (1U << 9U) - 1U
	};

	constexpr PhysicsBodyStateFieldMask operator|(
		PhysicsBodyStateFieldMask left,
		PhysicsBodyStateFieldMask right) noexcept {
		return static_cast<PhysicsBodyStateFieldMask>(
			static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	constexpr PhysicsBodyStateFieldMask operator&(
		PhysicsBodyStateFieldMask left,
		PhysicsBodyStateFieldMask right) noexcept {
		return static_cast<PhysicsBodyStateFieldMask>(
			static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
	}

	constexpr PhysicsBodyStateFieldMask& operator|=(
		PhysicsBodyStateFieldMask& left,
		PhysicsBodyStateFieldMask right) noexcept {
		left = left | right;
		return left;
	}

	constexpr bool HasPhysicsBodyStateFields(
		PhysicsBodyStateFieldMask value,
		PhysicsBodyStateFieldMask required) noexcept {
		return (value & required) == required;
	}

	// Change reasons survive provider conversion and let downstream consumers
	// distinguish a normal pose update from lifecycle or sleep-state transitions.
	enum class PhysicsBodyStateChangeMask : std::uint16_t {
		None = 0U,
		Created = 1U << 0U,
		Updated = 1U << 1U,
		Removed = 1U << 2U,
		Awakened = 1U << 3U,
		Slept = 1U << 4U,
		ExplicitlyRequested = 1U << 5U
	};

	constexpr PhysicsBodyStateChangeMask operator|(
		PhysicsBodyStateChangeMask left,
		PhysicsBodyStateChangeMask right) noexcept {
		return static_cast<PhysicsBodyStateChangeMask>(
			static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
	}

	constexpr PhysicsBodyStateChangeMask operator&(
		PhysicsBodyStateChangeMask left,
		PhysicsBodyStateChangeMask right) noexcept {
		return static_cast<PhysicsBodyStateChangeMask>(
			static_cast<std::uint16_t>(left) & static_cast<std::uint16_t>(right));
	}

	constexpr PhysicsBodyStateChangeMask& operator|=(
		PhysicsBodyStateChangeMask& left,
		PhysicsBodyStateChangeMask right) noexcept {
		left = left | right;
		return left;
	}

	enum class PhysicsForceMode : std::uint8_t {
		Force,
		Acceleration,
		Impulse,
		VelocityChange
	};

	enum class PhysicsWorldState : std::uint8_t {
		Absent,
		Creating,
		Active,
		Paused,
		Stopping,
		Draining,
		Failed,
		Destroyed
	};

	enum class PhysicsQueryKind : std::uint8_t {
		RayCast,
		ShapeCast,
		Overlap,
		Point
	};

	enum class PhysicsShapeKind : std::uint8_t {
		Box,
		Sphere,
		Capsule,
		Cylinder,
		TaperedCapsule,
		ConvexHull,
		Mesh,
		HeightField,
		Compound
	};

	// Durable authoring and provider character creation use the same typed
	// rotation policy without making the provider depend on ECS components.
	enum class PhysicsCharacterRotationPolicy : std::uint8_t {
		Free,
		AlignToUp,
		Locked
	};

	enum class PhysicsConstraintKind : std::uint8_t {
		Fixed,
		Point,
		Distance,
		Hinge,
		Slider,
		Cone,
		SwingTwist,
		SixDegreesOfFreedom,
		Gear
	};

	enum class PhysicsCombineMode : std::uint8_t {
		Average,
		Minimum,
		Multiply,
		Maximum
	};

	// Provider conversion happens explicitly in the backend. These compact math
	// values keep public ABI and serialized units independent of Jolt layouts.
	struct PhysicsVector3 {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;

		bool IsFinite() const noexcept;
		auto operator<=>(const PhysicsVector3&) const = default;
	};

	// Immutable terrain samples stay provider-neutral until a backend creates a
	// replacement static shape. Holes are one bit per 256 by 256 terrain cell.
	struct TerrainHeightFieldRecipe
	{
		static constexpr std::uint32_t kGridExtent = 257u;
		static constexpr std::size_t kSampleCount = static_cast<std::size_t>(kGridExtent) * kGridExtent;
		static constexpr std::size_t kHoleByteCount = (256u * 256u) / 8u;
		TerraEngine::UUID terrainAssetId = TerraEngine::EMPTY_UUID;
		std::int64_t areaX = 0;
		std::int64_t areaZ = 0;
		std::uint64_t authoredGeneration = 0u;
		std::uint64_t authoredHash = 0u;
		float sampleSpacingMetres = 1.0f;
		float heightDatumMetres = 0.0f;
		std::vector<std::int32_t> heightMillimetres;
		std::vector<std::uint8_t> holeBits;
		std::vector<std::uint16_t> materialIndices;
		[[nodiscard]] bool IsComplete() const noexcept
		{
			return terrainAssetId != TerraEngine::EMPTY_UUID && authoredGeneration != 0u && authoredHash != 0u
				&& std::isfinite(sampleSpacingMetres) && sampleSpacingMetres > 0.0f && std::isfinite(heightDatumMetres)
				&& heightMillimetres.size() == kSampleCount && holeBits.size() == kHoleByteCount
				&& (materialIndices.empty() || materialIndices.size() == 256u * 256u);
		}
	};

	struct PhysicsQuaternion {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
		float w = 1.0F;

		bool IsFinite() const noexcept;
		auto operator<=>(const PhysicsQuaternion&) const = default;
	};

	struct PhysicsPose {
		PhysicsVector3 position;
		PhysicsQuaternion rotation;

		bool IsFinite() const noexcept;
		auto operator<=>(const PhysicsPose&) const = default;
	};

	// Jolt continues to receive floats, but only after this descriptor has
	// localized a canonical position into its explicitly bounded island.
	struct PhysicsFrameDescriptor {
		PhysicsFrameHandle frame;
		PhysicsWorldHandle world;
		TerraEngine::Scene::WorldPosition canonicalOrigin;
		double maximumLocalMagnitudeMetres = 8'192.0;

		[[nodiscard]] bool IsValid() const noexcept;
	};

	struct PhysicsFrameLocalPose {
		PhysicsFrameHandle frame;
		PhysicsPose pose;

		[[nodiscard]] bool IsFinite() const noexcept;
	};

	enum class PhysicsFrameConversionError : std::uint8_t {
		None,
		InvalidFrame,
		StaleFrame,
		NonFinitePose,
		OutsideLocalBounds,
		CoordinateOverflow
	};

	struct PhysicsFramePoseConversion {
		PhysicsFrameLocalPose localPose;
		PhysicsFrameConversionError error = PhysicsFrameConversionError::None;

		[[nodiscard]] constexpr bool Succeeded() const noexcept {
			return error == PhysicsFrameConversionError::None;
		}
	};

	struct PhysicsCanonicalPoseConversion {
		TerraEngine::Scene::WorldPose canonicalPose;
		PhysicsFrameConversionError error = PhysicsFrameConversionError::None;

		[[nodiscard]] constexpr bool Succeeded() const noexcept {
			return error == PhysicsFrameConversionError::None;
		}
	};

	// A membership is copied into every cross-frame request. The exact descriptor
	// generation makes stale residency a rejection, never an implicit relocation.
	struct PhysicsFrameMembership {
		PhysicsFrameHandle frame;
		PhysicsWorldHandle world;
		std::uint64_t residencyGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return frame.IsValid() && world.IsValid() && residencyGeneration != 0U;
		}
	};

	TERRAENGINE_API PhysicsFramePoseConversion LocalizePhysicsPose(
		const TerraEngine::Scene::WorldPose& canonicalPose,
		const PhysicsFrameDescriptor& frame) noexcept;
	TERRAENGINE_API PhysicsCanonicalPoseConversion ReconstructCanonicalPhysicsPose(
		const PhysicsFrameLocalPose& localPose,
		const PhysicsFrameDescriptor& frame) noexcept;
	TERRAENGINE_API std::string_view ToString(PhysicsFrameConversionError error) noexcept;

	// A typed allocator owns compact slots for one handle family. Release keeps a
	// tombstone so callers can distinguish a destroyed target from an invalid
	// handle; reallocation advances the slot generation and makes the old value
	// stale. Allocation is protected because registries may be read concurrently.
	template<typename Handle>
	class PhysicsHandleAllocator {
		static_assert(std::is_same_v<Handle, PhysicsBodyHandle>
			|| std::is_same_v<Handle, PhysicsConstraintHandle>
			|| std::is_same_v<Handle, PhysicsCharacterHandle>
			|| std::is_same_v<Handle, PhysicsShapeHandle>,
			"PhysicsHandleAllocator requires an engine-owned typed physics handle.");

	public:
		explicit PhysicsHandleAllocator(std::size_t initialCapacity = 0) {
			slots.reserve(initialCapacity);
			freeSlots.reserve(initialCapacity);
		}

		PhysicsHandleAllocator(const PhysicsHandleAllocator&) = delete;
		PhysicsHandleAllocator& operator=(const PhysicsHandleAllocator&) = delete;

		Handle Allocate(PhysicsWorldHandle world) {
			if(!world.IsValid()) {
				return {};
			}

			std::lock_guard<std::mutex> lock(mutex);
			PhysicsSlotIndex index = kInvalidPhysicsSlot;
			if(!freeSlots.empty()) {
				index = freeSlots.back();
				freeSlots.pop_back();
				Slot& slot = slots[index];
				slot.generation = NextGeneration(slot.generation);
				slot.world = world;
				slot.alive = true;
			} else {
				if(slots.size() >= static_cast<std::size_t>(kInvalidPhysicsSlot)) {
					return {};
				}
				// A later Release must never allocate after marking a live slot
				// dead. Grow its free-list capacity before publishing a new slot.
				try {
					if(freeSlots.capacity() < slots.size() + 1U)
						freeSlots.reserve(std::max(slots.capacity(), slots.size() + 1U));
				} catch(const std::bad_alloc&) {
					return {};
				}
				index = static_cast<PhysicsSlotIndex>(slots.size());
				try {
					slots.push_back(Slot{world, 1U, true});
				} catch(const std::bad_alloc&) {
					return {};
				}
			}

			const Slot& slot = slots[index];
			return Handle{slot.world.worldId, slot.world.worldGeneration, index, slot.generation};
		}

		bool Release(Handle handle) {
			std::lock_guard<std::mutex> lock(mutex);
			if(StatusLocked(handle) != PhysicsHandleStatus::Alive) {
				return false;
			}

			Slot& slot = slots[handle.slotIndex];
			slot.alive = false;
			freeSlots.push_back(handle.slotIndex);
			return true;
		}

		PhysicsHandleStatus Status(Handle handle) const {
			std::lock_guard<std::mutex> lock(mutex);
			return StatusLocked(handle);
		}

		PhysicsHandleStatus Status(Handle handle, PhysicsWorldHandle currentWorld) const {
			if(!handle.IsValid()) {
				return PhysicsHandleStatus::Invalid;
			}
			if(!currentWorld.IsValid()
				|| handle.worldId != currentWorld.worldId
				|| handle.worldGeneration != currentWorld.worldGeneration) {
				return PhysicsHandleStatus::StaleWorld;
			}
			return Status(handle);
		}

		std::size_t Capacity() const {
			std::lock_guard<std::mutex> lock(mutex);
			return slots.size();
		}

		std::size_t AliveCount() const {
			std::lock_guard<std::mutex> lock(mutex);
			return slots.size() - freeSlots.size();
		}

		void Clear() {
			std::lock_guard<std::mutex> lock(mutex);
			freeSlots.clear();
			for(PhysicsSlotIndex index = 0; index < slots.size(); ++index) {
				Slot& slot = slots[index];
				slot.alive = false;
				freeSlots.push_back(index);
			}
		}

	private:
		struct Slot {
			PhysicsWorldHandle world;
			PhysicsGeneration generation = 1;
			bool alive = false;
		};

		static PhysicsGeneration NextGeneration(PhysicsGeneration generation) {
			++generation;
			return generation == 0 ? 1U : generation;
		}

		PhysicsHandleStatus StatusLocked(Handle handle) const {
			if(!handle.IsValid() || handle.slotIndex >= slots.size()) {
				return PhysicsHandleStatus::Invalid;
			}

			const Slot& slot = slots[handle.slotIndex];
			if(handle.worldId != slot.world.worldId
				|| handle.worldGeneration != slot.world.worldGeneration) {
				return PhysicsHandleStatus::StaleWorld;
			}
			if(handle.slotGeneration != slot.generation) {
				return PhysicsHandleStatus::StaleSlot;
			}
			return slot.alive ? PhysicsHandleStatus::Alive : PhysicsHandleStatus::Destroyed;
		}

		mutable std::mutex mutex;
		std::vector<Slot> slots;
		std::vector<PhysicsSlotIndex> freeSlots;
	};

	TERRAENGINE_API std::string_view ToString(PhysicsHandleStatus status);
	TERRAENGINE_API std::string_view ToString(PhysicsResultStatus status);
	TERRAENGINE_API std::string_view ToString(PhysicsMotionType type);
	TERRAENGINE_API std::string_view ToString(PhysicsMotionQuality quality);
	TERRAENGINE_API std::string_view ToString(PhysicsForceMode mode);
	TERRAENGINE_API std::string_view ToString(PhysicsWorldState state);
	TERRAENGINE_API std::string_view ToString(PhysicsQueryKind kind);
	TERRAENGINE_API std::string_view ToString(PhysicsShapeKind kind);
	TERRAENGINE_API std::string_view ToString(PhysicsConstraintKind kind);
	TERRAENGINE_API std::string_view ToString(PhysicsCombineMode mode);
}
