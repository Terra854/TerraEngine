// NavigationAgentComponent.h - durable ground-route authoring only.
#pragma once

#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace TerraEngine::Navigation
{
	enum class NavigationRoutePolicy : std::uint8_t
	{
		ExactDestination,
		NearestReachable
	};

	struct NavigationAgentValidationResult
	{
		bool accepted = false;
		std::string message;
	};
}

struct NavigationAgentComponent final : public Component<NavigationAgentComponent>
{
	static constexpr std::string_view kComponentName = "NavigationAgentComponent";

	TerraEngine::UUID profileId = TerraEngine::EMPTY_UUID;
	TerraEngine::Navigation::NavigationRoutePolicy routePolicy = TerraEngine::Navigation::NavigationRoutePolicy::ExactDestination;
	bool enabled = true;

	[[nodiscard]] TerraEngine::Navigation::NavigationAgentValidationResult Validate() const
	{
		using enum TerraEngine::Navigation::NavigationRoutePolicy;
		if(routePolicy != ExactDestination && routePolicy != NearestReachable)
			return {false, "Unknown ground route policy."};
		return {true, {}};
	}

	[[nodiscard]] std::uint32_t GetRoutePolicyValue() const noexcept { return static_cast<std::uint32_t>(routePolicy); }
	void SetRoutePolicyValue(std::uint32_t value) noexcept { routePolicy = static_cast<TerraEngine::Navigation::NavigationRoutePolicy>(value); }

	static void reflect(entt::meta_factory<NavigationAgentComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::profileId>(factory, "profileId");
		TerraEngine::Reflection::RegisterAccessorProperty<&NavigationAgentComponent::SetRoutePolicyValue, &NavigationAgentComponent::GetRoutePolicyValue>(factory, "routePolicy");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::enabled>(factory, "enabled");
	}
};
