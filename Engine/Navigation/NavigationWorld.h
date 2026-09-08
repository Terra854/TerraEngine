// NavigationWorld.h - immutable per-scene ground-navigation authority.

#pragma once

#include "Core/Export.h"
#include "Navigation/NavigationBudget.h"
#include "Navigation/NavigationTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace TerraEngine::Navigation {
	// Coverage keys remain engine-owned canonical addresses. Provider polygon
	// references are deliberately excluded because they are private to one shard
	// generation and cannot safely cross a publication boundary.
	struct NavigationCoverageKey {
		std::int64_t shardX = 0;
		std::int64_t shardZ = 0;
		std::int32_t verticalBand = 0;
		std::uint64_t profileBuildKey = 0U;

		[[nodiscard]] constexpr bool operator==(const NavigationCoverageKey&) const noexcept = default;
	};

	enum class NavigationCoverageState : std::uint8_t {
		Absent,
		Requested,
		Preparing,
		Ready,
		Dirty,
		Retiring
	};

	struct NavigationSourceIndexRecord {
		UUID sourceId = EMPTY_UUID;
		NavigationGeneration sourceRevision = 0U;
		NavigationGeneration geometryRevision = 0U;
		bool enabled = false;
	};

	struct NavigationCoverageRecord {
		NavigationCoverageKey key{};
		NavigationGeneration coverageGeneration = 0U;
		NavigationCoverageState state = NavigationCoverageState::Absent;
		std::size_t chargedBytes = 0U;
		bool complete = false;
	};

	struct NavigationSourceTombstone {
		UUID sourceId = EMPTY_UUID;
		NavigationGeneration removedRevision = 0U;
	};

	struct NavigationAgentTableRecord {
		NavigationAgentHandle handle{};
		NavigationGeneration intentGeneration = 0U;
	};

	struct NavigationRequestTableRecord {
		NavigationRequestHandle handle{};
		NavigationOutcome outcome = NavigationOutcome::Queued;
	};

	// Every reader retains one const root. A writer prepares a complete detached
	// replacement and publishes it only if its expected generation still matches.
	struct NavigationWorldRoot {
		NavigationWorldHandle world{};
		NavigationGeneration rootGeneration = 0U;
		std::vector<NavigationSourceIndexRecord> sources;
		std::vector<NavigationCoverageRecord> coverage;
		std::vector<NavigationSourceTombstone> tombstones;
		std::vector<NavigationAgentTableRecord> agents;
		std::vector<NavigationRequestTableRecord> requests;
		// NavigationWorld recomputes this owned allocation estimate before
		// publication; producers cannot use it to bypass the byte budget.
		std::size_t chargedBytes = 0U;
	};

	class TERRAENGINE_API NavigationWorld final {
	public:
		explicit NavigationWorld(NavigationWorldHandle handle, std::size_t maximumRetiredRoots = 64U);

		NavigationWorld(const NavigationWorld&) = delete;
		NavigationWorld& operator=(const NavigationWorld&) = delete;

		[[nodiscard]] NavigationWorldHandle Handle() const noexcept;
		[[nodiscard]] std::shared_ptr<const NavigationWorldRoot> PublishedRoot() const;
		[[nodiscard]] NavigationWorldRoot CreateCandidate() const;

		// Publication is all-or-nothing: a stale candidate leaves the current root
		// and every source, coverage, tombstone, request, and agent row unchanged.
		[[nodiscard]] bool Publish(NavigationWorldRoot candidate, NavigationGeneration expectedRootGeneration);
		void CollectRetiredRoots();
		[[nodiscard]] std::size_t RetiredRootCount() const;

	private:
		struct RetiredRoot {
			std::shared_ptr<const NavigationWorldRoot> root;
			NavigationBudgetReservation reservation;
		};

		[[nodiscard]] static bool EstimateOwnedBytes(const NavigationWorldRoot& root, std::size_t& bytes) noexcept;

		NavigationWorldHandle handle_{};
		mutable std::mutex mutex_;
		NavigationBudget budget_;
		std::shared_ptr<const NavigationWorldRoot> published_;
		NavigationBudgetReservation publishedReservation_;
		// Old roots stay strongly owned until no worker or reader lease remains.
		// Collection removes only roots whose world is the sole remaining owner.
		std::vector<RetiredRoot> retired_;
		// The byte budget owns payload admission; this secondary root-count bound
		// prevents stalled readers from growing the reclamation ledger forever.
		std::size_t maximumRetiredRoots_ = 64U;
	};
}
