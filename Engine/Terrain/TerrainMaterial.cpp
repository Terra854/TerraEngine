// TerrainMaterial.cpp -- Generated metallic-roughness terrain material builder.

#include "Terrain/TerrainMaterial.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"

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
		ResourceManager& resources, TerrainMaterialSettings settings)
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
		material->shadowBehavior = DeriveMaterialShadowBehavior(
			material->header.alphaMode, material->header.emissiveFactor);
		return material;
	}
}
