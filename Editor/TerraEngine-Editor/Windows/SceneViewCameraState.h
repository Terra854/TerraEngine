// SceneViewCameraState.h — Durable Scene View camera workspace state
//
// Keeps camera persistence independent from ImGui and renderer objects so the
// project workspace can validate state before applying it to the live viewport.

#pragma once

#include "SceneViewOverlayTypes.h"
#include "Engine/Scene/WorldCoordinate.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace TerraEngine::Editor::SceneView
{
	// This record contains every value that determines the visible Scene View
	// composition. It belongs to per-project UserSettings rather than scene data.
	struct CameraWorkspaceState
	{
		TerraEngine::Scene::WorldPosition position{{}, {0.0, 5.0, 10.0}};
		glm::vec3 rotationRadians{-0.35F, 0.0F, 0.0F};
		TerraEngine::Scene::WorldPosition focus{};
		float focusDistance = 18.0F;
		float fieldOfViewDegrees = 60.0F;
		float orthographicHeight = 10.0F;
		bool focusInitialized = false;
		TerraEngine::Editor::SceneViewOverlay::ProjectionMode projectionMode =
			TerraEngine::Editor::SceneViewOverlay::ProjectionMode::Perspective;
	};

	// Serialization uses decimal strings for world cells so distant positions do
	// not lose 64-bit precision when the JSON document passes through other tools.
	[[nodiscard]] nlohmann::json SerializeCameraWorkspaceState(const CameraWorkspaceState& state);
	// Deserialization is atomic: invalid input returns no value and never exposes a
	// partially decoded camera to the live Scene View.
	[[nodiscard]] std::optional<CameraWorkspaceState> DeserializeCameraWorkspaceState(
		const nlohmann::json& value,
		std::string* outError = nullptr);
}
