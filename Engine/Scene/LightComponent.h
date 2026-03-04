#pragma once

#include <cstdint>
#include <string_view>

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "Core/Object/Component.h"

// Author-facing light variants supported by the renderer.
enum class LightType : std::uint32_t
{
	Directional = 0u,
	Point = 1u,
	Spot = 2u,
	Area = 3u
};

enum class LightUnit : std::uint32_t
{
	Lumens = 0u,
	Lux = 1u
};

// Scene light component mirrored to GPU light payloads each frame.
struct LightComponent : public Component<LightComponent>
{
	static constexpr std::string_view kComponentName = "LightComponent";

	LightType type = LightType::Point;
	LightUnit unit = LightUnit::Lumens;
	glm::vec3 color = glm::vec3(1.0f);
	float intensity = 1.0f;
	float range = 10.0f;
	float innerConeAngle = glm::radians(20.0f);
	float outerConeAngle = glm::radians(30.0f);
	float width = 1.0f;
	float height = 1.0f;
	bool castsShadows = false;
	float shadowStrength = 1.0f;
	float shadowBias = 0.005f;
	float indirectMultiplier = 1.0f;

	// Reflection exposes a stable uint32 representation for type so serializer
	// code can persist the enum without custom JSON enum parsing.
	static void reflect(rttr::registration::class_<LightComponent>& registration)
	{
		registration
			.property("type", &LightComponent::GetTypeValue, &LightComponent::SetTypeValue)
			.property("unit", &LightComponent::GetUnitValue, &LightComponent::SetUnitValue)
			.property("color", &LightComponent::color)
			.property("intensity", &LightComponent::intensity)
			.property("range", &LightComponent::range)
			.property("innerConeAngle", &LightComponent::innerConeAngle)
			.property("outerConeAngle", &LightComponent::outerConeAngle)
			.property("width", &LightComponent::width)
			.property("height", &LightComponent::height)
			.property("castsShadows", &LightComponent::castsShadows)
			.property("shadowStrength", &LightComponent::shadowStrength)
			.property("shadowBias", &LightComponent::shadowBias)
			.property("indirectMultiplier", &LightComponent::indirectMultiplier);
	}

	std::uint32_t GetTypeValue() const
	{
		return static_cast<std::uint32_t>(type);
	}

	void SetTypeValue(std::uint32_t encodedType)
	{
		// Preserve the serialized raw value so unsupported legacy types are not
		// silently remapped. The editor/runtime now handle unsupported values explicitly.
		type = static_cast<LightType>(encodedType);
	}

	std::uint32_t GetUnitValue() const
	{
		return static_cast<std::uint32_t>(unit);
	}

	void SetUnitValue(std::uint32_t encodedUnit)
	{
		unit = static_cast<LightUnit>(encodedUnit);
	}
};
