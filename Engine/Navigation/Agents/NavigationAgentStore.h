#pragma once

#include "NavigationAgentTypes.h"
#include "Navigation/NavigationStores.h"

#include <mutex>
#include <optional>
#include <vector>

namespace TerraEngine::Navigation {
	struct NavigationAgentRuntimeRecord {
		NavigationAgentIdentity identity{};
		NavigationAgentAuthoredSettings settings{};
		NavigationGeneration configurationRevision = 1U;
		NavigationGeneration intentSequence = 0U;
		NavigationRequestedDestination destination{};
		NavigationAgentState state = NavigationAgentState::Idle;
		NavigationOutcome outcome = NavigationOutcome::Queued;
		NavigationReason reason = NavigationReason::None;
		NavigationRequestHandle request{};
		NavigationRouteHandle route{};
		NavigationAgentGuidanceSnapshot guidance{};
		// This advances at each protected publication even when guidance is neutral,
		// giving managed/editor readers a coherent observation revision.
		NavigationGeneration publicationRevision = 0U;
		double nextReplanTimeSeconds = 0.0;
		std::uint64_t sourceComponentGeneration = 0U;
	};

	struct NavigationAgentStoreCreateResult {
		NavigationAgentIdentity identity{};
		NavigationHandleStatus status = NavigationHandleStatus::Invalid;
		[[nodiscard]] bool Succeeded() const noexcept {
			return status == NavigationHandleStatus::Alive && identity.IsValid();
		}
	};

	struct NavigationAgentStoreReadResult {
		NavigationHandleStatus status = NavigationHandleStatus::Invalid;
		std::optional<NavigationAgentRuntimeRecord> record{};
	};

	class TERRAENGINE_API NavigationAgentStore final {
	public:
		explicit NavigationAgentStore(std::size_t capacity);
		NavigationAgentStoreCreateResult Create(NavigationAgentIdentity, NavigationAgentRuntimeRecord);
		NavigationAgentStoreReadResult Read(NavigationAgentIdentity) const;
		bool Release(NavigationAgentIdentity);
		std::size_t RevokeWorld(NavigationWorldHandle);
		std::vector<NavigationAgentRuntimeRecord> Snapshot() const;
		[[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }
		std::size_t AliveCount() const;
		std::size_t MemoryBytes() const;

		template<class F>
		bool Update(NavigationAgentIdentity id, F&& f) {
			std::lock_guard lock(mutex_);
			if(!Matches(id)) return false;
			f(slots_[id.runtimeSlot].record);
			return true;
		}

	private:
		struct Slot {
			NavigationGeneration generation = 0U;
			bool alive = false;
			NavigationAgentRuntimeRecord record{};
		};
		bool Matches(NavigationAgentIdentity) const;
		std::size_t capacity_;
		// Each store construction receives a process-unique high-word. Low bits
		// advance per slot reuse, so a cached identity cannot alias after shutdown.
		NavigationGeneration generationPrefix_ = 0U;
		mutable std::mutex mutex_;
		std::vector<Slot> slots_;
		std::vector<NavigationSlotIndex> free_;
	};
}
