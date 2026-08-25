// TerrainMaterial.h -- Shared authored settings for generated terrain materials.

#pragma once

#include "Core/Export.h"

#include <glm/vec3.hpp>

#include <memory>

struct Material;
class ResourceManager;

namespace TerraEngine::Terrain
{
	struct TerrainLayerMapping;
	// Terrain keeps a compact set of physically based inputs in the scene. The
	// generated editor and runtime materials both consume this value object, so an
	// exported terrain cannot silently fall back to different lighting values.
	struct TerrainMaterialSettings
	{
		glm::vec3 baseColor{0.18F, 0.55F, 0.16F};
		float metallic = 0.0F;
		float roughness = 0.92F;

		[[nodiscard]] bool operator==(const TerrainMaterialSettings&) const = default;
	};

	// Invalid scene input is clamped before it reaches renderer-owned resources.
	// This makes old or manually edited scene files safe to load.
	[[nodiscard]] TERRAENGINE_API TerrainMaterialSettings SanitizeTerrainMaterialSettings(
		TerrainMaterialSettings settings) noexcept;

	// Builds the ordinary TMAT-backed PBR representation used by both editor and
	// packaged terrain proxies. Resource registration remains with the caller.
	[[nodiscard]] TERRAENGINE_API std::unique_ptr<Material> CreateTerrainMaterial(
		ResourceManager& resources, TerrainMaterialSettings settings,
		const Material* surfaceSource = nullptr,
		const TerrainLayerMapping* mapping = nullptr);
}
