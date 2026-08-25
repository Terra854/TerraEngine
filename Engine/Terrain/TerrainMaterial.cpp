// TerrainMaterial.cpp -- Generated metallic-roughness terrain material builder.

#include "Terrain/TerrainMaterial.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"
#include "Terrain/TerrainLayerStack.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace TerraEngine::Terrain
{
	TerrainMaterialSettings SanitizeTerrainMaterialSettings(
		TerrainMaterialSettings settings) noexcept
	{
		// A finite clamp prevents NaNs from propagating into descriptor buffers and
		// gives malformed legacy scene values the same predictable fallback as UI.
		for(float* channel : {&settings.baseColor.r, &settings.baseColor.g, &settings.baseColor.b})
			*channel = std::isfinite(*channel) ? std::clamp(*channel, 0.0F, 1.0F) : 0.0F;
		settings.metallic = std::isfinite(settings.metallic)
			? std::clamp(settings.metallic, 0.0F, 1.0F) : 0.0F;
		settings.roughness = std::isfinite(settings.roughness)
			? std::clamp(settings.roughness, 0.0F, 1.0F) : 0.92F;
		return settings;
	}

	std::unique_ptr<Material> CreateTerrainMaterial(
		ResourceManager& resources, TerrainMaterialSettings settings,
		const Material* surfaceSource,
		const TerrainLayerMapping* mapping)
	{
		settings = SanitizeTerrainMaterialSettings(settings);
		auto material = std::make_unique<Material>();
		std::memcpy(material->header.magic, "TMAT", 4U);
		material->header.version = kMaterialCurrentVersion;
		material->header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
		// Generated terrain vertices carry slope normals, so the shared PBR path
		// receives scene lights and shadows exactly like an ordinary opaque mesh.
		material->header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
		material->header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
		material->header.flags = DoubleSided;
		material->header.shaderCollectionId = resources.GetDefaultShaderCollectionID();
		material->header.baseColorFactor[0] = settings.baseColor.r;
		material->header.baseColorFactor[1] = settings.baseColor.g;
		material->header.baseColorFactor[2] = settings.baseColor.b;
		material->header.baseColorFactor[3] = 1.0F;
		material->header.metallicFactor = settings.metallic;
		material->header.roughnessFactor = settings.roughness;
		material->header.normalScale = 1.0F;
		material->header.occlusionStrength = 1.0F;
		material->shaderCollectionId = material->header.shaderCollectionId;
		if(surfaceSource != nullptr && !surfaceSource->textures.empty())
		{
			// Terrain's first healthy layer is a normal TMAT source. Copy its texture
			// bindings into the generated proxy instead of retaining a pointer into the
			// resource cache, then apply the terrain layer's world-space tiling values.
			const std::size_t textureBytes = surfaceSource->textures.size_bytes();
			const std::size_t floatBytes = surfaceSource->floats.size_bytes();
			material->storage.resize(textureBytes + floatBytes);
			std::memcpy(material->storage.data(), surfaceSource->textures.data(), textureBytes);
			if(floatBytes != 0u)
				std::memcpy(material->storage.data() + textureBytes, surfaceSource->floats.data(), floatBytes);
			auto* copiedTextures = reinterpret_cast<TextureBinding*>(material->storage.data());
			if(mapping != nullptr)
			{
				for(std::size_t index = 0u; index < surfaceSource->textures.size(); ++index)
				{
					copiedTextures[index].uvScale[0] *= static_cast<float>(mapping->scaleX);
					copiedTextures[index].uvScale[1] *= static_cast<float>(mapping->scaleZ);
					copiedTextures[index].uvOffset[0] += static_cast<float>(mapping->offsetX);
					copiedTextures[index].uvOffset[1] += static_cast<float>(mapping->offsetZ);
				}
			}
			material->textures = std::span<const TextureBinding>(copiedTextures, surfaceSource->textures.size());
			material->floats = std::span<const FloatParameter>(
				reinterpret_cast<const FloatParameter*>(material->storage.data() + textureBytes),
				surfaceSource->floats.size());
			material->header.textureCount = static_cast<std::uint32_t>(material->textures.size());
			material->header.floatParameterCount = static_cast<std::uint32_t>(material->floats.size());
		}
		material->shadowBehavior = DeriveMaterialShadowBehavior(
			material->header.alphaMode, material->header.emissiveFactor);
		return material;
	}
}
