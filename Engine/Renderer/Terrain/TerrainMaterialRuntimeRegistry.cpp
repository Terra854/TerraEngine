// TerrainMaterialRuntimeRegistry.cpp -- atomic terrain material binding publication.

#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace TerraEngine::Renderer::Terrain
{
	TerrainMaterialRuntimeRegistry& TerrainMaterialRuntimeRegistry::Instance()
	{
		static TerrainMaterialRuntimeRegistry registry;
		return registry;
	}

	bool TerrainMaterialRuntimeRegistry::Publish(TerrainMaterialRuntimeBinding binding)
	{
		if(!binding.IsComplete()) return false;
		std::unique_lock lock(mutex);
		binding.layerCount = std::min<std::uint32_t>(
			binding.layerCount, static_cast<std::uint32_t>(binding.layers.size()));
		binding.publicationGeneration = ++generation;
		bindings[binding.materialId] = std::move(binding);
		return true;
	}

	std::optional<TerrainMaterialRuntimeBinding> TerrainMaterialRuntimeRegistry::Resolve(
		TerraEngine::UUID materialId) const
	{
		std::shared_lock lock(mutex);
		const auto found = bindings.find(materialId);
		return found == bindings.end()
			? std::nullopt : std::optional<TerrainMaterialRuntimeBinding>{found->second};
	}

	void TerrainMaterialRuntimeRegistry::Remove(TerraEngine::UUID materialId)
	{
		if(materialId == TerraEngine::EMPTY_UUID) return;
		std::unique_lock lock(mutex);
		if(bindings.erase(materialId) != 0u) ++generation;
	}

	void TerrainMaterialRuntimeRegistry::Clear()
	{
		std::unique_lock lock(mutex);
		if(bindings.empty()) return;
		bindings.clear();
		++generation;
	}
}
