// GroundPortalGraph.h - immutable provider-neutral cross-shard connectivity.
#pragma once

#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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
	};

	struct GroundPortalSearchResult
	{
		NavigationOutcome outcome = NavigationOutcome::UnavailableCoverage;
		NavigationReason reason = NavigationReason::MissingNeighbor;
		std::vector<NavigationCoverageKey> coveragePath;
		std::size_t expandedNodes = 0U;

		[[nodiscard]] bool Succeeded() const noexcept { return outcome == NavigationOutcome::Complete; }
	};

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
		[[nodiscard]] GroundPortalGraphSnapshot AcquireSnapshot() const;

	private:
		mutable std::mutex publicationMutex_;
		std::shared_ptr<const std::vector<GroundPortalRecord>> portals_;
		std::uint64_t revision_ = 0U;
	};
}
