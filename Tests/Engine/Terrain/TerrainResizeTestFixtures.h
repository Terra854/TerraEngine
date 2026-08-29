// TerrainResizeTestFixtures.h -- Reusable immutable resize test values.

#pragma once

#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"

namespace TerraEngine::Tests::TerrainResizeFixtures
{
	inline TerraEngine::Terrain::TerrainAreaDescriptor MakeDescriptor(
		TerraEngine::Terrain::TerrainAreaCoordinate coordinate,
		TerraEngine::Terrain::Generation generation = 1u)
	{
		TerraEngine::Terrain::TerrainAreaDescriptor descriptor{};
		descriptor.coordinate = coordinate;
		descriptor.editGeneration = generation;
		descriptor.layerPresence = {1u};
		const std::string identity = "resize-fixture:" + std::to_string(coordinate.x)
			+ ":" + std::to_string(coordinate.z) + ":" + std::to_string(generation);
		descriptor.authoredContentHash = TerraEngine::Terrain::HashTerrainBytes(
			std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
		return descriptor;
	}

	inline std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> MakeRoot(
		TerraEngine::Terrain::TerrainSampleBounds bounds = {0, 0, 256, 256},
		bool materializeOrigin = true)
	{
		auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
		root->assetUuid = TerraEngine::GenerateUUID();
		root->publicationGeneration = 1u;
		root->persistedRootGeneration = 1u;
		root->rootContentHash = {0x55u, 0xaau};
		root->sampleSpacingMetres = 1.0;
		root->authoredBounds = bounds;
		root->defaultLayerId = 1u;
		root->layerStackGeneration = 1u;
		if(materializeOrigin)
		{
			TerraEngine::Terrain::TerrainDirectoryShard shard{};
			const auto areas = TerraEngine::Terrain::TerrainAreasForSamples(bounds);
			shard.areaBounds = {areas->minX, areas->minZ, areas->maxXExclusive, areas->maxZExclusive};
			auto descriptor = MakeDescriptor({areas->minX, areas->minZ});
			shard.descriptors.emplace(descriptor.coordinate, descriptor);
			root->directoryShards.push_back(std::move(shard));
		}
		return root;
	}
}
