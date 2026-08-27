// TerrainRenderProxyRegistry.cpp -- Cross-module terrain render binding storage.

#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"

namespace TerraEngine::Renderer::Terrain
{
	TerrainRenderProxyRegistry& TerrainRenderProxyRegistry::Instance()
	{
		// This function is compiled and exported by the Engine DLL so native engine
		// systems and editor windows always receive the same process-wide registry.
		static TerrainRenderProxyRegistry registry;
		return registry;
	}

	bool TerrainRenderProxyRegistry::UpdateAreaRenderState(
		const TerraEngine::UUID& terrainAssetId,
		std::uint64_t expectedPublicationGeneration,
		TerraEngine::Terrain::TerrainAreaCoordinate coordinate,
		TerrainAreaRenderState state)
	{
		if(terrainAssetId == TerraEngine::EMPTY_UUID || expectedPublicationGeneration == 0u)
			return false;
		std::unique_lock lock(mutex);
		const auto snapshot = snapshots.find(terrainAssetId);
		if(snapshot == snapshots.end()
			|| snapshot->second.publicationGeneration != expectedPublicationGeneration) return false;
		auto area = std::find_if(snapshot->second.areas.begin(), snapshot->second.areas.end(),
			[coordinate](const TerrainAreaRenderBinding& candidate) {
				return candidate.coordinate == coordinate;
			});
		if(area == snapshot->second.areas.end()) return false;
		area->renderState = std::move(state);
		return true;
	}
}
