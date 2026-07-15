// IncrementalBodyStateStore.cpp - copy-on-write body-state page publication.

#include "IncrementalBodyStateStore.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Physics {
	namespace {
		bool SameState(const PhysicsBodyState& left, const PhysicsBodyState& right) noexcept {
			return left.handle == right.handle
				&& left.objectId == right.objectId
				&& left.objectGeneration == right.objectGeneration
				&& left.componentGeneration == right.componentGeneration
				&& left.worldPose == right.worldPose
				&& left.linearVelocity == right.linearVelocity
				&& left.angularVelocity == right.angularVelocity
				&& left.awake == right.awake;
		}

		bool SamePage(const IncrementalBodyStatePage& left, const IncrementalBodyStatePage& right) noexcept {
			if(left.pageIndex != right.pageIndex || left.liveBodyCount != right.liveBodyCount
				|| left.slots.size() != right.slots.size()) return false;
			for(std::size_t index = 0U; index < left.slots.size(); ++index) {
				if(left.slots[index].has_value() != right.slots[index].has_value()) return false;
				if(left.slots[index].has_value() && !SameState(*left.slots[index], *right.slots[index])) return false;
			}
			return true;
		}

		bool ValidStateIdentity(const PhysicsBodyState& state, PhysicsWorldHandle world) noexcept {
			return state.handle.IsValid()
				&& state.handle.worldId == world.worldId
				&& state.handle.worldGeneration == world.worldGeneration
				&& state.objectId != TerraEngine::EMPTY_UUID
				&& state.objectGeneration != 0U
				&& state.componentGeneration != 0U;
		}

		bool HasCompleteStoredFields(PhysicsBodyStateFieldMask fields) noexcept {
			constexpr PhysicsBodyStateFieldMask required = PhysicsBodyStateFieldMask::Pose
				| PhysicsBodyStateFieldMask::LinearVelocity
				| PhysicsBodyStateFieldMask::AngularVelocity
				| PhysicsBodyStateFieldMask::Awake;
			return HasPhysicsBodyStateFields(fields, required);
		}

		bool MergeState(
			PhysicsBodyState& destination,
			const PhysicsBodyState& source,
			PhysicsBodyStateFieldMask fields) noexcept {
			if(!ValidStateIdentity(source, {source.handle.worldId, source.handle.worldGeneration})) return false;
			if(destination.handle.IsValid() && (destination.handle != source.handle
				|| destination.objectId != source.objectId
				|| destination.objectGeneration != source.objectGeneration
				|| destination.componentGeneration != source.componentGeneration)) return false;
			destination.handle = source.handle;
			destination.objectId = source.objectId;
			destination.objectGeneration = source.objectGeneration;
			destination.componentGeneration = source.componentGeneration;
			if(HasPhysicsBodyStateFields(fields, PhysicsBodyStateFieldMask::Pose)) destination.worldPose = source.worldPose;
			if(HasPhysicsBodyStateFields(fields, PhysicsBodyStateFieldMask::LinearVelocity)) destination.linearVelocity = source.linearVelocity;
			if(HasPhysicsBodyStateFields(fields, PhysicsBodyStateFieldMask::AngularVelocity)) destination.angularVelocity = source.angularVelocity;
			if(HasPhysicsBodyStateFields(fields, PhysicsBodyStateFieldMask::Awake)) destination.awake = source.awake;
			return destination.worldPose.IsFinite()
				&& destination.linearVelocity.IsFinite()
				&& destination.angularVelocity.IsFinite();
		}
	}

	const PhysicsBodyState* IncrementalBodyStateSnapshot::Find(PhysicsBodyHandle handle) const noexcept {
		if(!handle.IsValid() || handle.worldId != world.worldId
			|| handle.worldGeneration != world.worldGeneration || pageSize == 0U) return nullptr;
		const std::size_t pageIndex = handle.slotIndex / pageSize;
		const std::size_t slotIndex = handle.slotIndex % pageSize;
		if(pageIndex >= pages.size() || pages[pageIndex] == nullptr
			|| slotIndex >= pages[pageIndex]->slots.size()) return nullptr;
		const std::optional<PhysicsBodyState>& state = pages[pageIndex]->slots[slotIndex];
		return state.has_value() && state->handle == handle ? &*state : nullptr;
	}

	std::vector<PhysicsBodyState> IncrementalBodyStateSnapshot::Flatten() const {
		std::vector<PhysicsBodyState> flattened;
		flattened.reserve(bodyCount);
		for(const auto& page : pages) {
			if(page == nullptr) continue;
			for(const auto& state : page->slots) {
				if(state.has_value()) flattened.push_back(*state);
			}
		}
		return flattened;
	}

	IncrementalBodyStateStore::IncrementalBodyStateStore(
		IncrementalBodyStateStoreConfiguration requestedConfiguration)
		: configuration(requestedConfiguration) {
		if(configuration.pageSize == 0U) configuration.pageSize = 256U;
		if(configuration.maximumSlots < configuration.pageSize) configuration.maximumSlots = configuration.pageSize;
		if(configuration.maximumRetainedBytes == 0U) configuration.maximumRetainedBytes = 64U * 1024U * 1024U;
	}

	std::optional<IncrementalBodyStatePublication> IncrementalBodyStateStore::Apply(
		const IncrementalBodyStateUpdateBatch& batch) {
		std::lock_guard<std::mutex> lock(mutex);
		auto reject = [&]() -> std::optional<IncrementalBodyStatePublication> {
			++statistics.rejectedBatchCount;
			return std::nullopt;
		};
		if(!batch.world.IsValid() || batch.tick == 0U || batch.providerOrderGeneration == 0U
			|| (expectedWorld.IsValid() && expectedWorld != batch.world)
			|| (latestComplete != nullptr && batch.tick <= latestComplete->tick)
			|| (latestComplete == nullptr && !batch.fullState)) return reject();

		std::unordered_set<std::uint64_t> seenHandles;
		seenHandles.reserve(batch.updates.size());
		std::size_t highestSlot = 0U;
		for(const IncrementalBodyStateUpdate& update : batch.updates) {
			const PhysicsBodyHandle handle = update.removed ? update.removedHandle : update.state.handle;
			const std::uint64_t key = (static_cast<std::uint64_t>(handle.slotIndex) << 32U) | handle.slotGeneration;
			if(!handle.IsValid() || handle.worldId != batch.world.worldId
				|| handle.worldGeneration != batch.world.worldGeneration
				|| handle.slotIndex >= configuration.maximumSlots || !seenHandles.insert(key).second
				|| (!update.removed && (!ValidStateIdentity(update.state, batch.world)
					|| update.fields == PhysicsBodyStateFieldMask::None))) return reject();
			highestSlot = std::max(highestSlot, static_cast<std::size_t>(handle.slotIndex));
		}
		if(batch.fullState && (batch.expectedCompleteBodyCount != batch.updates.size()
			|| std::any_of(batch.updates.begin(), batch.updates.end(), [](const auto& update) {
				return update.removed || !HasCompleteStoredFields(update.fields);
			}))) return reject();

		std::vector<std::shared_ptr<const IncrementalBodyStatePage>> pages = batch.fullState || latestComplete == nullptr
			? std::vector<std::shared_ptr<const IncrementalBodyStatePage>>{}
			: latestComplete->pages;
		const std::size_t requiredPageCount = batch.updates.empty() ? pages.size()
			: highestSlot / configuration.pageSize + 1U;
		pages.resize(std::max(pages.size(), requiredPageCount));
		std::unordered_map<std::size_t, std::shared_ptr<IncrementalBodyStatePage>> writablePages;
		writablePages.reserve(batch.updates.size());

		auto writablePage = [&](std::size_t pageIndex) -> IncrementalBodyStatePage& {
			const auto existing = writablePages.find(pageIndex);
			if(existing != writablePages.end()) return *existing->second;
			auto page = std::make_shared<IncrementalBodyStatePage>();
			page->pageIndex = static_cast<std::uint32_t>(pageIndex);
			page->generation = generation + 1U;
			page->slots.resize(configuration.pageSize);
			if(!batch.fullState && pageIndex < pages.size() && pages[pageIndex] != nullptr) {
				page->slots = pages[pageIndex]->slots;
				page->liveBodyCount = pages[pageIndex]->liveBodyCount;
			}
			writablePages.emplace(pageIndex, page);
			return *page;
		};

		std::vector<ChangedBodyStateRecord> changes;
		changes.reserve(batch.updates.size());
		for(const IncrementalBodyStateUpdate& update : batch.updates) {
			const PhysicsBodyHandle handle = update.removed ? update.removedHandle : update.state.handle;
			const std::size_t pageIndex = handle.slotIndex / configuration.pageSize;
			const std::size_t slotIndex = handle.slotIndex % configuration.pageSize;
			IncrementalBodyStatePage& page = writablePage(pageIndex);
			std::optional<PhysicsBodyState>& slot = page.slots[slotIndex];
			if(update.removed) {
				if(!slot.has_value() || slot->handle != handle) return reject();
				slot.reset();
				--page.liveBodyCount;
				changes.push_back({handle, std::nullopt, PhysicsBodyStateFieldMask::None,
					update.change | PhysicsBodyStateChangeMask::Removed, true});
				continue;
			}

			const bool created = !slot.has_value();
			if(created) {
				if(!HasCompleteStoredFields(update.fields)) return reject();
				slot.emplace();
				++page.liveBodyCount;
			}
			if(!MergeState(*slot, update.state, update.fields)) return reject();
			PhysicsBodyStateChangeMask change = update.change;
			if(created) change |= PhysicsBodyStateChangeMask::Created;
			changes.push_back({handle, *slot, update.fields, change, false});
		}

		std::size_t pagesCopied = 0U;
		std::size_t pagesReused = 0U;
		for(auto& [pageIndex, writable] : writablePages) {
			const std::shared_ptr<const IncrementalBodyStatePage> previous = latestComplete != nullptr
				&& pageIndex < latestComplete->pages.size() ? latestComplete->pages[pageIndex] : nullptr;
			if(previous != nullptr && SamePage(*previous, *writable)) {
				pages[pageIndex] = previous;
				++pagesReused;
			} else {
				pages[pageIndex] = std::move(writable);
				++pagesCopied;
			}
		}
		for(std::size_t index = 0U; index < pages.size(); ++index) {
			if(!writablePages.contains(index) && pages[index] != nullptr) ++pagesReused;
		}
		while(!pages.empty() && (pages.back() == nullptr || pages.back()->liveBodyCount == 0U)) pages.pop_back();

		std::size_t bodyCount = 0U;
		for(const auto& page : pages) if(page != nullptr) bodyCount += page->liveBodyCount;
		if(batch.expectedCompleteBodyCount != 0U && bodyCount != batch.expectedCompleteBodyCount) return reject();
		const std::size_t retainedBytes = pages.size()
			* (sizeof(IncrementalBodyStatePage) + configuration.pageSize * sizeof(std::optional<PhysicsBodyState>));
		if(retainedBytes > configuration.maximumRetainedBytes) return reject();

		++generation;
		if(generation == 0U) ++generation;
		auto complete = std::make_shared<IncrementalBodyStateSnapshot>();
		complete->world = batch.world;
		complete->tick = batch.tick;
		complete->storeGeneration = generation;
		complete->providerOrderGeneration = batch.providerOrderGeneration;
		complete->lastFullStateVerificationTick = batch.fullState ? batch.tick
			: latestComplete->lastFullStateVerificationTick;
		complete->pageSize = configuration.pageSize;
		complete->bodyCount = bodyCount;
		complete->retainedBytes = retainedBytes;
		complete->pages = std::move(pages);

		auto changed = std::make_shared<ChangedBodyStatePacket>();
		changed->world = batch.world;
		changed->tick = batch.tick;
		changed->storeGeneration = generation;
		changed->changes = std::make_shared<const std::vector<ChangedBodyStateRecord>>(std::move(changes));
		latestComplete = complete;
		latestChanged = changed;
		expectedWorld = batch.world;
		++statistics.publicationCount;
		statistics.pagesCopied += pagesCopied;
		statistics.pagesReused += pagesReused;
		statistics.retainedBytes = retainedBytes;
		return IncrementalBodyStatePublication{std::move(complete), std::move(changed), pagesReused, pagesCopied};
	}

	std::shared_ptr<const IncrementalBodyStateSnapshot> IncrementalBodyStateStore::AcquireComplete() const {
		std::lock_guard<std::mutex> lock(mutex);
		return latestComplete;
	}

	std::shared_ptr<const ChangedBodyStatePacket> IncrementalBodyStateStore::AcquireChanged() const {
		std::lock_guard<std::mutex> lock(mutex);
		return latestChanged;
	}

	IncrementalBodyStateStoreStatistics IncrementalBodyStateStore::Statistics() const {
		std::lock_guard<std::mutex> lock(mutex);
		return statistics;
	}

	void IncrementalBodyStateStore::Reset(PhysicsWorldHandle world) {
		std::lock_guard<std::mutex> lock(mutex);
		expectedWorld = world;
		latestComplete.reset();
		latestChanged.reset();
		generation = 0U;
		statistics.retainedBytes = 0U;
	}
}
