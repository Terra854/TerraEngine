// PrefabLinkComponent.h - prefab instance metadata attached to linked objects.
//
// This component is intentionally small and reflected so scenes can persist
// prefab links without editor-only side tables. Editor UI reads it to show
// prefab names, override state, unlink controls, and per-property reset actions.

#pragma once

#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

#include <string>
#include <string_view>

struct PrefabLinkComponent : public Component<PrefabLinkComponent>
{
	static constexpr std::string_view kComponentName = "PrefabLinkComponent";

	TerraEngine::UUID prefabSourceId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID sourceObjectId = TerraEngine::EMPTY_UUID;
	std::string prefabName;
	std::string prefabPath;
	std::string overriddenProperties;

	static void reflect(entt::meta_factory<PrefabLinkComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&PrefabLinkComponent::prefabSourceId>(factory, "prefabSourceId");
		TerraEngine::Reflection::RegisterDataProperty<&PrefabLinkComponent::sourceObjectId>(factory, "sourceObjectId");
		TerraEngine::Reflection::RegisterDataProperty<&PrefabLinkComponent::prefabName>(factory, "prefabName");
		TerraEngine::Reflection::RegisterDataProperty<&PrefabLinkComponent::prefabPath>(factory, "prefabPath");
		TerraEngine::Reflection::RegisterDataProperty<&PrefabLinkComponent::overriddenProperties>(factory, "overriddenProperties");
	}
};
