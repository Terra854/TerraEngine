// SkyComponent.h - Component-owned sky authoring settings.
//
// Sky data lives on scene objects so it follows the same save/load, inspector,
// duplication, and ownership rules as every other authored scene capability.

#pragma once

#include <string>
#include <string_view>

#include <glm/vec3.hpp>

#include "Core/Object/Component.h"

// SkyComponent stores the editable sky source for one scene object. The renderer
// reads active SkyComponent data directly, while scene files persist it through
// reflected component properties instead of scene-level JSON.
struct SkyComponent : public Component<SkyComponent>
{
	static constexpr std::string_view kComponentName = "SkyComponent";

	std::string sourceAsset;
	float exposure = 1.0f;
	float rotationOffsetDegrees = 0.0f;
	glm::vec3 horizonTint = glm::vec3(0.92f, 0.96f, 1.0f);
	glm::vec3 zenithTint = glm::vec3(0.20f, 0.38f, 0.80f);
	glm::vec3 groundTint = glm::vec3(0.10f, 0.12f, 0.18f);

	static void reflect(entt::meta_factory<SkyComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&SkyComponent::sourceAsset>(factory, "sourceAsset");
		TerraEngine::Reflection::RegisterDataProperty<&SkyComponent::exposure>(factory, "exposure");
		TerraEngine::Reflection::RegisterDataProperty<&SkyComponent::rotationOffsetDegrees>(factory, "rotationOffsetDegrees");
		TerraEngine::Reflection::RegisterDataProperty<&SkyComponent::horizonTint>(factory, "horizonTint");
		TerraEngine::Reflection::RegisterDataProperty<&SkyComponent::zenithTint>(factory, "zenithTint");
		TerraEngine::Reflection::RegisterDataProperty<&SkyComponent::groundTint>(factory, "groundTint");
	}
};
