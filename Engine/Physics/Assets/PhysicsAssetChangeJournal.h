// PhysicsAssetChangeJournal.h - explicit collider and material invalidation.
//
// Asset writers publish small generation-tagged records here after their
// protected update completes. Physics can then ask for changes since its last
// observation instead of polling every referenced file on every frame.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Physics {
	using PhysicsAssetGeneration = std::uint64_t;

	enum class PhysicsAssetKind : std::uint8_t {
		CookedCollider,
		Material
	};

	enum class PhysicsAssetChangeKind : std::uint8_t {
		Added,
		Updated,
		Removed
	};

	struct PhysicsAssetChangeRecord {
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		PhysicsAssetKind assetKind = PhysicsAssetKind::CookedCollider;
		PhysicsAssetChangeKind changeKind = PhysicsAssetChangeKind::Updated;
		PhysicsAssetGeneration generation = 0U;
	};

	// A snapshot owns all of its records and is therefore safe to pass to worker
	// preparation after the journal mutex has been released.
	struct PhysicsAssetChangeSnapshot {
		PhysicsAssetGeneration fromGenerationExclusive = 0U;
		PhysicsAssetGeneration toGenerationInclusive = 0U;
		std::uint64_t sourceEpoch = 0U;
		bool requiresFullRefresh = false;
		std::vector<PhysicsAssetChangeRecord> changes;

		bool Empty() const noexcept { return changes.empty() && !requiresFullRefresh; }
		bool IsValid() const noexcept;
	};

	// The journal is intentionally bounded. A consumer that falls behind the
	// retained window receives requiresFullRefresh instead of an incomplete set.
	class TERRAENGINE_API PhysicsAssetChangeJournal final {
	public:
		explicit PhysicsAssetChangeJournal(std::size_t maximumRetainedChanges = 4096U);

		static PhysicsAssetChangeJournal& Instance();

		PhysicsAssetGeneration Publish(
			const TerraEngine::UUID& assetId,
			PhysicsAssetKind assetKind,
			PhysicsAssetChangeKind changeKind);
		PhysicsAssetChangeSnapshot SnapshotSince(PhysicsAssetGeneration observedGeneration) const;
		PhysicsAssetGeneration CurrentGeneration() const;
		PhysicsAssetGeneration AssetGeneration(
			const TerraEngine::UUID& assetId,
			PhysicsAssetKind assetKind) const;
		std::uint64_t SourceEpoch() const;

		// Project changes invalidate every prior dependency observation. The global
		// generation remains monotonic, while the epoch tells diagnostics why a full
		// refresh was deliberately requested.
		void BeginSourceEpoch();

	private:
		struct AssetKey {
			TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
			PhysicsAssetKind assetKind = PhysicsAssetKind::CookedCollider;

			bool operator==(const AssetKey&) const = default;
		};

		struct AssetKeyHash {
			std::size_t operator()(const AssetKey& key) const noexcept;
		};

		PhysicsAssetGeneration NextGenerationLocked();

		const std::size_t maximumRetainedChanges;
		mutable std::mutex mutex;
		PhysicsAssetGeneration currentGeneration = 0U;
		PhysicsAssetGeneration fullRefreshGeneration = 0U;
		std::uint64_t sourceEpoch = 1U;
		std::deque<PhysicsAssetChangeRecord> retainedChanges;
		std::unordered_map<AssetKey, PhysicsAssetGeneration, AssetKeyHash> assetGenerations;
	};
}
