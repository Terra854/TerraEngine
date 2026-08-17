// TerrainSpatialProxy.h -- non-authoritative spatial coverage for terrain areas.

#pragma once

#include "Scene/WorldRegion.h"
#include "Terrain/TerrainAssetStore.h"

#include <optional>
#include <vector>

namespace TerraEngine::Terrain
{
	// A terrain proxy accelerates broad phase queries only. The authored area and
	// integer sample lattice remain the authority for editing, picking, and IO.
	struct TerrainSpatialProxy
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerrainAreaCoordinate area{};
		Generation authoredGeneration = 0u;
		TerraEngine::Scene::WorldPosition canonicalOwner{};
		TerraEngine::Scene::SpatialAABB localBounds{};
		std::vector<TerraEngine::Scene::WorldRegionKey> coveredRegions;
		bool fallback = false;
	};

	// Derives a bounded float AABB only after the caller supplies a compatible
	// local reference frame. Returning nullopt makes distant/unqualified areas
	// fall back to their coarse regional proxy instead of losing coordinate truth.
	TERRAENGINE_API std::optional<TerrainSpatialProxy> BuildTerrainSpatialProxy(
		const TerrainAreaDescriptor& descriptor,
		TerraEngine::UUID assetId,
		double sampleSpacingMetres,
		double heightDatumMetres,
		const TerraEngine::Scene::WorldPosition& terrainOrigin,
		const TerraEngine::Scene::SpatialReferenceFrame& frame);
}
