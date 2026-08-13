// AudioListenerComponent.h - durable scene authoring for an eligible listener.

#pragma once

#include "Core/Object/Component.h"

#include <string_view>

struct AudioListenerComponent final : public Component<AudioListenerComponent>
{
	static constexpr std::string_view kComponentName = "AudioListenerComponent";

	// A disabled listener remains serialized but is excluded from deterministic
	// selection; AudioSystem breaks ties by object UUID at its protected capture.
	bool eligible = true;

	static void reflect(entt::meta_factory<AudioListenerComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&AudioListenerComponent::eligible>(factory, "eligible");
	}
};
