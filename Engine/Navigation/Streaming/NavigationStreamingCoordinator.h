// NavigationStreamingCoordinator.h - bounded coverage interest and leases.
#pragma once

#include "Navigation/NavigationWorld.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Navigation
{
	enum class NavigationStreamingReason : std::uint8_t
	{
		None,
		MissingCoverage,
		MissingNeighbor,
		LeaseExpired,
		Invalidated,
		BudgetExceeded
	};

	struct NavigationCoverageInterest
	{
		std::int64_t centerShardX = 0;
		std::int64_t centerShardZ = 0;
		std::int32_t minimumBand = 0;
		std::int32_t maximumBand = 0;
		std::uint32_t radiusShards = 0U;
		std::uint64_t profileBuildKey = 0U;
	};

	struct NavigationCoverageKeyHash
	{
		[[nodiscard]] std::size_t operator()(const NavigationCoverageKey& key) const noexcept
		{
			std::size_t hash = std::hash<std::int64_t>{}(key.shardX);
			hash ^= std::hash<std::int64_t>{}(key.shardZ) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
			hash ^= std::hash<std::int32_t>{}(key.verticalBand) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
			return hash ^ std::hash<std::uint64_t>{}(key.profileBuildKey);
		}
	};

	struct NavigationStreamingObservation
	{
		NavigationCoverageState state = NavigationCoverageState::Absent;
		NavigationStreamingReason reason = NavigationStreamingReason::MissingCoverage;
		NavigationCoverageKey key{};
		std::uint64_t leaseGeneration = 0U;
	};

	class TERRAENGINE_API NavigationStreamingCoordinator final
	{
	public:
		explicit NavigationStreamingCoordinator(std::size_t maximumCoverage = 4096U);
		[[nodiscard]] std::vector<NavigationCoverageKey> RequestInterest(const NavigationCoverageInterest& interest);
		[[nodiscard]] NavigationStreamingObservation Observe(const NavigationCoverageKey& key) const;
		[[nodiscard]] bool PublishReady(const NavigationCoverageKey& key, NavigationGeneration generation, std::size_t chargedBytes);
		[[nodiscard]] bool MarkInvalidated(const NavigationCoverageKey& key);
		[[nodiscard]] bool AcquireRouteLease(const NavigationCoverageKey& key, std::string leaseId, std::uint64_t expiresAtFrame);
		[[nodiscard]] bool ReleaseRouteLease(const NavigationCoverageKey& key, const std::string& leaseId);
		[[nodiscard]] std::size_t RetireExpired(std::uint64_t frame);

	private:
		struct Entry
		{
			NavigationCoverageRecord record{};
			std::unordered_map<std::string, std::uint64_t> leases;
		};

		mutable std::mutex mutex;
		std::size_t maximumCoverage = 0U;
		std::unordered_map<NavigationCoverageKey, Entry, NavigationCoverageKeyHash> entries;
	};
}
