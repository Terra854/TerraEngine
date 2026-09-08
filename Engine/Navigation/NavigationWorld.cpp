// NavigationWorld.cpp - atomic immutable-root publication and reclamation.

#include "Navigation/NavigationWorld.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace TerraEngine::Navigation {
	NavigationWorld::NavigationWorld(NavigationWorldHandle handle, std::size_t maximumRetiredRoots)
		: handle_(handle)
		, maximumRetiredRoots_(maximumRetiredRoots) {
		NavigationWorldRoot initial;
		initial.world = handle_;
		initial.rootGeneration = handle_.IsValid() ? 1U : 0U;
		(void)EstimateOwnedBytes(initial, initial.chargedBytes);
		publishedReservation_ = budget_.TryReserve(
			NavigationBudgetCategory::PublishedNavigation, initial.chargedBytes, "initial navigation world root");
		published_ = std::make_shared<const NavigationWorldRoot>(std::move(initial));
	}

	NavigationWorldHandle NavigationWorld::Handle() const noexcept {
		return handle_;
	}

	std::shared_ptr<const NavigationWorldRoot> NavigationWorld::PublishedRoot() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return published_;
	}

	NavigationWorldRoot NavigationWorld::CreateCandidate() const {
		std::lock_guard<std::mutex> lock(mutex_);
		NavigationWorldRoot candidate = *published_;
		if(candidate.rootGeneration != std::numeric_limits<NavigationGeneration>::max()) {
			++candidate.rootGeneration;
		}
		return candidate;
	}

	bool NavigationWorld::Publish(NavigationWorldRoot candidate, NavigationGeneration expectedRootGeneration) {
		std::lock_guard<std::mutex> lock(mutex_);
		// Reclaim roots whose external leases ended before checking the hard bound.
		// If all slots are still borrowed, reject the replacement atomically rather
		// than allowing retirement ownership to grow without limit.
		retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const RetiredRoot& retired) {
			return retired.root.use_count() == 1;
		}), retired_.end());
		std::size_t candidateBytes = 0U;
		if(!handle_.IsValid() || candidate.world != handle_
			|| published_->rootGeneration != expectedRootGeneration
			|| expectedRootGeneration == std::numeric_limits<NavigationGeneration>::max()
			|| candidate.rootGeneration != expectedRootGeneration + 1U
			|| maximumRetiredRoots_ == 0U || retired_.size() >= maximumRetiredRoots_
			|| !EstimateOwnedBytes(candidate, candidateBytes)) {
			return false;
		}

		NavigationBudgetReservation candidateReservation = budget_.TryReserve(
			NavigationBudgetCategory::PublishedNavigation, candidateBytes, "navigation world root replacement");
		if(!candidateReservation.accepted) {
			return false;
		}
		candidate.chargedBytes = candidateBytes;
		std::shared_ptr<const NavigationWorldRoot> replacement =
			std::make_shared<const NavigationWorldRoot>(std::move(candidate));
		retired_.push_back({std::move(published_), std::move(publishedReservation_)});
		published_ = std::move(replacement);
		publishedReservation_ = std::move(candidateReservation);
		return true;
	}

	void NavigationWorld::CollectRetiredRoots() {
		std::lock_guard<std::mutex> lock(mutex_);
		retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const RetiredRoot& retired) {
			// One reference means only the retirement list still owns the root.
			return retired.root.use_count() == 1;
		}), retired_.end());
	}

	std::size_t NavigationWorld::RetiredRootCount() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return retired_.size();
	}

	bool NavigationWorld::EstimateOwnedBytes(const NavigationWorldRoot& root, std::size_t& bytes) noexcept {
		bytes = sizeof(NavigationWorldRoot);
		const auto addCapacity = [&bytes](std::size_t count, std::size_t elementSize) {
			if(count != 0U && elementSize > std::numeric_limits<std::size_t>::max() / count) {
				return false;
			}
			const std::size_t allocationBytes = count * elementSize;
			if(allocationBytes > std::numeric_limits<std::size_t>::max() - bytes) {
				return false;
			}
			bytes += allocationBytes;
			return true;
		};
		return addCapacity(root.sources.capacity(), sizeof(NavigationSourceIndexRecord))
			&& addCapacity(root.coverage.capacity(), sizeof(NavigationCoverageRecord))
			&& addCapacity(root.tombstones.capacity(), sizeof(NavigationSourceTombstone))
			&& addCapacity(root.agents.capacity(), sizeof(NavigationAgentTableRecord))
			&& addCapacity(root.requests.capacity(), sizeof(NavigationRequestTableRecord));
	}
}
