// Transform.h — Component defining the spatial position, rotation, and scale of an object.
//
// As a fundamental component for almost all scene objects, it provides TRS
// (Translation, Rotation, Scale) data using GLM math types. The component is
// integrated with the reflection layer to enable spatial manipulation via the
// editor and scripting systems.

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
		// position is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		: position(0.0f, 0.0f, 0.0f)
		, rotation(1.0f, 0.0f, 0.0f, 0.0f)
		, scale(1.0f, 1.0f, 1.0f)
	{
	}

	glm::vec3 position;
	glm::quat rotation;
	glm::vec3 scale;

	// reflect is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static void reflect(entt::meta_factory<Transform>& factory)
	{
		// Register every field so editor tooling can adjust TRS values through
		// the shared EnTT-backed reflection data.
		TerraEngine::Reflection::RegisterDataProperty<&Transform::position>(factory, "Position");
		TerraEngine::Reflection::RegisterDataProperty<&Transform::rotation>(factory, "Rotation");
		TerraEngine::Reflection::RegisterDataProperty<&Transform::scale>(factory, "Scale");
	}
};
