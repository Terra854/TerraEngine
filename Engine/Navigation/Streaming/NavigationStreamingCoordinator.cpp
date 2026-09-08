// NavigationStreamingCoordinator.cpp - bounded deterministic coverage state.
#include "NavigationStreamingCoordinator.h"

#include <algorithm>
#include <iterator>

namespace TerraEngine::Navigation
{
	NavigationStreamingCoordinator::NavigationStreamingCoordinator(std::size_t maximum)
		: maximumCoverage(maximum) {}

	std::vector<NavigationCoverageKey> NavigationStreamingCoordinator::RequestInterest(const NavigationCoverageInterest& interest)
	{
		std::vector<NavigationCoverageKey> requested;
		const std::int64_t radius = static_cast<std::int64_t>(interest.radiusShards);
		const std::int64_t diameter = radius * 2 + 1;
		const std::int64_t bands = static_cast<std::int64_t>(interest.maximumBand) - interest.minimumBand + 1;
		if(radius > 64 || bands <= 0 || diameter > 129 || diameter * diameter * bands > static_cast<std::int64_t>(maximumCoverage)) return requested;
		std::scoped_lock lock(mutex);
		requested.reserve(static_cast<std::size_t>(diameter * diameter * bands));
		for(std::int32_t band = interest.minimumBand; band <= interest.maximumBand; ++band)
			for(std::int64_t z = -radius; z <= radius; ++z)
				for(std::int64_t x = -radius; x <= radius; ++x)
				{
					NavigationCoverageKey key{interest.centerShardX + x, interest.centerShardZ + z, band, interest.profileBuildKey};
					requested.push_back(key);
					Entry& entry = entries[key];
					entry.record.key = key;
					if(entry.record.state == NavigationCoverageState::Absent) entry.record.state = NavigationCoverageState::Requested;
				}
		return requested;
	}

	NavigationStreamingObservation NavigationStreamingCoordinator::Observe(const NavigationCoverageKey& key) const
	{
		std::scoped_lock lock(mutex);
		const auto found = entries.find(key);
		if(found == entries.end()) return {NavigationCoverageState::Absent, NavigationStreamingReason::MissingCoverage, key, 0U};
		const Entry& entry = found->second;
		if(entry.record.state == NavigationCoverageState::Ready && entry.record.complete)
			return {entry.record.state, NavigationStreamingReason::None, key, entry.record.coverageGeneration};
		if(entry.record.state == NavigationCoverageState::Retiring) return {entry.record.state, NavigationStreamingReason::LeaseExpired, key, entry.record.coverageGeneration};
		if(entry.record.state == NavigationCoverageState::Dirty) return {entry.record.state, NavigationStreamingReason::Invalidated, key, entry.record.coverageGeneration};
		return {entry.record.state, NavigationStreamingReason::MissingCoverage, key, entry.record.coverageGeneration};
	}

	bool NavigationStreamingCoordinator::PublishReady(const NavigationCoverageKey& key, NavigationGeneration generation, std::size_t chargedBytes)
	{
		std::scoped_lock lock(mutex);
		if(entries.find(key) == entries.end() && entries.size() >= maximumCoverage) return false;
		Entry& entry = entries[key];
		if(entry.record.state == NavigationCoverageState::Retiring) return false;
		entry.record.key = key; entry.record.coverageGeneration = generation;
		entry.record.chargedBytes = chargedBytes; entry.record.complete = true; entry.record.state = NavigationCoverageState::Ready;
		return true;
	}

	bool NavigationStreamingCoordinator::MarkInvalidated(const NavigationCoverageKey& key)
	{
		std::scoped_lock lock(mutex);
		const auto found = entries.find(key);
		if(found == entries.end()) return false;
		found->second.record.state = NavigationCoverageState::Dirty;
		found->second.record.complete = false;
		return true;
	}

	bool NavigationStreamingCoordinator::AcquireRouteLease(const NavigationCoverageKey& key, std::string leaseId, std::uint64_t expiresAtFrame)
	{
		if(leaseId.empty()) return false;
		std::scoped_lock lock(mutex);
		const auto found = entries.find(key);
		if(found == entries.end() || found->second.record.state != NavigationCoverageState::Ready) return false;
		found->second.leases[std::move(leaseId)] = expiresAtFrame;
		return true;
	}

	bool NavigationStreamingCoordinator::ReleaseRouteLease(const NavigationCoverageKey& key, const std::string& leaseId)
	{
		std::scoped_lock lock(mutex);
		const auto found = entries.find(key);
		return found != entries.end() && found->second.leases.erase(leaseId) != 0U;
	}

	std::size_t NavigationStreamingCoordinator::RetireExpired(std::uint64_t frame)
	{
		std::scoped_lock lock(mutex);
		std::size_t retired = 0U;
		for(auto& [key, entry] : entries)
		{
			for(auto iterator = entry.leases.begin(); iterator != entry.leases.end();)
				iterator = iterator->second <= frame ? entry.leases.erase(iterator) : std::next(iterator);
			if(entry.leases.empty() && (entry.record.state == NavigationCoverageState::Dirty || entry.record.state == NavigationCoverageState::Ready))
			{
				entry.record.state = NavigationCoverageState::Retiring;
				++retired;
			}
		}
		return retired;
	}
}
