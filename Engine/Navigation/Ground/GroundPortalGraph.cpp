// GroundPortalGraph.cpp - deterministic immutable seam graph publication/search.

#include "GroundPortalGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Navigation::Ground
{
	namespace
	{
		bool SameCoverage(const NavigationCoverageKey& first, const NavigationCoverageKey& second) noexcept
		{
			return first == second;
		}

		// Boundary spans may share an axis and overlap while belonging to distant
		// shards. Only immediately adjacent coverage keys describe the same physical
		// shard seam; accepting any other pair would invent traversable topology.
		bool SharesPhysicalBoundary(const GroundBoundaryInterval& first,
			const GroundBoundaryInterval& second) noexcept
		{
			if(first.coverage.profileBuildKey != second.coverage.profileBuildKey
				|| first.coverage.verticalBand != second.coverage.verticalBand) return false;
			if(first.boundaryAxis == 0U) {
				return first.coverage.shardZ == second.coverage.shardZ
					&& (first.coverage.shardX + 1U == second.coverage.shardX
						|| second.coverage.shardX + 1U == first.coverage.shardX);
			}
			if(first.boundaryAxis == 1U) {
				return first.coverage.shardX == second.coverage.shardX
					&& (first.coverage.shardZ + 1U == second.coverage.shardZ
						|| second.coverage.shardZ + 1U == first.coverage.shardZ);
			}
			return false;
		}

		bool KeyLess(const NavigationCoverageKey& first, const NavigationCoverageKey& second) noexcept
		{
			if(first.shardX != second.shardX) return first.shardX < second.shardX;
			if(first.shardZ != second.shardZ) return first.shardZ < second.shardZ;
			if(first.verticalBand != second.verticalBand) return first.verticalBand < second.verticalBand;
			return first.profileBuildKey < second.profileBuildKey;
		}

		bool RecordLess(const GroundPortalRecord& first, const GroundPortalRecord& second) noexcept
		{
			if(KeyLess(first.fromCoverage, second.fromCoverage)) return true;
			if(KeyLess(second.fromCoverage, first.fromCoverage)) return false;
			if(KeyLess(first.toCoverage, second.toCoverage)) return true;
			if(KeyLess(second.toCoverage, first.toCoverage)) return false;
			if(first.layer != second.layer) return first.layer < second.layer;
			if(first.intervalStart != second.intervalStart) return first.intervalStart < second.intervalStart;
			return first.intervalEnd < second.intervalEnd;
		}

		bool ProfileAllows(const GroundPortalRecord& portal, const NavigationProfile& profile) noexcept
		{
			return portal.supported && portal.clearanceValid
				&& portal.minimumClearanceMetres >= static_cast<float>(profile.clearanceMarginMetres);
		}

		[[nodiscard]] bool HasCompatibleFloorHeight(const GroundBoundaryInterval& first,
			const GroundBoundaryInterval& second, double transverse, float& firstHeight,
			float& secondHeight) noexcept;

		[[nodiscard]] std::shared_ptr<const std::vector<GroundPortalRecord>> BuildPortalCandidate(
			const std::vector<GroundBoundaryInterval>& intervals) {
			auto next = std::make_shared<std::vector<GroundPortalRecord>>();
			for(std::size_t firstIndex = 0U; firstIndex < intervals.size(); ++firstIndex)
			{
				const GroundBoundaryInterval& first = intervals[firstIndex];
				if(first.intervalEnd <= first.intervalStart) continue;
				for(std::size_t secondIndex = firstIndex + 1U; secondIndex < intervals.size(); ++secondIndex)
				{
					const GroundBoundaryInterval& second = intervals[secondIndex];
					if(first.boundaryAxis != second.boundaryAxis || first.positiveSide == second.positiveSide
						|| first.layer != second.layer || SameCoverage(first.coverage, second.coverage)
						|| first.coverageGeneration == 0U || second.coverageGeneration == 0U)
						continue;
					if(!SharesPhysicalBoundary(first, second)) continue;
					const double overlapStart = std::max(first.intervalStart, second.intervalStart);
					const double overlapEnd = std::min(first.intervalEnd, second.intervalEnd);
					if(overlapEnd <= overlapStart) continue;
					const double overlapMidpoint = (overlapStart + overlapEnd) * 0.5;
					float firstHeight = 0.0F;
					float secondHeight = 0.0F;
					if(!HasCompatibleFloorHeight(first, second, overlapMidpoint, firstHeight, secondHeight)) continue;
					GroundPortalRecord portal{};
					portal.fromCoverage = first.coverage;
					portal.toCoverage = second.coverage;
					portal.fromGeneration = first.coverageGeneration;
					portal.toGeneration = second.coverageGeneration;
					portal.boundaryAxis = first.boundaryAxis;
					portal.layer = first.layer;
					portal.intervalStart = overlapStart;
					portal.intervalEnd = overlapEnd;
					portal.minimumClearanceMetres = std::min(first.minimumClearanceMetres, second.minimumClearanceMetres);
					portal.capabilityMask = first.capabilityMask & second.capabilityMask;
					portal.traversalCost = std::max(0.0F, first.traversalCost) + std::max(0.0F, second.traversalCost);
					portal.supported = first.supported && second.supported;
					portal.clearanceValid = first.clearanceValid && second.clearanceValid;
					portal.fromHeightMetres = firstHeight;
					portal.toHeightMetres = secondHeight;
					next->push_back(portal);
					std::swap(portal.fromCoverage, portal.toCoverage);
					std::swap(portal.fromGeneration, portal.toGeneration);
					std::swap(portal.fromHeightMetres, portal.toHeightMetres);
					next->push_back(portal);
				}
			}
			std::sort(next->begin(), next->end(), RecordLess);
			return next;
		}

		[[nodiscard]] float HeightAt(const GroundBoundaryInterval& interval, double transverse) noexcept
		{
			const double length = interval.intervalEnd - interval.intervalStart;
			if(!std::isfinite(length) || length <= 0.0
				|| !std::isfinite(interval.heightAtIntervalStart)
				|| !std::isfinite(interval.heightAtIntervalEnd)) {
				return std::numeric_limits<float>::quiet_NaN();
			}
			const double fraction = (transverse - interval.intervalStart) / length;
			return static_cast<float>(static_cast<double>(interval.heightAtIntervalStart)
				+ (static_cast<double>(interval.heightAtIntervalEnd) - interval.heightAtIntervalStart) * fraction);
		}

		[[nodiscard]] bool HasCompatibleFloorHeight(const GroundBoundaryInterval& first,
			const GroundBoundaryInterval& second, double transverse, float& firstHeight,
			float& secondHeight) noexcept
		{
			firstHeight = HeightAt(first, transverse);
			secondHeight = HeightAt(second, transverse);
			const float maximumStep = std::min(first.maximumStepHeightMetres, second.maximumStepHeightMetres);
			// Recast quantizes floor heights, so tolerate one small provider rounding
			// error. A portal never uses an average height: both endpoints retain their
			// physical floor values after this climb-compatible comparison succeeds.
			constexpr float kHeightComparisonEpsilon = 0.05F;
			return std::isfinite(firstHeight) && std::isfinite(secondHeight)
				&& std::isfinite(maximumStep) && maximumStep >= 0.0F
				&& std::abs(firstHeight - secondHeight) <= maximumStep + kHeightComparisonEpsilon;
		}
	}

	GroundPortalGraph::GroundPortalGraph()
		: portals_(std::make_shared<const std::vector<GroundPortalRecord>>()) {}

	bool GroundPortalGraph::PublishBoundaryIntervals(const std::vector<GroundBoundaryInterval>& intervals,
		std::uint64_t expectedRevision)
	{
		return PublishBoundaryIntervalsTransactionally(intervals, expectedRevision, [] { return true; });
	}

	bool GroundPortalGraph::PublishBoundaryIntervalsTransactionally(
		const std::vector<GroundBoundaryInterval>& intervals, std::uint64_t expectedRevision,
		const std::function<bool()>& commitWorldRoot)
	{
		if(!commitWorldRoot) return false;
		// Build and sort before taking the publication lock. This keeps the short
		// protected transaction free from allocation and means a rejected callback
		// cannot leave a partially assembled portal vector behind.
		const std::shared_ptr<const std::vector<GroundPortalRecord>> next = BuildPortalCandidate(intervals);
		std::lock_guard lock(publicationMutex_);
		if(expectedRevision != revision_ || revision_ == std::numeric_limits<std::uint64_t>::max()) return false;
		if(!commitWorldRoot()) return false;
		++revision_;
		portals_ = next;
		return true;
	}

	GroundPortalGraphSnapshot GroundPortalGraph::AcquireSnapshot() const
	{
		std::lock_guard lock(publicationMutex_);
		return GroundPortalGraphSnapshot(portals_, revision_);
	}

	GroundPortalSearchResult GroundPortalGraphSnapshot::Search(const NavigationCoverageKey& start,
		const NavigationCoverageKey& goal, const NavigationProfile& profile,
		std::size_t maximumNodes, std::size_t maximumOutputNodes, std::stop_token cancellation,
		std::uint32_t capabilityMask) const
	{
		GroundPortalSearchResult result{};
		if(!profile.Validate().accepted) {
			result.outcome = NavigationOutcome::InvalidRequest;
			result.reason = NavigationReason::InvalidDestination;
			return result;
		}
		if(SameCoverage(start, goal)) {
			result.outcome = NavigationOutcome::Complete;
			result.reason = NavigationReason::None;
			result.coveragePath.push_back(start);
			return result;
		}
		if(maximumNodes == 0U || maximumOutputNodes == 0U) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}

		struct SearchNode { NavigationCoverageKey key{}; double cost = 0.0; std::vector<NavigationCoverageKey> path; };
		std::vector<SearchNode> nodes;
		nodes.push_back({start, 0.0, {start}});
		while(!nodes.empty())
		{
			if(cancellation.stop_requested()) {
				result.outcome = NavigationOutcome::Cancelled;
				result.reason = NavigationReason::Cancelled;
				return result;
			}
			if(result.expandedNodes++ >= maximumNodes) {
				result.outcome = NavigationOutcome::BudgetExhausted;
				result.reason = NavigationReason::NodeLimit;
				return result;
			}
			std::size_t bestIndex = 0U;
			for(std::size_t index = 1U; index < nodes.size(); ++index)
				if(nodes[index].cost < nodes[bestIndex].cost
					|| nodes[index].cost == nodes[bestIndex].cost && KeyLess(nodes[index].key, nodes[bestIndex].key)) bestIndex = index;
			const SearchNode current = nodes[bestIndex];
			nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(bestIndex));
			if(SameCoverage(current.key, goal)) {
				result.coveragePath = current.path;
				if(result.coveragePath.size() > maximumOutputNodes) {
					result.coveragePath.clear(); result.outcome = NavigationOutcome::BudgetExhausted; result.reason = NavigationReason::OutputLimit; return result;
				}
				result.outcome = NavigationOutcome::Complete; result.reason = NavigationReason::None; return result;
			}
			for(const GroundPortalRecord& portal : *portals_)
				if(SameCoverage(portal.fromCoverage, current.key) && ProfileAllows(portal, profile)
					&& (portal.capabilityMask & capabilityMask) != 0U
					&& std::find(current.path.begin(), current.path.end(), portal.toCoverage) == current.path.end()) {
					std::vector<NavigationCoverageKey> path = current.path;
					path.push_back(portal.toCoverage);
					nodes.push_back({portal.toCoverage, current.cost + portal.traversalCost, std::move(path)});
				}
		}
		result.outcome = NavigationOutcome::UnavailableCoverage;
		result.reason = NavigationReason::MissingNeighbor;
		return result;
	}

	GroundPortalSearchResult ClassifyPortalMissingNeighbor(const NavigationCoverageKey& start,
		const NavigationCoverageKey& goal, std::span<const NavigationCoverageKey> completeCoverage,
		std::size_t maximumNodes)
	{
		GroundPortalSearchResult result{};
		const auto contains = [completeCoverage](const NavigationCoverageKey& key) {
			return std::find(completeCoverage.begin(), completeCoverage.end(), key) != completeCoverage.end();
		};
		if(maximumNodes == 0U || !contains(start) || !contains(goal)) return result;
		if(SameCoverage(start, goal)) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::MissingSupport;
			return result;
		}

		// Only cardinal neighbours in the same profile and vertical band form a
		// coverage component. Diagonal contact cannot prove a walkable seam.
		std::vector<NavigationCoverageKey> frontier{start};
		std::vector<NavigationCoverageKey> visited{start};
		std::size_t next = 0U;
		while(next < frontier.size() && visited.size() <= maximumNodes)
		{
			const NavigationCoverageKey current = frontier[next++];
			const auto tryAdd = [&](std::int64_t shardX, std::int64_t shardZ) {
				NavigationCoverageKey neighbour{shardX, shardZ, current.verticalBand, current.profileBuildKey};
				if(!contains(neighbour) || std::find(visited.begin(), visited.end(), neighbour) != visited.end()) return false;
				if(SameCoverage(neighbour, goal)) return true;
				visited.push_back(neighbour);
				frontier.push_back(neighbour);
				return false;
			};
			if(current.shardX != std::numeric_limits<std::int64_t>::min()
				&& tryAdd(current.shardX - 1LL, current.shardZ)) {
				result.outcome = NavigationOutcome::Unreachable;
				result.reason = NavigationReason::MissingSupport;
				return result;
			}
			if(current.shardX != std::numeric_limits<std::int64_t>::max()
				&& tryAdd(current.shardX + 1LL, current.shardZ)) {
				result.outcome = NavigationOutcome::Unreachable;
				result.reason = NavigationReason::MissingSupport;
				return result;
			}
			if(current.shardZ != std::numeric_limits<std::int64_t>::min()
				&& tryAdd(current.shardX, current.shardZ - 1LL)) {
				result.outcome = NavigationOutcome::Unreachable;
				result.reason = NavigationReason::MissingSupport;
				return result;
			}
			if(current.shardZ != std::numeric_limits<std::int64_t>::max()
				&& tryAdd(current.shardX, current.shardZ + 1LL)) {
				result.outcome = NavigationOutcome::Unreachable;
				result.reason = NavigationReason::MissingSupport;
				return result;
			}
		}
		return result;
	}
}
