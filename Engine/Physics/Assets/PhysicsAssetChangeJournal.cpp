#include "PhysicsAssetChangeJournal.h"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

namespace TerraEngine::Physics {
	namespace {
		bool LessAssetKey(
			PhysicsAssetKind leftKind,
			const TerraEngine::UUID& leftId,
			PhysicsAssetKind rightKind,
			const TerraEngine::UUID& rightId) {
			return std::tie(leftKind, leftId) < std::tie(rightKind, rightId);
		}
	}

	bool PhysicsAssetChangeSnapshot::IsValid() const noexcept {
		if(sourceEpoch == 0U || toGenerationInclusive < fromGenerationExclusive) {
			return false;
		}
		PhysicsAssetKind previousKind = PhysicsAssetKind::CookedCollider;
		TerraEngine::UUID previousId = TerraEngine::EMPTY_UUID;
		bool first = true;
		for(const PhysicsAssetChangeRecord& change : changes) {
			if(change.assetId == TerraEngine::EMPTY_UUID || change.generation == 0U
				|| change.generation > toGenerationInclusive
				|| change.generation <= fromGenerationExclusive) {
				return false;
			}
			if(!first && !LessAssetKey(previousKind, previousId, change.assetKind, change.assetId)) {
				return false;
			}
			previousKind = change.assetKind;
			previousId = change.assetId;
			first = false;
		}
		return true;
	}

	PhysicsAssetChangeJournal::PhysicsAssetChangeJournal(std::size_t retainedLimit)
		: maximumRetainedChanges(std::max<std::size_t>(1U, retainedLimit)) {}

	PhysicsAssetChangeJournal& PhysicsAssetChangeJournal::Instance() {
		static PhysicsAssetChangeJournal journal;
		return journal;
	}

	std::size_t PhysicsAssetChangeJournal::AssetKeyHash::operator()(const AssetKey& key) const noexcept {
		const std::size_t assetHash = std::hash<TerraEngine::UUID>{}(key.assetId);
		return assetHash ^ (static_cast<std::size_t>(key.assetKind) + 0x9e3779b9U
			+ (assetHash << 6U) + (assetHash >> 2U));
	}

	PhysicsAssetGeneration PhysicsAssetChangeJournal::NextGenerationLocked() {
		++currentGeneration;
		if(currentGeneration == 0U) {
			// Generation zero is reserved for consumers that have never observed the
			// journal. Overflow is extraordinarily unlikely, but this keeps the typed
			// contract valid instead of silently publishing the sentinel.
			currentGeneration = 1U;
			fullRefreshGeneration = currentGeneration;
			retainedChanges.clear();
			assetGenerations.clear();
		}
		return currentGeneration;
	}

	PhysicsAssetGeneration PhysicsAssetChangeJournal::Publish(
		const TerraEngine::UUID& assetId,
		PhysicsAssetKind assetKind,
		PhysicsAssetChangeKind changeKind) {
		if(assetId == TerraEngine::EMPTY_UUID) {
			return 0U;
		}

		std::lock_guard lock(mutex);
		const PhysicsAssetGeneration generation = NextGenerationLocked();
		retainedChanges.push_back({assetId, assetKind, changeKind, generation});
		assetGenerations.insert_or_assign(AssetKey{assetId, assetKind}, generation);
		while(retainedChanges.size() > maximumRetainedChanges) {
			retainedChanges.pop_front();
		}
		return generation;
	}

	PhysicsAssetChangeSnapshot PhysicsAssetChangeJournal::SnapshotSince(
		PhysicsAssetGeneration observedGeneration) const {
		std::lock_guard lock(mutex);
		PhysicsAssetChangeSnapshot snapshot;
		snapshot.fromGenerationExclusive = observedGeneration;
		snapshot.toGenerationInclusive = currentGeneration;
		snapshot.sourceEpoch = sourceEpoch;

		const PhysicsAssetGeneration oldestRetained = retainedChanges.empty()
			? currentGeneration + (currentGeneration != std::numeric_limits<PhysicsAssetGeneration>::max() ? 1U : 0U)
			: retainedChanges.front().generation;
		snapshot.requiresFullRefresh = observedGeneration > currentGeneration
			|| observedGeneration < fullRefreshGeneration
			|| (observedGeneration < currentGeneration
				&& observedGeneration + 1U < oldestRetained);

		// Coalescing retains the latest meaning of each asset while stable sorting by
		// type and identity makes worker results independent from file-event order.
		using Key = std::pair<PhysicsAssetKind, TerraEngine::UUID>;
		std::map<Key, PhysicsAssetChangeRecord> latest;
		for(const PhysicsAssetChangeRecord& change : retainedChanges) {
			if(change.generation > observedGeneration) {
				latest.insert_or_assign(Key{change.assetKind, change.assetId}, change);
			}
		}
		snapshot.changes.reserve(latest.size());
		for(const auto& [key, change] : latest) {
			(void)key;
			snapshot.changes.push_back(change);
		}
		return snapshot;
	}

	PhysicsAssetGeneration PhysicsAssetChangeJournal::CurrentGeneration() const {
		std::lock_guard lock(mutex);
		return currentGeneration;
	}

	PhysicsAssetGeneration PhysicsAssetChangeJournal::AssetGeneration(
		const TerraEngine::UUID& assetId,
		PhysicsAssetKind assetKind) const {
		std::lock_guard lock(mutex);
		const auto found = assetGenerations.find({assetId, assetKind});
		return found != assetGenerations.end() ? found->second : 0U;
	}

	std::uint64_t PhysicsAssetChangeJournal::SourceEpoch() const {
		std::lock_guard lock(mutex);
		return sourceEpoch;
	}

	void PhysicsAssetChangeJournal::BeginSourceEpoch() {
		std::lock_guard lock(mutex);
		++sourceEpoch;
		if(sourceEpoch == 0U) sourceEpoch = 1U;
		fullRefreshGeneration = NextGenerationLocked();
		retainedChanges.clear();
		assetGenerations.clear();
	}
}
