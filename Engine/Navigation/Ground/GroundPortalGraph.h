// GroundPortalGraph.h - immutable provider-neutral cross-shard connectivity.
#pragma once

#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

namespace TerraEngine::Navigation::Ground
{
	struct GroundBoundaryInterval
	{
		NavigationCoverageKey coverage{};
		NavigationGeneration coverageGeneration = 0U;
		std::uint8_t boundaryAxis = 0U;
		bool positiveSide = false;
		std::uint32_t layer = 0U;
		double intervalStart = 0.0;
		double intervalEnd = 0.0;
		float minimumClearanceMetres = 0.0F;
		bool supported = false;
		bool clearanceValid = false;
		std::uint32_t capabilityMask = 0xFFFFFFFFU;
		float traversalCost = 1.0F;
		// Heights are sampled at this interval's endpoints from an actual exterior
		// polygon edge. They let the graph reject stacked floors in one vertical band.
		float heightAtIntervalStart = 0.0F;
		float heightAtIntervalEnd = 0.0F;
		float maximumStepHeightMetres = 0.0F;
	};

	struct GroundPortalRecord
	{
		NavigationCoverageKey fromCoverage{};
		NavigationCoverageKey toCoverage{};
		NavigationGeneration fromGeneration = 0U;
		NavigationGeneration toGeneration = 0U;
		std::uint8_t boundaryAxis = 0U;
		std::uint32_t layer = 0U;
		double intervalStart = 0.0;
		double intervalEnd = 0.0;
		float minimumClearanceMetres = 0.0F;
		std::uint32_t capabilityMask = 0xFFFFFFFFU;
		float traversalCost = 1.0F;
		bool supported = false;
		bool clearanceValid = false;
		// Directional portal heights are sampled at the shared overlap midpoint.
		// Search keeps them provider-local and gives each shard its own endpoint Y.
		float fromHeightMetres = 0.0F;
		float toHeightMetres = 0.0F;
	};

	struct GroundPortalSearchResult
	{
		NavigationOutcome outcome = NavigationOutcome::UnavailableCoverage;
		NavigationReason reason = NavigationReason::MissingNeighbor;
		std::vector<NavigationCoverageKey> coveragePath;
		std::size_t expandedNodes = 0U;

		[[nodiscard]] bool Succeeded() const noexcept { return outcome == NavigationOutcome::Complete; }
	};

	// Classifies an empty portal search against complete coverage evidence. This
	// deliberately ignores portal traversability: a connected component means a
	// profile-specific obstruction (such as low clearance), whereas a gap remains
	// a genuine unavailable-coverage neighbour. The copied key list and node
	// budget keep this worker-safe classification bounded and scene-independent.
	[[nodiscard]] GroundPortalSearchResult ClassifyPortalMissingNeighbor(const NavigationCoverageKey& start,
		const NavigationCoverageKey& goal, std::span<const NavigationCoverageKey> completeCoverage,
		std::size_t maximumNodes);

	class GroundPortalGraphSnapshot
	{
	public:
		GroundPortalGraphSnapshot() = default;

		[[nodiscard]] const std::vector<GroundPortalRecord>& Portals() const noexcept { return *portals_; }
		[[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
		[[nodiscard]] GroundPortalSearchResult Search(const NavigationCoverageKey& start,
			const NavigationCoverageKey& goal, const NavigationProfile& profile,
			std::size_t maximumNodes, std::size_t maximumOutputNodes,
			std::stop_token cancellation = {}, std::uint32_t capabilityMask = 0xFFFFFFFFU) const;

	private:
		friend class GroundPortalGraph;
		GroundPortalGraphSnapshot(std::shared_ptr<const std::vector<GroundPortalRecord>> portals, std::uint64_t revision)
			: portals_(std::move(portals)), revision_(revision) {}
		std::shared_ptr<const std::vector<GroundPortalRecord>> portals_ = std::make_shared<const std::vector<GroundPortalRecord>>();
		std::uint64_t revision_ = 0U;
	};

	class GroundPortalGraph
	{
	public:
		GroundPortalGraph();
		GroundPortalGraph(const GroundPortalGraph&) = delete;
		GroundPortalGraph& operator=(const GroundPortalGraph&) = delete;

		// Builds every opposite-side pair whose intervals overlap on the same
		// boundary axis/layer; no center-sample shortcut is permitted.
		[[nodiscard]] bool PublishBoundaryIntervals(const std::vector<GroundBoundaryInterval>& intervals,
			std::uint64_t expectedRevision);
		// Prepares the immutable portal candidate before locking, then executes the
		// caller's protected world-root commit while the expected portal revision is
		// still current. A false callback leaves both authorities unchanged.
		[[nodiscard]] bool PublishBoundaryIntervalsTransactionally(
			const std::vector<GroundBoundaryInterval>& intervals, std::uint64_t expectedRevision,
			const std::function<bool()>& commitWorldRoot);
		[[nodiscard]] GroundPortalGraphSnapshot AcquireSnapshot() const;

	private:
		mutable std::mutex publicationMutex_;
		std::shared_ptr<const std::vector<GroundPortalRecord>> portals_;
		std::uint64_t revision_ = 0U;
	};
}
