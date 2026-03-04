#pragma once

#include <cstdint>
#include <string_view>
#include <cmath>
#include <algorithm>

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

enum class LightAppearance : std::uint32_t
{
	Color = 0u,
	FilterAndTemperature = 1u
};

// Scene light component mirrored to GPU light payloads each frame.
struct LightComponent : public Component<LightComponent>
{
	static constexpr std::string_view kComponentName = "LightComponent";

	LightType type = LightType::Point;
	LightUnit unit = LightUnit::Lumens;
	LightAppearance appearance = LightAppearance::Color;
	glm::vec3 color = glm::vec3(1.0f);
	float temperature = 6500.0f; // D65 standard illuminant (Daylight)
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
			.property("appearance", &LightComponent::GetAppearanceValue, &LightComponent::SetAppearanceValue)
			.property("color", &LightComponent::color)
			.property("temperature", &LightComponent::temperature)
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

	std::uint32_t GetAppearanceValue() const
	{
		return static_cast<std::uint32_t>(appearance);
	}

	void SetAppearanceValue(std::uint32_t encodedAppearance)
	{
		appearance = static_cast<LightAppearance>(encodedAppearance);
	}

	static glm::vec3 KelvinToRGB(float kelvin)
	{
		float temp = kelvin / 100.0f;
		float r, g, b;

		if (temp <= 66.0f)
		{
			r = 1.0f;
			g = std::clamp((99.4708025861f * std::log(temp) - 161.1195681661f) / 255.0f, 0.0f, 1.0f);
			if (temp <= 19.0f)
			{
				b = 0.0f;
			}
			else
			{
				b = std::clamp((138.5177312231f * std::log(temp - 10.0f) - 305.0447927307f) / 255.0f, 0.0f, 1.0f);
			}
		}
		else
		{
			r = std::clamp((329.698727446f * std::pow(temp - 60.0f, -0.1332047592f)) / 255.0f, 0.0f, 1.0f);
			g = std::clamp((288.1221695283f * std::pow(temp - 60.0f, -0.0755148492f)) / 255.0f, 0.0f, 1.0f);
			b = 1.0f;
		}

		return glm::vec3(r, g, b);
	}
};
