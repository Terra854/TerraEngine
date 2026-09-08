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
		NavigationGeneration sourceRevision = 0U;
		NavigationGeneration physicalReadyRevision = 0U;
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

	struct NavigationDirtyCoverageDependency {
		std::int32_t tileX = 0;
		std::int32_t tileZ = 0;
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

		[[nodiscard]] NavigationDynamicChangeReceipt CommitSourceChange(const NavigationDynamicSourceChange& change);
		[[nodiscard]] NavigationSourceTombstoneSnapshot ObserveAtProtectedBoundary() const;
		[[nodiscard]] std::size_t PendingPreparationCount() const noexcept;
		[[nodiscard]] NavigationGeneration LatestSourceRevision() const noexcept;
		[[nodiscard]] NavigationGeneration LatestSourceRevision(NavigationUuid source) const noexcept;
		[[nodiscard]] const std::vector<NavigationDirtyCoverageDependency>& DirtyCoverage() const noexcept;
		[[nodiscard]] const std::vector<NavigationDirtySeamDependency>& DirtySeams() const noexcept;

	private:
		struct SourceRevisionRecord {
			NavigationUuid source{};
			NavigationGeneration revision = 0U;
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
