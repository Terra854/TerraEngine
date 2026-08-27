// TerrainMaterialRuntimeRegistry.cpp -- atomic terrain material binding publication.

#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Renderer::Terrain
{
	namespace
	{
		bool FinitePair(const std::array<float, 2u>& value) noexcept
		{
			return std::isfinite(value[0]) && std::isfinite(value[1]);
		}

		bool ValidLayerMapping(const TerrainGpuLayerMapping& mapping) noexcept
		{
			const float rotationLengthSquared =
				mapping.inverseRotationCosSin[0] * mapping.inverseRotationCosSin[0]
				+ mapping.inverseRotationCosSin[1] * mapping.inverseRotationCosSin[1];
			return FinitePair(mapping.tileSizeMetres)
				&& mapping.tileSizeMetres[0] > 0.0F && mapping.tileSizeMetres[1] > 0.0F
				&& FinitePair(mapping.inverseRotationCosSin)
				&& std::isfinite(rotationLengthSquared)
				&& std::abs(rotationLengthSquared - 1.0F) <= 1.0e-3F
				&& std::isfinite(mapping.triplanarSharpness)
				&& mapping.triplanarSharpness >= 1.0F && mapping.triplanarSharpness <= 32.0F
				&& (mapping.mode == TerrainGpuMappingMode::TerrainXZ
					|| mapping.mode == TerrainGpuMappingMode::Triplanar);
		}

		template<std::size_t Size>
		bool FiniteArray(const std::array<float, Size>& value) noexcept
		{
			return std::all_of(value.begin(), value.end(), [](float channel) {
				return std::isfinite(channel);
			});
		}

		bool ValidLayerSource(const TerrainLayerSourceBinding& layer) noexcept
		{
			return layer.stableLayerId != 0u && ValidLayerMapping(layer.mapping)
				&& FiniteArray(layer.baseColorFactor)
				&& FiniteArray(layer.emissiveFactor)
				&& std::isfinite(layer.normalScale) && layer.normalScale >= 0.0F
				&& std::isfinite(layer.occlusionStrength)
				&& layer.occlusionStrength >= 0.0F && layer.occlusionStrength <= 1.0F
				&& std::isfinite(layer.roughnessFactor) && layer.roughnessFactor >= 0.0F
				&& std::isfinite(layer.metallicFactor) && layer.metallicFactor >= 0.0F;
		}

		TerrainGpuLayerBinding MakeGpuLayer(const TerrainLayerSourceBinding& source) noexcept
		{
			TerrainGpuLayerBinding gpu{};
			gpu.baseColorTextureIndex = source.textureIndices[0];
			gpu.normalTextureIndex = source.textureIndices[1];
			gpu.ormTextureIndex = source.textureIndices[2];
			gpu.emissiveTextureIndex = source.textureIndices[3];
			gpu.baseColorFactor = source.baseColorFactor;
			gpu.emissiveFactor = {
				source.emissiveFactor[0], source.emissiveFactor[1],
				source.emissiveFactor[2], 0.0F};
			gpu.normalScale = source.normalScale;
			gpu.occlusionStrength = source.occlusionStrength;
			gpu.roughnessFactor = source.roughnessFactor;
			gpu.metallicFactor = source.metallicFactor;
			gpu.tileSizeMetres = source.mapping.tileSizeMetres;
			gpu.inverseRotationCosSin = source.mapping.inverseRotationCosSin;
			gpu.triplanarSharpness = source.mapping.triplanarSharpness;
			gpu.mappingMode = source.mapping.mode;
			gpu.stableLayerId = source.stableLayerId;
			return gpu;
		}
	}

	bool BuildTerrainGpuLayerBinding(
		const TerrainLayerSourceBinding& source,
		TerrainGpuLayerBinding& outBinding) noexcept
	{
		outBinding = {};
		if(!ValidLayerSource(source)) return false;
		outBinding = MakeGpuLayer(source);
		return true;
	}

	bool TerrainAreaMappingBinding::IsComplete() const noexcept
	{
		if(!std::isfinite(sampleSpacingMetres) || sampleSpacingMetres <= 0.0
			|| !std::isfinite(localSampleOrigin[0]) || !std::isfinite(localSampleOrigin[1])
			|| localSampleOrigin[0] < 0.0 || localSampleOrigin[1] < 0.0
			|| localSampleOrigin[0] > static_cast<double>(kTerrainControlExtentSamples)
			|| localSampleOrigin[1] > static_cast<double>(kTerrainControlExtentSamples)
			|| controlExtentSamples != kTerrainControlExtentSamples
			|| layerCount == 0u || layerCount > kTerrainGpuLayerLimit)
		{
			return false;
		}
		for(std::uint32_t layerIndex = 0u; layerIndex < layerCount; ++layerIndex)
		{
			const TerrainGpuLayerPhase& phase = layerPhases[layerIndex];
			if(phase.stableLayerId == 0u || !FinitePair(phase.phaseMetres)
				|| phase.phaseMetres[0] < 0.0F || phase.phaseMetres[1] < 0.0F)
			{
				return false;
			}
		}
		return true;
	}

	bool TerrainMaterialRuntimeBinding::HasCompleteAreaMappings() const noexcept
	{
		if(areaMappings.empty()) return false;
		std::unordered_set<TerraEngine::Terrain::TerrainAreaCoordinate,
			TerraEngine::Terrain::TerrainAreaCoordinateHasher> uniqueAreas;
		for(const TerrainAreaMappingBinding& areaBinding : areaMappings)
		{
			if(!areaBinding.IsComplete() || areaBinding.layerCount != layerCount
				|| !uniqueAreas.emplace(areaBinding.area).second)
			{
				return false;
			}
			for(std::uint32_t layerIndex = 0u; layerIndex < layerCount; ++layerIndex)
			{
				if(areaBinding.layerPhases[layerIndex].stableLayerId
					!= layers[layerIndex].stableLayerId
					|| areaBinding.layerPhases[layerIndex].phaseMetres[0]
					>= layers[layerIndex].mapping.tileSizeMetres[0]
					|| areaBinding.layerPhases[layerIndex].phaseMetres[1]
					>= layers[layerIndex].mapping.tileSizeMetres[1])
				{
					return false;
				}
			}
		}
		return true;
	}

	TerrainMaterialRuntimeRegistry& TerrainMaterialRuntimeRegistry::Instance()
	{
		static TerrainMaterialRuntimeRegistry registry;
		return registry;
	}

	bool TerrainMaterialRuntimeRegistry::Publish(TerrainMaterialRuntimeBinding binding)
	{
		if(!binding.IsComplete()) return false;
		std::unordered_set<std::uint16_t> stableLayerIds;
		for(std::uint32_t layerIndex = 0u; layerIndex < binding.layerCount; ++layerIndex)
		{
			TerrainLayerSourceBinding& layer = binding.layers[layerIndex];
			// Legacy diagnostics used negative base alpha as their fallback marker.
			// Preserve that authored-unavailable state while keeping ordinary factors
			// finite and avoiding any partial publication.
			if(layer.baseColorFactor[3] < 0.0F) layer.fallbackActive = true;
			if(!ValidLayerSource(layer)
				|| !stableLayerIds.emplace(layer.stableLayerId).second) return false;
			if(!BuildTerrainGpuLayerBinding(layer, binding.gpuLayers[layerIndex])) return false;
		}
		// Empty area mappings remain accepted for the temporary flat compatibility
		// binding used by existing tests and origin-only proxies. Once a producer
		// supplies area mappings, the complete table is validated atomically.
		if(!binding.areaMappings.empty() && !binding.HasCompleteAreaMappings()) return false;
		std::unique_lock lock(mutex);
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

	std::vector<TerraEngine::UUID> TerrainMaterialRuntimeRegistry::RepublishSamplerQuality(
		std::uint32_t requestedAnisotropy)
	{
		requestedAnisotropy = std::max(1u, requestedAnisotropy);
		std::unique_lock lock(mutex);
		if(requestedSamplerAnisotropy == requestedAnisotropy) return {};
		requestedSamplerAnisotropy = requestedAnisotropy;
		std::vector<TerraEngine::UUID> affected;
		affected.reserve(bindings.size());
		for(auto& [materialId, binding] : bindings)
		{
			// A new immutable generation forces descriptor aliases to be resolved
			// from the same source UUIDs without recreating source image storage.
			binding.publicationGeneration = ++generation;
			affected.push_back(materialId);
		}
		return affected;
	}

	std::uint32_t TerrainMaterialRuntimeRegistry::RequestedSamplerAnisotropy() const noexcept
	{
		std::shared_lock lock(mutex);
		return requestedSamplerAnisotropy;
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
