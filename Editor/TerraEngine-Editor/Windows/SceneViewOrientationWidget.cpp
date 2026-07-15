// SceneViewOrientationWidget.cpp - Screen-space Scene View orientation helpers.

#include "SceneViewOrientationWidget.h"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

namespace TerraEngine::Editor::SceneViewOverlay
{
	const char* ToString(OrientationAxis axis)
	{
		switch (axis)
		{
		case OrientationAxis::PositiveX: return "x";
		case OrientationAxis::NegativeX: return "-x";
		case OrientationAxis::PositiveY: return "y";
		case OrientationAxis::NegativeY: return "-y";
		case OrientationAxis::PositiveZ: return "z";
		case OrientationAxis::NegativeZ: return "-z";
		case OrientationAxis::None: return "none";
		default: return "unknown";
		}
	}

	OrientationWidgetState BuildOrientationWidgetState(const glm::vec3& cameraForward, const glm::vec3& cameraUp, ProjectionMode projectionMode, const glm::vec4& screenBounds)
	{
		OrientationWidgetState state;
		state.cameraForward = glm::length(cameraForward) > 0.0001f ? glm::normalize(cameraForward) : glm::vec3(0.0f, 0.0f, -1.0f);
		state.cameraUp = glm::length(cameraUp) > 0.0001f ? glm::normalize(cameraUp) : glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 rightCandidate = glm::cross(state.cameraForward, state.cameraUp);
		if (glm::length(rightCandidate) <= 0.0001f)
		{
			state.cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
		}
		else
		{
			state.cameraRight = glm::normalize(rightCandidate);
		}
		state.projectionMode = projectionMode;
		state.availableAxisTargets = { "x", "-x", "y", "-y", "z", "-z" };
		state.screenBounds = screenBounds;
		state.label = projectionMode == ProjectionMode::Perspective ? "Persp" : "Ortho";
		return state;
	}

	void OrientationWidgetCache::Reset()
	{
		key.reset();
		state = OrientationWidgetState{};
		reuseCount = 0u;
		rebuildCount = 0u;
	}

	const OrientationWidgetState& BuildOrientationWidgetStateCached(
		OrientationWidgetCache& cache,
		const SceneViewOverlayCacheKey& key,
		const glm::vec3& cameraForward,
		const glm::vec3& cameraUp,
		ProjectionMode projectionMode,
		const glm::vec4& screenBounds)
	{
		if (cache.key.has_value() && *cache.key == key)
		{
			++cache.reuseCount;
			return cache.state;
		}

		// The caller owns generation changes. Rebuild exactly once when any
		// viewport, scene, selection, simulation, or preference input changes.
		cache.state = BuildOrientationWidgetState(cameraForward, cameraUp, projectionMode, screenBounds);
		cache.key = key;
		++cache.rebuildCount;
		return cache.state;
	}

	OrientationAxis HitTestOrientationWidget(const OrientationWidgetState& state, const glm::vec2& pointer)
	{
		const float minX = state.screenBounds.x;
		const float minY = state.screenBounds.y;
		const float maxX = state.screenBounds.z;
		const float maxY = state.screenBounds.w;
		if (pointer.x < minX || pointer.y < minY || pointer.x > maxX || pointer.y > maxY)
		{
			return OrientationAxis::None;
		}

		const glm::vec2 center((minX + maxX) * 0.5f, (minY + maxY) * 0.5f);
		const float radius = std::max(10.0f, (maxX - minX) * 0.48f);
		struct AxisTarget
		{
			OrientationAxis axis = OrientationAxis::None;
			glm::vec2 position = glm::vec2(0.0f);
		};
		const AxisTarget targets[] = {
			{ OrientationAxis::PositiveX, glm::vec2(maxX - 16.0f, center.y) },
			{ OrientationAxis::NegativeX, glm::vec2(minX + 16.0f, center.y) },
			{ OrientationAxis::PositiveY, glm::vec2(center.x, minY + 16.0f) },
			{ OrientationAxis::NegativeY, glm::vec2(center.x, maxY - 16.0f) },
			{ OrientationAxis::PositiveZ, glm::vec2(center.x - 22.0f, center.y + 22.0f) },
			{ OrientationAxis::NegativeZ, glm::vec2(center.x + 22.0f, center.y - 22.0f) }
		};

		// Treat the whole orientation box as an axis picker. This is more usable
		// than requiring a precise hit on the tiny endpoint labels.
		float bestDistanceSquared = radius * radius;
		OrientationAxis bestAxis = OrientationAxis::None;
		for (const AxisTarget& target : targets)
		{
			const glm::vec2 delta = pointer - target.position;
			const float distanceSquared = glm::dot(delta, delta);
			if (distanceSquared <= bestDistanceSquared)
			{
				bestDistanceSquared = distanceSquared;
				bestAxis = target.axis;
			}
		}
		return bestAxis;
	}

	OrientationSnapEuler SnapEulerForAxis(OrientationAxis axis)
	{
		switch (axis)
		{
		case OrientationAxis::PositiveX: return { 0.0f, -glm::half_pi<float>(), "Right" };
		case OrientationAxis::NegativeX: return { 0.0f, glm::half_pi<float>(), "Left" };
		case OrientationAxis::PositiveY: return { -glm::half_pi<float>() + 0.001f, 0.0f, "Top" };
		case OrientationAxis::NegativeY: return { glm::half_pi<float>() - 0.001f, 0.0f, "Bottom" };
		case OrientationAxis::PositiveZ: return { 0.0f, glm::pi<float>(), "Back" };
		case OrientationAxis::NegativeZ: return { 0.0f, 0.0f, "Front" };
		case OrientationAxis::None: return { 0.0f, 0.0f, "Persp" };
		default: return { 0.0f, 0.0f, "Persp" };
		}
	}
}
