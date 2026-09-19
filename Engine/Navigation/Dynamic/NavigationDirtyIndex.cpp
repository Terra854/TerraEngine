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

		[[nodiscard]] std::optional<NavigationGeometryBounds> ExpandBounds(
			const NavigationGeometryBounds& bounds, double horizontalExpansion, double verticalExpansion) noexcept {
			// Compute in double precision so a valid profile halo cannot silently wrap
			// a float-local source bound before CellFor validates the result.
			if(!std::isfinite(horizontalExpansion) || !std::isfinite(verticalExpansion)
				|| horizontalExpansion < 0.0 || verticalExpansion < 0.0) return std::nullopt;
			const double minimumX = static_cast<double>(bounds.minimum.x) - horizontalExpansion;
			const double maximumX = static_cast<double>(bounds.maximum.x) + horizontalExpansion;
			const double minimumY = static_cast<double>(bounds.minimum.y) - verticalExpansion;
			const double maximumY = static_cast<double>(bounds.maximum.y) + verticalExpansion;
			const double minimumZ = static_cast<double>(bounds.minimum.z) - horizontalExpansion;
			const double maximumZ = static_cast<double>(bounds.maximum.z) + horizontalExpansion;
			const double maximumFloat = static_cast<double>(std::numeric_limits<float>::max());
			if(!std::isfinite(minimumX) || !std::isfinite(maximumX)
				|| !std::isfinite(minimumY) || !std::isfinite(maximumY)
				|| !std::isfinite(minimumZ) || !std::isfinite(maximumZ)
				|| minimumX < -maximumFloat || maximumX > maximumFloat
				|| minimumY < -maximumFloat || maximumY > maximumFloat
				|| minimumZ < -maximumFloat || maximumZ > maximumFloat) return std::nullopt;
			auto roundMinimumOutward = [](double value) noexcept {
				float rounded = static_cast<float>(value);
				// A narrowing conversion can round an exact lower edge upward across a
				// tile boundary. Step outward only in that direction so every tile the
				// double halo reaches remains dirty after float-local indexing.
				if(static_cast<double>(rounded) > value)
					rounded = std::nextafter(rounded, -std::numeric_limits<float>::infinity());
				return rounded;
			};
			auto roundMaximumOutward = [](double value) noexcept {
				float rounded = static_cast<float>(value);
				if(static_cast<double>(rounded) < value)
					rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
				return rounded;
			};
			return NavigationGeometryBounds{{roundMinimumOutward(minimumX), roundMinimumOutward(minimumY),
				roundMinimumOutward(minimumZ)}, {roundMaximumOutward(maximumX), roundMaximumOutward(maximumY),
				roundMaximumOutward(maximumZ)}};
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

		[[nodiscard]] NavigationGeneration ChangeSequence(const NavigationDynamicSourceChange& change) noexcept {
			// Direct native callers predate committed scene receipts. Keep their
			// source-revision ordering only as a compatibility fallback; production
			// changes always carry a separate monotonic sequence.
			return change.changeSequence != 0U ? change.changeSequence : change.sourceRevision;
		}

		[[nodiscard]] std::optional<std::int64_t> CellFor(float value, float size) noexcept {
			if(!std::isfinite(value) || !std::isfinite(size) || size <= 0.0F) return std::nullopt;
			const double cell = std::floor(static_cast<double>(value) / static_cast<double>(size));
			// The positive limit is exclusive: `double(INT64_MAX)` rounds to 2^63,
			// which cannot be represented by a signed int64 after conversion.
			constexpr double kMinimumInt64 = -9223372036854775808.0;
			constexpr double kOnePastMaximumInt64 = 9223372036854775808.0;
			if(cell < kMinimumInt64 || cell >= kOnePastMaximumInt64) return std::nullopt;
			return static_cast<std::int64_t>(cell);
		}

		[[nodiscard]] std::optional<std::int64_t> AddExact(std::int64_t first, std::int64_t second) noexcept {
			if((second > 0 && first > std::numeric_limits<std::int64_t>::max() - second)
				|| (second < 0 && first < std::numeric_limits<std::int64_t>::min() - second)) return std::nullopt;
			return first + second;
		}

		[[nodiscard]] std::optional<std::uint64_t> InclusiveCount(
			std::int64_t minimum, std::int64_t maximum) noexcept {
			if(maximum < minimum) return std::nullopt;
			// Unsigned subtraction is defined modulo 2^64 and exactly represents the
			// non-negative signed distance. A full int64 range has no representable
			// inclusive count, so reject it instead of overflowing the dependency cap.
			const std::uint64_t distance = static_cast<std::uint64_t>(maximum)
				- static_cast<std::uint64_t>(minimum);
			if(distance == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
			return distance + 1U;
		}

		[[nodiscard]] bool ProductExceeds(std::uint64_t first, std::uint64_t second,
			std::uint64_t limit) noexcept {
			return first == 0U || second == 0U || first > limit / second;
		}
	}

	bool NavigationSourceTombstoneSnapshot::Contains(NavigationUuid source, NavigationGeneration revision) const noexcept {
		return std::any_of(entries.begin(), entries.end(), [source, revision](const NavigationSourceTombstone& entry) {
			return entry.sourceId == source.value && entry.removedRevision >= revision;
		});
	}

	NavigationDirtyIndex::NavigationDirtyIndex(NavigationDirtyIndexSettings settings) : settings_(settings) {}

	NavigationDynamicChangeReceipt NavigationDirtyIndex::CommitSourceChange(const NavigationDynamicSourceChange& change) {
		return CommitSourceChange(change, {});
	}

	NavigationDynamicChangeReceipt NavigationDirtyIndex::CommitSourceChange(
		const NavigationDynamicSourceChange& change, NavigationDirtyAdmissionExpansion expansion) {
		NavigationDynamicChangeReceipt receipt{};
		const bool settingsValid = std::isfinite(settings_.clearanceExpansionMetres)
			&& std::isfinite(settings_.buildHaloMetres) && std::isfinite(settings_.tileSizeMetres)
			&& std::isfinite(settings_.verticalBandMetres) && settings_.clearanceExpansionMetres >= 0.0F
			&& settings_.buildHaloMetres >= 0.0F && settings_.tileSizeMetres > 0.0F
			&& settings_.verticalBandMetres > 0.0F;
		const bool needsPrevious = UsesPreviousBounds(change.kind);
		const bool needsCurrent = UsesCurrentBounds(change.kind);
		const NavigationGeneration changeSequence = ChangeSequence(change);
		if(!settingsValid || change.source.value == EMPTY_UUID || change.sourceRevision == 0U || changeSequence == 0U
			|| change.physicalReadyRevision == 0U || (needsPrevious && !IsFiniteBounds(change.previousBounds))
			|| (needsCurrent && !IsFiniteBounds(change.currentBounds))
			|| !std::isfinite(expansion.minimumHorizontalExpansionMetres)
			|| !std::isfinite(expansion.minimumVerticalExpansionMetres)
			|| expansion.minimumHorizontalExpansionMetres < 0.0
			|| expansion.minimumVerticalExpansionMetres < 0.0) return receipt;
		const auto sourceRevision = std::find_if(sourceRevisions_.begin(), sourceRevisions_.end(),
			[&change](const SourceRevisionRecord& entry) { return entry.source == change.source; });
		// The receipt sequence orders observations. Content identity may be a FNV
		// hash, so numerical sourceRevision ordering would reject a legitimate later
		// transform whose hash happens to be smaller.
		if((sourceRevision != sourceRevisions_.end()
			&& (changeSequence <= sourceRevision->changeSequence
				|| change.physicalReadyRevision < sourceRevision->physicalReadyRevision))
			|| pending_.size() >= settings_.maximumPendingChanges) return receipt;

		NavigationGeometryBounds affected = needsPrevious ? change.previousBounds : change.currentBounds;
		if(needsPrevious && needsCurrent) affected = MergeBounds(change.previousBounds, change.currentBounds);
		const double baselineExpansion = static_cast<double>(settings_.clearanceExpansionMetres)
			+ static_cast<double>(settings_.buildHaloMetres);
		const std::optional<NavigationGeometryBounds> expanded = ExpandBounds(affected,
			std::max(baselineExpansion, expansion.minimumHorizontalExpansionMetres),
			std::max(baselineExpansion, expansion.minimumVerticalExpansionMetres));
		if(!expanded) return receipt;
		affected = *expanded;
		const auto localMinimumX = CellFor(affected.minimum.x, settings_.tileSizeMetres);
		const auto localMaximumX = CellFor(affected.maximum.x, settings_.tileSizeMetres);
		const auto localMinimumZ = CellFor(affected.minimum.z, settings_.tileSizeMetres);
		const auto localMaximumZ = CellFor(affected.maximum.z, settings_.tileSizeMetres);
		const auto localMinimumBand = CellFor(affected.minimum.y, settings_.verticalBandMetres);
		const auto localMaximumBand = CellFor(affected.maximum.y, settings_.verticalBandMetres);
		if(!localMinimumX || !localMaximumX || !localMinimumZ || !localMaximumZ
			|| !localMinimumBand || !localMaximumBand) return receipt;
		const auto minimumX = AddExact(change.canonicalTileOriginX, *localMinimumX);
		const auto maximumX = AddExact(change.canonicalTileOriginX, *localMaximumX);
		const auto minimumZ = AddExact(change.canonicalTileOriginZ, *localMinimumZ);
		const auto maximumZ = AddExact(change.canonicalTileOriginZ, *localMaximumZ);
		const auto minimumBand64 = AddExact(change.canonicalVerticalBandOrigin, *localMinimumBand);
		const auto maximumBand64 = AddExact(change.canonicalVerticalBandOrigin, *localMaximumBand);
		if(!minimumX || !maximumX || !minimumZ || !maximumZ || !minimumBand64 || !maximumBand64
			|| *minimumBand64 < std::numeric_limits<std::int32_t>::min()
			|| *maximumBand64 > std::numeric_limits<std::int32_t>::max()) return receipt;
		const std::int32_t minimumBand = static_cast<std::int32_t>(*minimumBand64);
		const std::int32_t maximumBand = static_cast<std::int32_t>(*maximumBand64);
		const std::optional<std::uint64_t> xCount = InclusiveCount(*minimumX, *maximumX);
		const std::optional<std::uint64_t> zCount = InclusiveCount(*minimumZ, *maximumZ);
		const std::optional<std::uint64_t> bandCount = InclusiveCount(minimumBand, maximumBand);
		const std::uint64_t maximumDependencies = static_cast<std::uint64_t>(settings_.maximumDependenciesPerChange);
		if(!xCount || !zCount || !bandCount || *xCount == 0U || *zCount == 0U || *bandCount == 0U
			|| *xCount > maximumDependencies || *zCount > maximumDependencies || *bandCount > maximumDependencies
			|| ProductExceeds(*xCount, *zCount, maximumDependencies)
			|| ProductExceeds(*xCount * *zCount, *bandCount, maximumDependencies)) return receipt;

		std::vector<NavigationDirtyCoverageDependency> localCoverage;
		for(std::int32_t band = minimumBand;; ++band) {
			for(std::int64_t z = *minimumZ;; ++z) {
				for(std::int64_t x = *minimumX;; ++x) {
					localCoverage.push_back({x, z, band});
					if(x == *maximumX) break;
				}
				if(z == *maximumZ) break;
			}
			if(band == maximumBand) break;
		}
		std::sort(localCoverage.begin(), localCoverage.end(), [](const auto& left, const auto& right) {
			return std::tie(left.verticalBand, left.tileZ, left.tileX) < std::tie(right.verticalBand, right.tileZ, right.tileX);
		});
		localCoverage.erase(std::unique(localCoverage.begin(), localCoverage.end()), localCoverage.end());

		std::vector<NavigationDirtySeamDependency> localSeams;
		for(const NavigationDirtyCoverageDependency& tile : localCoverage) {
			// East and north edges are canonical owners, avoiding duplicate seam jobs.
			if(tile.tileX == std::numeric_limits<std::int64_t>::max()
				|| tile.tileZ == std::numeric_limits<std::int64_t>::max()) return receipt;
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
			sourceRevisions_.push_back({change.source, change.sourceRevision, changeSequence, change.physicalReadyRevision});
		} else {
			sourceRevision->revision = change.sourceRevision;
			sourceRevision->changeSequence = changeSequence;
			sourceRevision->physicalReadyRevision = change.physicalReadyRevision;
		}
		latestRevision_ = std::max(latestRevision_, changeSequence);
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
	NavigationGeneration NavigationDirtyIndex::LatestChangeSequence() const noexcept { return latestRevision_; }
	NavigationGeneration NavigationDirtyIndex::LatestChangeSequence(NavigationUuid source) const noexcept {
		const auto found = std::find_if(sourceRevisions_.begin(), sourceRevisions_.end(),
			[source](const SourceRevisionRecord& entry) { return entry.source == source; });
		return found == sourceRevisions_.end() ? 0U : found->changeSequence;
	}
	NavigationGeneration NavigationDirtyIndex::LatestSourceRevision() const noexcept { return LatestChangeSequence(); }
	NavigationGeneration NavigationDirtyIndex::LatestSourceRevision(NavigationUuid source) const noexcept {
		return LatestChangeSequence(source);
	}

	std::vector<NavigationDynamicSourceChange> NavigationDirtyIndex::ObservePendingChanges() const {
		// The index owns the mutable receipt deque. Returning a value copy prevents a
		// detached worker from observing acknowledgement or later receipts half-way
		// through its profile-specific halo calculation.
		return pending_;
	}

	const std::vector<NavigationDirtyCoverageDependency>& NavigationDirtyIndex::DirtyCoverage() const noexcept { return dirtyCoverage_; }
	const std::vector<NavigationDirtySeamDependency>& NavigationDirtyIndex::DirtySeams() const noexcept { return dirtySeams_; }
}
