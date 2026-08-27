// TerrainSurfaceCache.cpp -- worker-safe terrain control atlas planning.

#include "Renderer/Terrain/TerrainSurfaceCache.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"

#include <algorithm>
#include <atomic>

namespace TerraEngine::Renderer::Terrain
{
	// Cached pages store controls and resolved PBR outputs, never a divergent layer
	// record. Both cached and direct consumers therefore retain the frozen ABI.
	static_assert(alignof(TerrainGpuLayerBinding) == 16u && sizeof(TerrainGpuLayerBinding) == 96u);

	const TerrainDiagnosticSurfaceMaterial& GetTerrainDiagnosticSurfaceMaterial() noexcept
	{
		static constexpr TerrainDiagnosticSurfaceMaterial material{};
		return material;
	}

	TerrainSurfaceControlResolution ResolveTerrainSurfaceControlSample(
		const TerraEngine::Terrain::TerrainWeightSample* sample,
		const TerraEngine::Terrain::TerrainLayerStack& stack) noexcept
	{
		TerrainSurfaceControlResolution result{};
		if(sample == nullptr)
		{
			// A missing required control page is different from an authored zero
			// sample, but both resolve to the same finite diagnostic contributor.
			const TerraEngine::Terrain::TerrainWeightSample empty{};
			result.weights = TerraEngine::Terrain::ResolveTerrainWeightSample(empty, stack);
			result.requiredControlMissing = true;
			return result;
		}
		result.weights = TerraEngine::Terrain::ResolveTerrainWeightSample(*sample, stack);
		return result;
	}

	bool TerrainSurfacePageKey::IsComplete() const noexcept { return assetId != TerraEngine::EMPTY_UUID && sourceHash.IsValid() && authoredGeneration != 0u; }
	bool TerrainSurfacePageHandle::IsResident() const noexcept { return key.IsComplete() && atlasTile != 0xffffffffu && byteCount != 0u && publicationGeneration != 0u; }

	TerrainSurfaceCachePlan PrepareTerrainSurfaceCachePlan(std::uint64_t observedGeneration, std::span<const TerrainSurfacePageHandle> prior, std::vector<TerrainSurfaceCacheRequest> requests, std::size_t budgetBytes, const std::atomic_bool* cancellation)
	{
		TerrainSurfaceCachePlan plan{};
		plan.observedGeneration = observedGeneration;
		if (observedGeneration == 0u || (cancellation != nullptr && cancellation->load(std::memory_order_acquire))) { plan.cancelled = true; return plan; }
		plan.candidate.assign(prior.begin(), prior.end());
		std::sort(requests.begin(), requests.end(), [](const auto& left, const auto& right) { return left.priority > right.priority; });
		for (const TerrainSurfaceCacheRequest& request : requests)
		{
			if (cancellation != nullptr && cancellation->load(std::memory_order_acquire)) { plan.cancelled = true; return plan; }
			if (!request.key.IsComplete() || request.byteCount == 0u) continue;
			auto existing = std::find_if(plan.candidate.begin(), plan.candidate.end(), [&request](const auto& page)
			{
				return page.key.assetId == request.key.assetId && page.key.area == request.key.area
					&& page.key.sourceHash == request.key.sourceHash && page.key.authoredGeneration == request.key.authoredGeneration;
			});
			if (existing != plan.candidate.end()) continue;
			auto previous = std::find_if(plan.candidate.begin(), plan.candidate.end(), [&request](const auto& page) { return page.key.assetId == request.key.assetId && page.key.area == request.key.area && !page.lastKnownGood; });
			if (previous != plan.candidate.end()) previous->lastKnownGood = true;
			const std::uint32_t tile = static_cast<std::uint32_t>(plan.candidate.size());
			TerrainSurfacePageHandle next{request.key, tile, request.byteCount, observedGeneration, false, request.protectedFromEviction};
			plan.candidate.push_back(next);
		}
		auto bytes = [&plan]() { std::size_t total = 0u; for (const auto& page : plan.candidate) total += page.byteCount; return total; };
		while (bytes() > budgetBytes)
		{
			auto victim = std::find_if(plan.candidate.begin(), plan.candidate.end(), [](const auto& page) { return !page.lastKnownGood && !page.protectedFromEviction; });
			if (victim == plan.candidate.end())
			{
				// A newly requested protected replacement is not visible yet. When it
				// cannot coexist with its protected last-known-good page, discard the
				// candidate instead of exceeding the budget or creating a coverage hole.
				victim = std::find_if(plan.candidate.rbegin(), plan.candidate.rend(), [](const auto& page) { return !page.lastKnownGood; }).base();
				if(victim == plan.candidate.begin()) break;
				--victim;
			}
			plan.evicted.push_back(victim->key);
			plan.candidate.erase(victim);
		}
		return plan;
	}

	bool PublishTerrainSurfaceCachePlan(std::vector<TerrainSurfacePageHandle>& inOutVisible, std::uint64_t& inOutGeneration, const TerrainSurfaceCachePlan& plan, std::uint64_t currentGeneration)
	{
		if (plan.cancelled || plan.observedGeneration == 0u || plan.observedGeneration != currentGeneration) return false;
		inOutVisible = plan.candidate;
		inOutGeneration = currentGeneration;
		return true;
	}
}
