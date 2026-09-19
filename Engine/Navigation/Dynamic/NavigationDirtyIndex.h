// NavigationDirtyIndex.h - local dependency selection for dynamic source changes.

#pragma once

#include "Core/Export.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace TerraEngine::Navigation {
	enum class NavigationDynamicChangeKind : std::uint8_t {
		Created, Enabled, Disabled, Moved, Rotated, Scaled, Removed, Replaced, Reshaped
	};

	// This record owns only provider-neutral copied values. It can safely cross a
	// scheduler boundary without retaining a scene object or mutable component.
	struct NavigationDynamicSourceChange {
		NavigationUuid source{};
		// This deterministic content identity proves which immutable source snapshot
		// a worker may use. It can be a hash, so a newer value is not necessarily
		// numerically greater than an older value.
		NavigationGeneration sourceRevision = 0U;
		// The protected scene queue assigns this monotonic ordering token after a
		// transaction commits. Zero preserves legacy direct callers by falling back
		// to sourceRevision, but production bridges must provide a real sequence.
		NavigationGeneration changeSequence = 0U;
		NavigationGeneration physicalReadyRevision = 0U;
		// Bounds remain float-local to this canonical anchor. The exact int64 tile
		// origins keep dirty coverage aligned with large-world shard keys without
		// converting a far cell through a lossy global double.
		std::int64_t canonicalTileOriginX = 0;
		std::int64_t canonicalTileOriginZ = 0;
		std::int64_t canonicalVerticalBandOrigin = 0;
		NavigationDynamicChangeKind kind = NavigationDynamicChangeKind::Created;
		NavigationGeometryBounds previousBounds{};
		NavigationGeometryBounds currentBounds{};
	};

	struct NavigationDirtyIndexSettings {
		float clearanceExpansionMetres = 0.0F;
		float buildHaloMetres = 0.0F;
		float tileSizeMetres = 32.0F;
		float verticalBandMetres = 128.0F;
		std::size_t maximumPendingChanges = 16384U;
		std::size_t maximumDependenciesPerChange = 4096U;
	};

	// A protected caller supplies the largest active Recast profile border for
	// this receipt batch. The dirty index still keeps its configured baseline,
	// so legacy/direct callers remain conservative without knowing profiles.
	struct NavigationDirtyAdmissionExpansion {
		double minimumHorizontalExpansionMetres = 0.0;
		double minimumVerticalExpansionMetres = 0.0;
	};

	struct NavigationDirtyCoverageDependency {
		std::int64_t tileX = 0;
		std::int64_t tileZ = 0;
		std::int32_t verticalBand = 0;

		[[nodiscard]] constexpr bool operator==(const NavigationDirtyCoverageDependency&) const noexcept = default;
	};

	struct NavigationDirtySeamDependency {
		NavigationDirtyCoverageDependency first{};
		NavigationDirtyCoverageDependency second{};

		[[nodiscard]] constexpr bool operator==(const NavigationDirtySeamDependency&) const noexcept = default;
	};

	struct NavigationDynamicChangeReceipt {
		bool accepted = false;
		NavigationGeometryBounds affectedBounds{};
		std::size_t dirtyCoverageCount = 0U;
		std::size_t dirtySeamCount = 0U;
	};

	struct NavigationSourceTombstoneSnapshot {
		std::vector<NavigationSourceTombstone> entries;

		[[nodiscard]] bool Contains(NavigationUuid source, NavigationGeneration revision) const noexcept;
	};

	class TERRAENGINE_API NavigationDirtyIndex final {
	public:
		explicit NavigationDirtyIndex(NavigationDirtyIndexSettings settings);

		// The compatibility overload uses only the index's configured baseline.
		// NavigationSystem passes a profile-aware expansion at its protected receipt
		// boundary; the stored pending change always keeps its original bounds.
		[[nodiscard]] NavigationDynamicChangeReceipt CommitSourceChange(const NavigationDynamicSourceChange& change);
		[[nodiscard]] NavigationDynamicChangeReceipt CommitSourceChange(
			const NavigationDynamicSourceChange& change, NavigationDirtyAdmissionExpansion expansion);
		// A successful protected publication contains every dirty dependency up to
		// its global source revision. Retire that copied preparation backlog while
		// retaining source revisions and removal tombstones for stale-work checks.
		[[nodiscard]] bool AcknowledgePublishedThrough(NavigationGeneration sourceRevision);
		[[nodiscard]] NavigationSourceTombstoneSnapshot ObserveAtProtectedBoundary() const;
		[[nodiscard]] std::size_t PendingPreparationCount() const noexcept;
		[[nodiscard]] NavigationGeneration LatestChangeSequence() const noexcept;
		[[nodiscard]] NavigationGeneration LatestChangeSequence(NavigationUuid source) const noexcept;
		// Compatibility diagnostics retain the older spelling. It deliberately
		// returns a publication sequence, never a source-content hash.
		[[nodiscard]] NavigationGeneration LatestSourceRevision() const noexcept;
		[[nodiscard]] NavigationGeneration LatestSourceRevision(NavigationUuid source) const noexcept;
		// Returns a bounded value copy of every accepted change that has not yet
		// reached a complete protected publication. Dynamic tile reuse uses this
		// complete receipt chain to prove that a tile outside each profile-specific
		// Recast halo was not touched; callers must never retain this view of state.
		[[nodiscard]] std::vector<NavigationDynamicSourceChange> ObservePendingChanges() const;
		[[nodiscard]] const std::vector<NavigationDirtyCoverageDependency>& DirtyCoverage() const noexcept;
		[[nodiscard]] const std::vector<NavigationDirtySeamDependency>& DirtySeams() const noexcept;

	private:
		struct SourceRevisionRecord {
			NavigationUuid source{};
			NavigationGeneration revision = 0U;
			NavigationGeneration changeSequence = 0U;
			// Physical publications are independently revisioned. Retaining the last
			// observed value prevents an older terrain, resource, or streamed payload
			// from being lowered after a newer copied source revision was accepted.
			NavigationGeneration physicalReadyRevision = 0U;
		};

		NavigationDirtyIndexSettings settings_{};
		std::vector<NavigationDynamicSourceChange> pending_;
		std::vector<NavigationSourceTombstone> tombstones_;
		std::vector<SourceRevisionRecord> sourceRevisions_;
		std::vector<NavigationDirtyCoverageDependency> dirtyCoverage_;
		std::vector<NavigationDirtySeamDependency> dirtySeams_;
		NavigationGeneration latestRevision_ = 0U;
	};
}
