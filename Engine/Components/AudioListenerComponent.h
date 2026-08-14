// AudioListenerComponent.h - durable scene authoring for an eligible listener.

#pragma once

#include "Audio/AudioTypes.h"
#include "Core/Object/Component.h"

#include <string_view>

struct AudioListenerComponent final : public Component<AudioListenerComponent>
{
	static constexpr std::string_view kComponentName = "AudioListenerComponent";

	// A disabled listener remains serialized but is excluded from deterministic
	// selection; AudioSystem breaks ties by object UUID at its protected capture.
	bool eligible = true;
	TerraEngine::Audio::AudioOutputMode outputMode = TerraEngine::Audio::AudioOutputMode::Stereo;

	[[nodiscard]] std::uint32_t GetOutputModeValue() const noexcept
	{
		return static_cast<std::uint32_t>(outputMode);
	}

	void SetOutputModeValue(std::uint32_t value) noexcept
	{
		// Unknown serialized or automation input falls back to the established
		// stereo behavior before it can reach provider code.
		outputMode = value <= static_cast<std::uint32_t>(TerraEngine::Audio::AudioOutputMode::Surround7Point1)
			? static_cast<TerraEngine::Audio::AudioOutputMode>(value)
			: TerraEngine::Audio::AudioOutputMode::Stereo;
	}

	static void reflect(entt::meta_factory<AudioListenerComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&AudioListenerComponent::eligible>(factory, "eligible");
		TerraEngine::Reflection::RegisterAccessorProperty<&AudioListenerComponent::SetOutputModeValue, &AudioListenerComponent::GetOutputModeValue>(factory, "outputMode");
	}
};
