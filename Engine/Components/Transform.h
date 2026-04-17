#pragma once

#include "Component.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <string_view>

// Basic spatial component that exposes TRS data via glm math types.
class Transform : public Component<Transform> {
public:
	static constexpr std::string_view kComponentName = "Transform";

	Transform()
		: position(0.0f, 0.0f, 0.0f)
		, rotation(1.0f, 0.0f, 0.0f, 0.0f)
		, scale(1.0f, 1.0f, 1.0f)
	{
	}

	glm::vec3 position;
	glm::quat rotation;
	glm::vec3 scale;

	static void reflect(entt::meta_factory<Transform>& factory)
	{
		// Register every field so editor tooling can adjust TRS values through
		// the shared EnTT-backed reflection data.
		TerraEngine::Reflection::RegisterDataProperty<&Transform::position>(factory, "Position");
		TerraEngine::Reflection::RegisterDataProperty<&Transform::rotation>(factory, "Rotation");
		TerraEngine::Reflection::RegisterDataProperty<&Transform::scale>(factory, "Scale");
	}
};

