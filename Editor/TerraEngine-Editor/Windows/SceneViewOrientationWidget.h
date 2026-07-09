// SceneViewOrientationWidget.h - Screen-space Scene View orientation helpers.

#pragma once

#include "SceneViewOverlayTypes.h"

#include <glm/glm.hpp>

namespace TerraEngine::Editor::SceneViewOverlay
{
	enum class OrientationAxis
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ,
		None
	};

	struct OrientationSnapEuler
	{
		float pitchRadians = 0.0f;
		float yawRadians = 0.0f;
		std::string label = "Persp";
	};

	[[nodiscard]] const char* ToString(OrientationAxis axis);
	[[nodiscard]] OrientationWidgetState BuildOrientationWidgetState(const glm::vec3& cameraForward, const glm::vec3& cameraUp, ProjectionMode projectionMode, const glm::vec4& screenBounds);
	[[nodiscard]] OrientationAxis HitTestOrientationWidget(const OrientationWidgetState& state, const glm::vec2& pointer);
	[[nodiscard]] OrientationSnapEuler SnapEulerForAxis(OrientationAxis axis);
}
