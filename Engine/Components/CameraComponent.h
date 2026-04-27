// CameraComponent.h — Component defining perspective and orthographic camera parameters.
//
// This component stores field of view, aspect ratio, and clipping plane data used
// by the renderer to construct view and projection matrices. It integrates with
// the engine's reflection system to allow real-time adjustment of camera
// properties through the editor or scripts.

#pragma once

#include <string_view>
#include <glm/vec3.hpp>

#include "Core/Object/Component.h"

// Camera metadata that pairs with a Transform to describe how the renderer
// should build its view and projection matrices. The defaults mirror the
// previously hardcoded preview camera so existing visuals stay consistent.
struct CameraComponent : public Component<CameraComponent>
{
	static constexpr std::string_view kComponentName = "CameraComponent";

	float verticalFovDegrees = 45.0f;
	float aspectRatio = 16.0f / 9.0f;
	float nearPlane = 0.1f;
	float farPlane = 100.0f;
	glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

	// Register the camera properties with the EnTT-backed reflection layer so tools can edit the projection
	// parameters at runtime while the renderer reads them in a thread-safe way.
	static void reflect(entt::meta_factory<CameraComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&CameraComponent::verticalFovDegrees>(factory, "verticalFovDegrees");
		TerraEngine::Reflection::RegisterDataProperty<&CameraComponent::aspectRatio>(factory, "aspectRatio");
		TerraEngine::Reflection::RegisterDataProperty<&CameraComponent::nearPlane>(factory, "nearPlane");
		TerraEngine::Reflection::RegisterDataProperty<&CameraComponent::farPlane>(factory, "farPlane");
		TerraEngine::Reflection::RegisterDataProperty<&CameraComponent::up>(factory, "up");
	}
};
