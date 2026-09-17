// NavigationDirtyIndex.cpp - deterministic dirty coverage and tombstone tracking.

#include "Navigation/Dynamic/NavigationDirtyIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] bool IsFiniteBounds(const NavigationGeometryBounds& bounds) noexcept {
			return std::isfinite(bounds.minimum.x) && std::isfinite(bounds.minimum.y)
				&& std::isfinite(bounds.minimum.z) && std::isfinite(bounds.maximum.x)
				&& std::isfinite(bounds.maximum.y) && std::isfinite(bounds.maximum.z)
				&& bounds.minimum.x <= bounds.maximum.x && bounds.minimum.y <= bounds.maximum.y
				&& bounds.minimum.z <= bounds.maximum.z;
		}

		[[nodiscard]] NavigationGeometryBounds ExpandBounds(NavigationGeometryBounds bounds, float expansion) noexcept {
			bounds.minimum.x -= expansion; bounds.minimum.y -= expansion; bounds.minimum.z -= expansion;
			bounds.maximum.x += expansion; bounds.maximum.y += expansion; bounds.maximum.z += expansion;
			return bounds;
		}

		[[nodiscard]] NavigationGeometryBounds MergeBounds(
			const NavigationGeometryBounds& first, const NavigationGeometryBounds& second) noexcept {
			return {{std::min(first.minimum.x, second.minimum.x), std::min(first.minimum.y, second.minimum.y), std::min(first.minimum.z, second.minimum.z)},
				{std::max(first.maximum.x, second.maximum.x), std::max(first.maximum.y, second.maximum.y), std::max(first.maximum.z, second.maximum.z)}};
		}

		[[nodiscard]] bool UsesPreviousBounds(NavigationDynamicChangeKind kind) noexcept {
			return kind != NavigationDynamicChangeKind::Created && kind != NavigationDynamicChangeKind::Enabled;
		}

		[[nodiscard]] bool UsesCurrentBounds(NavigationDynamicChangeKind kind) noexcept {
			return kind != NavigationDynamicChangeKind::Removed && kind != NavigationDynamicChangeKind::Disabled;
		}

		[[nodiscard]] bool IsRemovalLike(NavigationDynamicChangeKind kind) noexcept {
			return kind == NavigationDynamicChangeKind::Removed || kind == NavigationDynamicChangeKind::Disabled;
		}

		[[nodiscard]] std::int32_t CellFor(float value, float size) noexcept {
			const double cell = std::floor(static_cast<double>(value) / static_cast<double>(size));
			return static_cast<std::int32_t>(std::clamp(cell,
				static_cast<double>(std::numeric_limits<std::int32_t>::min()),
				static_cast<double>(std::numeric_limits<std::int32_t>::max())));
		}
	}

	bool NavigationSourceTombstoneSnapshot::Contains(NavigationUuid source, NavigationGeneration revision) const noexcept {
		return std::any_of(entries.begin(), entries.end(), [source, revision](const NavigationSourceTombstone& entry) {
			return entry.sourceId == source.value && entry.removedRevision >= revision;
		});
	}

	NavigationDirtyIndex::NavigationDirtyIndex(NavigationDirtyIndexSettings settings) : settings_(settings) {}

	NavigationDynamicChangeReceipt NavigationDirtyIndex::CommitSourceChange(const NavigationDynamicSourceChange& change) {
		NavigationDynamicChangeReceipt receipt{};
		const bool settingsValid = std::isfinite(settings_.clearanceExpansionMetres)
			&& std::isfinite(settings_.buildHaloMetres) && std::isfinite(settings_.tileSizeMetres)
			&& std::isfinite(settings_.verticalBandMetres) && settings_.clearanceExpansionMetres >= 0.0F
			&& settings_.buildHaloMetres >= 0.0F && settings_.tileSizeMetres > 0.0F
			&& settings_.verticalBandMetres > 0.0F;
		const bool needsPrevious = UsesPreviousBounds(change.kind);
		const bool needsCurrent = UsesCurrentBounds(change.kind);
		if(!settingsValid || change.source.value == EMPTY_UUID || change.sourceRevision == 0U
			|| change.physicalReadyRevision == 0U || (needsPrevious && !IsFiniteBounds(change.previousBounds))
			|| (needsCurrent && !IsFiniteBounds(change.currentBounds))) return receipt;
		const auto sourceRevision = std::find_if(sourceRevisions_.begin(), sourceRevisions_.end(),
			[&change](const SourceRevisionRecord& entry) { return entry.source == change.source; });
		// A source identity has two ordered observations: the logical source value
		// and its checked physical input. Logical changes may reuse unchanged
		// physical geometry (for example a transform), but a late companion,
		// terrain, or streaming result must never regress that physical evidence.
		if((sourceRevision != sourceRevisions_.end()
			&& (change.sourceRevision <= sourceRevision->revision
				|| change.physicalReadyRevision < sourceRevision->physicalReadyRevision))
			|| pending_.size() >= settings_.maximumPendingChanges) return receipt;

		NavigationGeometryBounds affected = needsPrevious ? change.previousBounds : change.currentBounds;
		if(needsPrevious && needsCurrent) affected = MergeBounds(change.previousBounds, change.currentBounds);
		affected = ExpandBounds(affected, settings_.clearanceExpansionMetres + settings_.buildHaloMetres);
		const std::int32_t minimumX = CellFor(affected.minimum.x, settings_.tileSizeMetres);
		const std::int32_t maximumX = CellFor(affected.maximum.x, settings_.tileSizeMetres);
		const std::int32_t minimumZ = CellFor(affected.minimum.z, settings_.tileSizeMetres);
		const std::int32_t maximumZ = CellFor(affected.maximum.z, settings_.tileSizeMetres);
		const std::int32_t minimumBand = CellFor(affected.minimum.y, settings_.verticalBandMetres);
		const std::int32_t maximumBand = CellFor(affected.maximum.y, settings_.verticalBandMetres);
		const std::uint64_t xCount = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximumX) - minimumX + 1);
		const std::uint64_t zCount = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximumZ) - minimumZ + 1);
		const std::uint64_t bandCount = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximumBand) - minimumBand + 1);
		if(xCount == 0U || zCount == 0U || bandCount == 0U || xCount > settings_.maximumDependenciesPerChange
			|| zCount > settings_.maximumDependenciesPerChange || bandCount > settings_.maximumDependenciesPerChange
			|| xCount * zCount > settings_.maximumDependenciesPerChange
			|| xCount * zCount * bandCount > settings_.maximumDependenciesPerChange) return receipt;

		std::vector<NavigationDirtyCoverageDependency> localCoverage;
		for(std::int32_t band = minimumBand; band <= maximumBand; ++band)
			for(std::int32_t z = minimumZ; z <= maximumZ; ++z)
				for(std::int32_t x = minimumX; x <= maximumX; ++x) localCoverage.push_back({x, z, band});
		std::sort(localCoverage.begin(), localCoverage.end(), [](const auto& left, const auto& right) {
			return std::tie(left.verticalBand, left.tileZ, left.tileX) < std::tie(right.verticalBand, right.tileZ, right.tileX);
		});
		localCoverage.erase(std::unique(localCoverage.begin(), localCoverage.end()), localCoverage.end());

		std::vector<NavigationDirtySeamDependency> localSeams;
		for(const NavigationDirtyCoverageDependency& tile : localCoverage) {
			// East and north edges are canonical owners, avoiding duplicate seam jobs.
			localSeams.push_back({tile, {tile.tileX + 1, tile.tileZ, tile.verticalBand}});
			localSeams.push_back({tile, {tile.tileX, tile.tileZ + 1, tile.verticalBand}});
		}
		pending_.push_back(change);
		dirtyCoverage_.insert(dirtyCoverage_.end(), localCoverage.begin(), localCoverage.end());
		dirtySeams_.insert(dirtySeams_.end(), localSeams.begin(), localSeams.end());
		std::sort(dirtyCoverage_.begin(), dirtyCoverage_.end(), [](const auto& left, const auto& right) {
			return std::tie(left.verticalBand, left.tileZ, left.tileX) < std::tie(right.verticalBand, right.tileZ, right.tileX);
		});
		dirtyCoverage_.erase(std::unique(dirtyCoverage_.begin(), dirtyCoverage_.end()), dirtyCoverage_.end());

		if(sourceRevision == sourceRevisions_.end()) {
			sourceRevisions_.push_back({change.source, change.sourceRevision, change.physicalReadyRevision});
		} else {
			sourceRevision->revision = change.sourceRevision;
			sourceRevision->physicalReadyRevision = change.physicalReadyRevision;
		}
		latestRevision_ = std::max(latestRevision_, change.sourceRevision);
		if(IsRemovalLike(change.kind)) {
			auto tombstone = std::find_if(tombstones_.begin(), tombstones_.end(),
				[&change](const NavigationSourceTombstone& entry) { return entry.sourceId == change.source.value; });
			if(tombstone == tombstones_.end()) tombstones_.push_back({change.source.value, change.sourceRevision});
			else tombstone->removedRevision = std::max(tombstone->removedRevision, change.sourceRevision);
		}
		receipt = {true, affected, localCoverage.size(), localSeams.size()};
		return receipt;
	}

	bool NavigationDirtyIndex::AcknowledgePublishedThrough(NavigationGeneration sourceRevision) {
		// Publication accepts only the globally latest copied source revision. If a
		// newer change arrived after preparation, its candidate is stale and must not
		// retire any dependency that the newer publication still needs.
		if(sourceRevision == 0U || sourceRevision != latestRevision_ || pending_.empty()) return false;
		pending_.clear();
		dirtyCoverage_.clear();
		dirtySeams_.clear();
		return true;
	}

	NavigationSourceTombstoneSnapshot NavigationDirtyIndex::ObserveAtProtectedBoundary() const { return {tombstones_}; }
	std::size_t NavigationDirtyIndex::PendingPreparationCount() const noexcept { return pending_.size(); }
	NavigationGeneration NavigationDirtyIndex::LatestSourceRevision() const noexcept { return latestRevision_; }
	NavigationGeneration NavigationDirtyIndex::LatestSourceRevision(NavigationUuid source) const noexcept {
		const auto found = std::find_if(sourceRevisions_.begin(), sourceRevisions_.end(),
			[source](const SourceRevisionRecord& entry) { return entry.source == source; });
		return found == sourceRevisions_.end() ? 0U : found->revision;
	}
	const std::vector<NavigationDirtyCoverageDependency>& NavigationDirtyIndex::DirtyCoverage() const noexcept { return dirtyCoverage_; }
	const std::vector<NavigationDirtySeamDependency>& NavigationDirtyIndex::DirtySeams() const noexcept { return dirtySeams_; }
}
