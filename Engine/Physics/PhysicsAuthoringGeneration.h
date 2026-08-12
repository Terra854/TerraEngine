// PhysicsAuthoringGeneration.h - immutable reason-tagged authoring changes.
//
// The generation is the worker-safe bridge between protected scene capture and
// provider preparation. It names only stable engine identities and descriptor
// indices; no Object facade, component address, or provider pointer is retained.

#pragma once

#include "Assets/PhysicsAssetChangeJournal.h"
#include "Core/Export.h"
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace TerraEngine::Physics {
	using PhysicsDescriptorIndex = std::uint32_t;
	inline constexpr PhysicsDescriptorIndex kInvalidPhysicsDescriptorIndex =
		std::numeric_limits<PhysicsDescriptorIndex>::max();

	enum class PhysicsAuthoringChangeReason : std::uint32_t {
		None = 0U,
		Added = 1U << 0U,
		Removed = 1U << 1U,
		Transform = 1U << 2U,
		BodyComponent = 1U << 3U,
		ColliderComponent = 1U << 4U,
		HierarchyDependency = 1U << 5U,
		ColliderAsset = 1U << 6U,
		MaterialAsset = 1U << 7U,
		Settings = 1U << 8U,
		Lifecycle = 1U << 9U
	};

	constexpr PhysicsAuthoringChangeReason operator|(
		PhysicsAuthoringChangeReason left,
		PhysicsAuthoringChangeReason right) noexcept {
		return static_cast<PhysicsAuthoringChangeReason>(
			static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	constexpr PhysicsAuthoringChangeReason operator&(
		PhysicsAuthoringChangeReason left,
		PhysicsAuthoringChangeReason right) noexcept {
		return static_cast<PhysicsAuthoringChangeReason>(
			static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
	}

	constexpr PhysicsAuthoringChangeReason& operator|=(
		PhysicsAuthoringChangeReason& left,
		PhysicsAuthoringChangeReason right) noexcept {
		left = left | right;
		return left;
	}

	constexpr bool HasAnyReason(
		PhysicsAuthoringChangeReason value,
		PhysicsAuthoringChangeReason mask) noexcept {
		return (value & mask) != PhysicsAuthoringChangeReason::None;
	}

	struct PhysicsAuthoringChange {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration objectGeneration = 0U;
		PhysicsDescriptorIndex descriptorIndex = kInvalidPhysicsDescriptorIndex;
		PhysicsAuthoringChangeReason reasons = PhysicsAuthoringChangeReason::None;
	};

	struct PhysicsAuthoringAssetObservation {
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		PhysicsAssetKind assetKind = PhysicsAssetKind::CookedCollider;
		PhysicsAssetGeneration generation = 0U;
	};

	// A fallback is explicit evidence that incremental scene continuity could not
	// be proved. Consumers must rebuild completely instead of guessing that a
	// missing publication meant no authored change.
	enum class PhysicsAuthoringFallbackReason : std::uint8_t {
		None,
		InitialWorld,
		WorldReplacement,
		ScenePacketUnavailable,
		ScenePacketInvalid,
		ScenePacketDiscontinuity,
		SceneGenerationChanged,
		AssetJournalRefresh,
		IntegrityRecovery
	};

	struct PhysicsAuthoringGenerationData {
		PhysicsWorldHandle world;
		PhysicsGeneration generation = 0U;
		PhysicsGeneration snapshotGeneration = 0U;
		std::uint64_t sceneGeneration = 0U;
		std::uint64_t scenePublicationRevision = 0U;
		std::uint64_t previousScenePublicationRevision = 0U;
		std::uint64_t hierarchyGeneration = 0U;
		std::uint64_t settingsHash = 0U;
		PhysicsAssetGeneration assetJournalGeneration = 0U;
		bool fullAssetRefresh = false;
		PhysicsAuthoringFallbackReason fallbackReason = PhysicsAuthoringFallbackReason::None;
		std::vector<PhysicsAuthoringChange> changes;
		// Typed closure lists let preparation touch only dependent runtime families.
		std::vector<TerraEngine::UUID> constraintRecordIds;
		std::vector<TerraEngine::UUID> characterObjectIds;
		std::vector<TerraEngine::UUID> removedObjectIds;
		std::vector<PhysicsAuthoringAssetObservation> assetObservations;
	};

	class TERRAENGINE_API PhysicsAuthoringGeneration final {
	public:
		static std::optional<PhysicsAuthoringGeneration> Create(
			PhysicsAuthoringGenerationData data);

		const PhysicsAuthoringGenerationData& Data() const noexcept { return *data; }
		bool IsValid() const noexcept { return data != nullptr; }
		bool Unchanged() const noexcept { return data != nullptr && data->changes.empty(); }
		const PhysicsAuthoringChange* Find(const TerraEngine::UUID& objectId) const noexcept;
		std::vector<PhysicsAuthoringChange> ChangesWithReason(
			PhysicsAuthoringChangeReason mask) const;

	private:
		explicit PhysicsAuthoringGeneration(
			std::shared_ptr<const PhysicsAuthoringGenerationData> value)
			: data(std::move(value)) {}

		std::shared_ptr<const PhysicsAuthoringGenerationData> data;
	};
}
