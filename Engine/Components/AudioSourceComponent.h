// AudioSourceComponent.h - durable scene authoring for one logical sound source.

#pragma once

#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

struct AudioSourceComponent final : public Component<AudioSourceComponent>
{
	static constexpr std::string_view kComponentName = "AudioSourceComponent";

	TerraEngine::UUID clipAssetId = TerraEngine::EMPTY_UUID;
	bool playOnStart = false;
	bool loop = false;
	float volume = 1.0F;
	float pitch = 1.0F;
	float spatialBlend = 1.0F;
	float maxDistance = 50.0F;
	TerraEngine::UUID mixGroupId = TerraEngine::EMPTY_UUID;
	std::uint32_t priority = 128U;

	// Reflection uses setters for constrained values so editor/automation and
	// scene deserialization cannot publish an invalid attenuation range.
	[[nodiscard]] float GetVolume() const noexcept { return volume; }
	void SetVolume(float value) noexcept { volume = std::clamp(value, 0.0F, 4.0F); }
	[[nodiscard]] float GetPitch() const noexcept { return pitch; }
	void SetPitch(float value) noexcept { pitch = std::clamp(value, 0.125F, 4.0F); }
	[[nodiscard]] float GetSpatialBlend() const noexcept { return spatialBlend; }
	void SetSpatialBlend(float value) noexcept { spatialBlend = std::clamp(value, 0.0F, 1.0F); }
	[[nodiscard]] float GetMaxDistance() const noexcept { return maxDistance; }
	void SetMaxDistance(float value) noexcept { maxDistance = std::max(value, 0.001F); }
	[[nodiscard]] std::uint32_t GetPriority() const noexcept { return priority; }
	void SetPriority(std::uint32_t value) noexcept { priority = std::min(value, 255U); }

	static void reflect(entt::meta_factory<AudioSourceComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&AudioSourceComponent::clipAssetId>(factory, "clipAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AudioSourceComponent::playOnStart>(factory, "playOnStart");
		TerraEngine::Reflection::RegisterDataProperty<&AudioSourceComponent::loop>(factory, "loop");
		TerraEngine::Reflection::RegisterAccessorProperty<&AudioSourceComponent::SetVolume, &AudioSourceComponent::GetVolume>(factory, "volume");
		TerraEngine::Reflection::RegisterAccessorProperty<&AudioSourceComponent::SetPitch, &AudioSourceComponent::GetPitch>(factory, "pitch");
		TerraEngine::Reflection::RegisterAccessorProperty<&AudioSourceComponent::SetSpatialBlend, &AudioSourceComponent::GetSpatialBlend>(factory, "spatialBlend");
		TerraEngine::Reflection::RegisterAccessorProperty<&AudioSourceComponent::SetMaxDistance, &AudioSourceComponent::GetMaxDistance>(factory, "maxDistance");
		TerraEngine::Reflection::RegisterDataProperty<&AudioSourceComponent::mixGroupId>(factory, "mixGroupId");
		TerraEngine::Reflection::RegisterAccessorProperty<&AudioSourceComponent::SetPriority, &AudioSourceComponent::GetPriority>(factory, "priority");
	}
};
