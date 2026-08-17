// TerrainSpatialProxy.cpp -- exact terrain to bounded spatial proxy conversion.

#include "Terrain/TerrainSpatialProxy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Terrain
{
namespace
{
	std::optional<TerraEngine::Scene::WorldPosition> OffsetOrigin(
		const TerraEngine::Scene::WorldPosition& origin, long double xMetres, long double yMetres, long double zMetres)
	{
		if (!std::isfinite(xMetres) || !std::isfinite(yMetres) || !std::isfinite(zMetres)
			|| xMetres < static_cast<long double>(std::numeric_limits<double>::lowest())
			|| xMetres > static_cast<long double>(std::numeric_limits<double>::max())
			|| zMetres < static_cast<long double>(std::numeric_limits<double>::lowest())
			|| zMetres > static_cast<long double>(std::numeric_limits<double>::max())) return std::nullopt;
		TerraEngine::Scene::WorldDelta delta{};
		delta.offsetMetres = { static_cast<double>(xMetres), static_cast<double>(yMetres), static_cast<double>(zMetres) };
		const auto absolute = TerraEngine::Scene::ApplyWorldDelta(origin, delta);
		return absolute.Succeeded() ? std::optional<TerraEngine::Scene::WorldPosition>{absolute.position} : std::nullopt;
	}
}

	std::optional<TerrainSpatialProxy> BuildTerrainSpatialProxy(
		const TerrainAreaDescriptor& descriptor,
		TerraEngine::UUID assetId,
		double sampleSpacingMetres,
		double heightDatumMetres,
		const TerraEngine::Scene::WorldPosition& terrainOrigin,
		const TerraEngine::Scene::SpatialReferenceFrame& frame)
	{
		if (assetId == TerraEngine::EMPTY_UUID || descriptor.editGeneration == 0u
			|| !std::isfinite(sampleSpacingMetres) || sampleSpacingMetres <= 0.0 || !std::isfinite(heightDatumMetres)) return std::nullopt;
		const long double extent = static_cast<long double>(kTerrainAreaExtentSamples) * sampleSpacingMetres;
		const long double x = static_cast<long double>(descriptor.coordinate.x) * extent;
		const long double z = static_cast<long double>(descriptor.coordinate.z) * extent;
		const auto owner = OffsetOrigin(terrainOrigin, x, heightDatumMetres + static_cast<long double>(descriptor.minHeightMm) / 1000.0L, z);
		const auto opposite = OffsetOrigin(terrainOrigin, x + extent, heightDatumMetres + static_cast<long double>(descriptor.maxHeightMm) / 1000.0L, z + extent);
		if (!owner.has_value() || !opposite.has_value()) return std::nullopt;
		const auto localOwner = TerraEngine::Scene::LocalizeToSpatialFrame(*owner, frame);
		const auto localOpposite = TerraEngine::Scene::LocalizeToSpatialFrame(*opposite, frame);
		if (!localOwner.Succeeded() || !localOpposite.Succeeded()) return std::nullopt;
		TerraEngine::Scene::WorldRegionKey firstRegion{};
		TerraEngine::Scene::WorldRegionKey lastRegion{};
		if (TerraEngine::Scene::WorldPositionToRegionKey(*owner, firstRegion) != TerraEngine::Scene::WorldCoordinateError::None
			|| TerraEngine::Scene::WorldPositionToRegionKey(*opposite, lastRegion) != TerraEngine::Scene::WorldCoordinateError::None) return std::nullopt;
		const std::int64_t countX = lastRegion.x - firstRegion.x + 1;
		const std::int64_t countZ = lastRegion.z - firstRegion.z + 1;
		if (countX <= 0 || countZ <= 0 || static_cast<std::uint64_t>(countX) * static_cast<std::uint64_t>(countZ) > TerraEngine::Scene::kMaximumWorldRegionCoverCount) return std::nullopt;

		TerrainSpatialProxy proxy{};
		proxy.assetId = assetId;
		proxy.area = descriptor.coordinate;
		proxy.authoredGeneration = descriptor.editGeneration;
		proxy.canonicalOwner = *owner;
		proxy.localBounds = TerraEngine::Scene::MakeAABB(
			glm::vec3(localOwner.offsetMetres), glm::vec3(localOpposite.offsetMetres), TerraEngine::Scene::SpatialBoundsSource::Authored);
		for (std::int64_t regionZ = firstRegion.z; regionZ <= lastRegion.z; ++regionZ)
			for (std::int64_t regionX = firstRegion.x; regionX <= lastRegion.x; ++regionX)
				proxy.coveredRegions.push_back({regionX, firstRegion.y, regionZ});
		return proxy.localBounds.IsUsable() ? std::optional<TerrainSpatialProxy>{std::move(proxy)} : std::nullopt;
	}
}
