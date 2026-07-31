#include "VirtualGeometryRasterBins.h"
#include <algorithm>
namespace TerraEngine::Renderer
{
	namespace
	{
		std::vector<VirtualGeometryRasterCandidate> OrderCandidates(
			const std::vector<VirtualGeometryRasterCandidate>& candidates)
		{
			std::vector<VirtualGeometryRasterCandidate> ordered = candidates;
			std::stable_sort(ordered.begin(), ordered.end(),
				[](const VirtualGeometryRasterCandidate& left,
					const VirtualGeometryRasterCandidate& right)
				{
					return left.stableId < right.stableId;
				});
			return ordered;
		}

		void CountRoute(
			VirtualGeometryRasterBinPlan& plan,
			VirtualGeometryRasterRoute route)
		{
			switch(route)
			{
			case VirtualGeometryRasterRoute::Hardware:
				++plan.hardwareCount;
				break;
			case VirtualGeometryRasterRoute::Compute:
				++plan.computeCount;
				break;
			case VirtualGeometryRasterRoute::Indexed:
			case VirtualGeometryRasterRoute::Fallback:
				++plan.indexedCount;
				break;
			}
		}

		bool SupportsAcceleratedRoute(
			const VirtualGeometryRasterCandidate& candidate,
			std::uint32_t materialBinCapacity,
			VirtualGeometryRasterRoute route)
		{
			if(candidate.materialBin >= materialBinCapacity)
			{
				return false;
			}
			return route == VirtualGeometryRasterRoute::Hardware
				? candidate.hardwareSupported
				: candidate.computeSupported;
		}
	}

	VirtualGeometryRasterBinPlan BuildVirtualGeometryRasterBinPlan(
		const std::vector<VirtualGeometryRasterCandidate>& candidates,
		std::uint32_t materialBinCapacity)
	{
		VirtualGeometryRasterBinPlan plan{};
		const std::vector<VirtualGeometryRasterCandidate> ordered =
			OrderCandidates(candidates);
		for(const VirtualGeometryRasterCandidate& candidate : ordered)
		{
			plan.routeStableIds.push_back(candidate.stableId);
			plan.routeSourceOrdinals.push_back(candidate.sourceOrdinal);
			VirtualGeometryRasterRoute route =
				VirtualGeometryRasterRoute::Indexed;
			if(candidate.materialBin >= materialBinCapacity)
			{
				route = VirtualGeometryRasterRoute::Fallback;
				++plan.overflowCount;
				plan.failOpen = true;
				plan.fallbackStableIds.push_back(candidate.stableId);
			}
			else if(candidate.hardwareSupported)
			{
				route = VirtualGeometryRasterRoute::Hardware;
			}
			else if(candidate.computeSupported)
			{
				route = VirtualGeometryRasterRoute::Compute;
			}
			else
			{
				plan.failOpen = true;
				plan.fallbackStableIds.push_back(candidate.stableId);
			}
			plan.routes.push_back(route);
			CountRoute(plan, route);
		}
		return plan;
	}

	VirtualGeometryRasterBinPlan BuildVirtualGeometryHybridRasterBinPlan(
		const std::vector<VirtualGeometryRasterCandidate>& candidates,
		std::uint32_t materialBinCapacity)
	{
		VirtualGeometryRasterBinPlan plan{};
		const std::vector<VirtualGeometryRasterCandidate> ordered =
			OrderCandidates(candidates);
		plan.routes.assign(
			ordered.size(),
			VirtualGeometryRasterRoute::Indexed);
		for(const VirtualGeometryRasterCandidate& candidate : ordered)
		{
			plan.routeStableIds.push_back(candidate.stableId);
			plan.routeSourceOrdinals.push_back(candidate.sourceOrdinal);
			if(candidate.materialBin >= materialBinCapacity)
			{
				++plan.overflowCount;
				plan.failOpen = true;
				plan.fallbackStableIds.push_back(candidate.stableId);
			}
			else if(!candidate.hardwareSupported && !candidate.computeSupported)
			{
				// Indexed is the complete visible path when neither accelerated
				// route can safely render this packet.
				plan.failOpen = true;
				plan.fallbackStableIds.push_back(candidate.stableId);
			}
		}

		// Pick a distinct hardware/compute pair. Trying every stable-ordered pair
		// avoids a greedy corner case where the first packet supports both routes
		// but a later packet supports only one of them.
		std::size_t hardwareIndex = ordered.size();
		std::size_t computeIndex = ordered.size();
		for(std::size_t hardwareCandidate = 0;
			hardwareCandidate < ordered.size()
				&& hardwareIndex == ordered.size();
			++hardwareCandidate)
		{
			if(!SupportsAcceleratedRoute(
				ordered[hardwareCandidate],
				materialBinCapacity,
				VirtualGeometryRasterRoute::Hardware))
			{
				continue;
			}
			for(std::size_t computeCandidate = 0;
				computeCandidate < ordered.size();
				++computeCandidate)
			{
				if(computeCandidate != hardwareCandidate
					&& SupportsAcceleratedRoute(
						ordered[computeCandidate],
						materialBinCapacity,
						VirtualGeometryRasterRoute::Compute))
				{
					hardwareIndex = hardwareCandidate;
					computeIndex = computeCandidate;
					break;
				}
			}
		}

		if(hardwareIndex == ordered.size())
		{
			// Complete three-route coverage may be impossible on a constrained
			// adapter/material set. Still exercise every accelerated route that a
			// real candidate supports before leaving the rest safely indexed.
			for(std::size_t candidateIndex = 0;
				candidateIndex < ordered.size();
				++candidateIndex)
			{
				if(SupportsAcceleratedRoute(
					ordered[candidateIndex],
					materialBinCapacity,
					VirtualGeometryRasterRoute::Hardware))
				{
					hardwareIndex = candidateIndex;
					break;
				}
			}
			for(std::size_t candidateIndex = 0;
				candidateIndex < ordered.size();
				++candidateIndex)
			{
				if(candidateIndex != hardwareIndex
					&& SupportsAcceleratedRoute(
						ordered[candidateIndex],
						materialBinCapacity,
						VirtualGeometryRasterRoute::Compute))
				{
					computeIndex = candidateIndex;
					break;
				}
			}
		}

		if(hardwareIndex != ordered.size())
		{
			plan.routes[hardwareIndex] =
				VirtualGeometryRasterRoute::Hardware;
		}
		if(computeIndex != ordered.size())
		{
			plan.routes[computeIndex] =
				VirtualGeometryRasterRoute::Compute;
		}

		// All unassigned packets remain on the real indexed route. With three or
		// more packets, a supported distinct accelerated pair therefore guarantees
		// Hardware + Compute + Indexed coverage without inventing a hybrid enum.
		for(const VirtualGeometryRasterRoute route : plan.routes)
		{
			CountRoute(plan, route);
		}
		plan.hybridCoverageComplete =
			plan.hardwareCount > 0u
			&& plan.computeCount > 0u
			&& plan.indexedCount > 0u;
		if(!plan.hybridCoverageComplete)
		{
			// Incomplete route coverage is a validation limitation, never a reason
			// to hide geometry. Indexed routes above keep every packet visible.
			plan.failOpen = true;
		}
		return plan;
	}
} // namespace TerraEngine::Renderer
