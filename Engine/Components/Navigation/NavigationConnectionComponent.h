// NavigationConnectionComponent.h - explicit, gameplay-controlled ground links.
#pragma once

#include "Core/Object/Component.h"
#include "Scene/WorldCoordinate.h"
#include "Utilities/UUID.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace TerraEngine::Navigation
{
	enum class NavigationConnectionDirection : std::uint8_t
	{
		Bidirectional,
		ForwardOnly,
		ReverseOnly
	};

	struct NavigationConnectionValidationResult
	{
		bool accepted = false;
		std::string message;
	};
}

struct NavigationConnectionComponent final : public Component<NavigationConnectionComponent>
{
	static constexpr std::string_view kComponentName = "NavigationConnectionComponent";

	TerraEngine::UUID connectionId = TerraEngine::GenerateUUID();
	std::uint32_t revision = 1U;
	bool enabled = true;
	TerraEngine::Scene::WorldPosition firstEndpoint{};
	TerraEngine::Scene::WorldPosition secondEndpoint{};
	TerraEngine::Navigation::NavigationConnectionDirection direction = TerraEngine::Navigation::NavigationConnectionDirection::Bidirectional;
	std::uint32_t capabilityMask = 0xFFFFFFFFU;
	bool gameplayCondition = true;
	std::uint32_t areaId = 0U;
	float traversalCost = 1.0F;

	[[nodiscard]] TerraEngine::Navigation::NavigationConnectionValidationResult Validate() const
	{
		using enum TerraEngine::Navigation::NavigationConnectionDirection;
		if(direction != Bidirectional && direction != ForwardOnly && direction != ReverseOnly)
			return {false, "Unknown ground connection direction."};
		if(!std::isfinite(traversalCost) || traversalCost < 0.0F)
			return {false, "Ground connection cost must be finite and nonnegative."};
		if(!TerraEngine::Scene::NormalizeWorldPosition(firstEndpoint).Succeeded()
			|| !TerraEngine::Scene::NormalizeWorldPosition(secondEndpoint).Succeeded())
			return {false, "Ground connection endpoints must be canonical world positions."};
		return {true, {}};
	}

	[[nodiscard]] std::uint32_t GetDirectionValue() const noexcept { return static_cast<std::uint32_t>(direction); }
	void SetDirectionValue(std::uint32_t value) noexcept { direction = static_cast<TerraEngine::Navigation::NavigationConnectionDirection>(value); }

	static void reflect(entt::meta_factory<NavigationConnectionComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::connectionId>(factory, "connectionId");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::revision>(factory, "revision");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::enabled>(factory, "enabled");
		TerraEngine::Reflection::RegisterAccessorProperty<&NavigationConnectionComponent::SetDirectionValue, &NavigationConnectionComponent::GetDirectionValue>(factory, "direction");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::capabilityMask>(factory, "capabilityMask");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::gameplayCondition>(factory, "gameplayCondition");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::areaId>(factory, "areaId");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationConnectionComponent::traversalCost>(factory, "traversalCost");
	}
};
