// TerrainComponent.h — Scene-owned reference to one durable terrain asset.
//
// Terrain samples, layers, and holes live in the terrain asset store rather than
// in ECS component storage. Keeping this component deliberately small makes a
// scene instance cheap to serialize while terrain workers can publish immutable
// asset generations without mutating ObjectManager from a worker thread.

#pragma once

#include "Core/Object/Component.h"
#include "Scene/WorldCoordinate.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>

struct TerrainComponent final : public Component<TerrainComponent>
{
	static constexpr std::string_view kComponentName = "TerrainComponent";
	static constexpr std::uint32_t kSchemaVersion = 1U;

	// The durable Asset Database UUID is the only authored terrain reference in
	// a scene. Terrain area data is intentionally not duplicated in components.
	std::uint32_t schemaVersion = kSchemaVersion;
	TerraEngine::UUID terrainAssetId = TerraEngine::EMPTY_UUID;
	bool visible = true;
	bool castsShadows = true;

	// Terrain v1 accepts translation through Transform::worldPose, but rejects
	// rotation and scale because either would break the exact shared sample grid.
	// The helper is pure so scene import, editor validation, and tests all use
	// the same rule without giving the component ownership of Transform state.
	[[nodiscard]] static bool SupportsTranslationOnlyPlacement(
		const TerraEngine::Scene::WorldPose& pose) noexcept
	{
		constexpr float kUnitTolerance = 0.0001F;
		// Keep this validation self-contained: including the full Transform
		// component here would make every terrain consumer instantiate unrelated
		// reflection code merely to check a small value contract.
		const bool finite = std::isfinite(pose.rotation.w)
			&& std::isfinite(pose.rotation.x)
			&& std::isfinite(pose.rotation.y)
			&& std::isfinite(pose.rotation.z)
			&& std::isfinite(pose.scale.x)
			&& std::isfinite(pose.scale.y)
			&& std::isfinite(pose.scale.z);
		const bool identityRotation = std::abs(pose.rotation.w - 1.0F) <= kUnitTolerance
			&& std::abs(pose.rotation.x) <= kUnitTolerance
			&& std::abs(pose.rotation.y) <= kUnitTolerance
			&& std::abs(pose.rotation.z) <= kUnitTolerance;
		const bool unitScale = std::abs(pose.scale.x - 1.0F) <= kUnitTolerance
			&& std::abs(pose.scale.y - 1.0F) <= kUnitTolerance
			&& std::abs(pose.scale.z - 1.0F) <= kUnitTolerance;
		return finite && identityRotation && unitScale;
	}

	static void reflect(entt::meta_factory<TerrainComponent>& factory)
	{
		// These are the entire durable scene contract. Runtime residency, edit
		// generations, previews, and derived pages are rebuilt from the asset.
		TerraEngine::Reflection::RegisterDataProperty<&TerrainComponent::schemaVersion>(factory, "schemaVersion");
		TerraEngine::Reflection::RegisterDataProperty<&TerrainComponent::terrainAssetId>(factory, "terrainAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&TerrainComponent::visible>(factory, "visible");
		TerraEngine::Reflection::RegisterDataProperty<&TerrainComponent::castsShadows>(factory, "castsShadows");
	}
};
