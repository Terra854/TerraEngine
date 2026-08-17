// TerrainDistantProxy.cpp -- deterministic conservative terrain regional proxy.

#include "Renderer/Terrain/TerrainDistantProxy.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace TerraEngine::Renderer::Terrain
{
	bool TerrainDistantProxy::IsComplete() const noexcept
	{
		return assetId != TerraEngine::EMPTY_UUID && authoredHash.IsValid()
			&& authoredGeneration != 0u && coveredAreaWidth != 0u && identity.IsValid();
	}

	TerrainDistantProxy BuildTerrainDistantProxy(
		TerraEngine::UUID assetId,
		TerraEngine::Terrain::TerrainAreaCoordinate regionalRoot,
		std::uint32_t coveredAreaWidth,
		std::span<const TerrainPageCookResult> pages)
	{
		TerrainDistantProxy proxy{};
		const std::uint64_t requiredPageCount = static_cast<std::uint64_t>(coveredAreaWidth) * coveredAreaWidth;
		if (assetId == TerraEngine::EMPTY_UUID || coveredAreaWidth == 0u || pages.empty()
			|| requiredPageCount > pages.size()) return proxy;
		if (regionalRoot.x > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(coveredAreaWidth)
			|| regionalRoot.z > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(coveredAreaWidth)) return proxy;
		const TerrainPageCookResult& first = pages.front();
		if (!first.complete || !first.authoredHash.IsValid() || first.authoredGeneration == 0u) return proxy;
		proxy.assetId = assetId;
		proxy.regionalRoot = regionalRoot;
		proxy.authoredHash = first.authoredHash;
		proxy.authoredGeneration = first.authoredGeneration;
		proxy.coveredAreaWidth = coveredAreaWidth;
		proxy.minimumHeightMetres = std::numeric_limits<float>::max();
		proxy.maximumHeightMetres = std::numeric_limits<float>::lowest();
		std::vector<std::uint8_t> identityBytes;
		std::set<TerraEngine::Terrain::TerrainAreaCoordinate> coveredAreas;
		for (const TerrainPageCookResult& page : pages)
		{
			if (!page.complete || page.authoredHash != proxy.authoredHash || page.authoredGeneration != proxy.authoredGeneration) return {};
			if (page.assetId != assetId || page.area.x < regionalRoot.x || page.area.x >= regionalRoot.x + static_cast<std::int64_t>(coveredAreaWidth)
				|| page.area.z < regionalRoot.z || page.area.z >= regionalRoot.z + static_cast<std::int64_t>(coveredAreaWidth)) return {};
			coveredAreas.insert(page.area);
			proxy.minimumHeightMetres = std::min(proxy.minimumHeightMetres, page.minimumHeightMetres);
			proxy.maximumHeightMetres = std::max(proxy.maximumHeightMetres, page.maximumHeightMetres);
			proxy.conservativeErrorMetres = std::max(proxy.conservativeErrorMetres, page.maximumGeometricErrorMetres);
			const auto append = [&identityBytes](std::uint64_t value)
			{
				for (std::uint32_t shift = 0u; shift < 64u; shift += 8u) identityBytes.push_back(static_cast<std::uint8_t>(value >> shift));
			};
			append(page.sourceIdentity.low64);
			append(page.sourceIdentity.high64);
		}
		if (coveredAreas.size() != requiredPageCount) return {};
		// The regional reduction itself adds a conservative half-range error.
		proxy.conservativeErrorMetres = std::max(proxy.conservativeErrorMetres, (proxy.maximumHeightMetres - proxy.minimumHeightMetres) * 0.5f);
		proxy.identity = TerraEngine::Terrain::HashTerrainBytes(identityBytes);
		return proxy;
	}
}
