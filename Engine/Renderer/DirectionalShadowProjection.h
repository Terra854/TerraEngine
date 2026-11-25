#pragma once

#include "Scene/WorldCoordinate.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace TerraEngine::Renderer
{
	// Fade the subject's influence as it leaves the camera ray. Picking a hard
	// cutoff here can move the shadow footprint tens of metres in one frame.
	inline float ResolveDirectionalShadowFocusDepth(const glm::vec3& offset, const glm::vec3& forward)
	{
		const float depth = glm::dot(offset, forward);
		if (!std::isfinite(depth) || depth <= 0.0f) return 0.0f;
		const float lateral = glm::length(offset - forward * depth);
		return depth * (1.0f - glm::smoothstep(0.5f, 0.8f, lateral / depth));
	}

	// The square light-space footprint stays fixed, preserving the existing
	// texel density. When the editor dollies out, move the footprint toward
	// the subject instead of letting it retreat with the camera.
	inline glm::vec3 ResolveSceneDirectionalShadowAnchor(const glm::vec3& cameraPosition,
		const glm::mat4& viewMatrix, float focusDistanceMetres)
	{
		constexpr float shiftStartMetres = 28.0f;
		constexpr float shiftEndMetres = 52.0f;
		if (!std::isfinite(focusDistanceMetres) || focusDistanceMetres <= shiftStartMetres)
			return cameraPosition;

		// The third row of the right-handed view rotation is camera backward.
		const glm::vec3 forward(-viewMatrix[0][2], -viewMatrix[1][2], -viewMatrix[2][2]);
		const float weight = glm::smoothstep(shiftStartMetres, shiftEndMetres, focusDistanceMetres);
		return cameraPosition + forward * (focusDistanceMetres * weight);
	}

	struct DirectionalShadowProjection
	{
		glm::vec3 center;
		glm::mat4 viewProjection;
	};

	inline constexpr std::uint32_t kDirectionalShadowBaseResolution = 1024u;
	inline constexpr std::uint32_t kDirectionalShadowMaximumSceneResolution = 4096u;
	inline constexpr float kDirectionalShadowBaseHalfExtent = 40.0f;
	inline constexpr float kDirectionalShadowBaseFarPlane = 180.0f;
	inline constexpr float kDirectionalShadowNearPlane = 0.1f;

	// Only these tiers preserve the intended footprint and texel density. The
	// backend checks again before replacing its physical image allocation.
	inline constexpr bool IsValidSceneDirectionalShadowResolution(std::uint32_t resolution)
	{
		return resolution >= kDirectionalShadowBaseResolution
			&& resolution <= kDirectionalShadowMaximumSceneResolution
			&& (resolution & (resolution - 1u)) == 0u;
	}

	inline float SceneDirectionalShadowFarPlane(std::uint32_t resolution)
	{
		const float extent = kDirectionalShadowBaseHalfExtent * resolution / kDirectionalShadowBaseResolution;
		return std::max(kDirectionalShadowBaseFarPlane, 2.0f * extent + 40.0f);
	}

	// A bounding sphere fits the same map as the sun rotates. Each larger tier
	// preserves the base world-space texel size and leaves room for filtering.
	inline std::uint32_t ResolveSceneDirectionalShadowResolution(const glm::vec3& halfSize)
	{
		const float radius = glm::length(halfSize) + 2.0f;
		for (auto resolution = kDirectionalShadowBaseResolution;
			resolution <= kDirectionalShadowMaximumSceneResolution; resolution *= 2u)
		{
			if (radius <= kDirectionalShadowBaseHalfExtent * resolution / kDirectionalShadowBaseResolution)
				return resolution;
		}
		return 0u;
	}

	// Snap the canonical anchor within its world cell, then apply the same
	// correction in whichever local frame consumes the projection. This avoids
	// snapping the near-zero translation of an already camera-centered matrix.
	inline DirectionalShadowProjection BuildDirectionalShadowProjection(const glm::vec3& lightDirection,
		const glm::vec3& anchor, const glm::dvec3& anchorInWorldCell,
		std::uint32_t resolution = kDirectionalShadowBaseResolution, bool sceneCoverage = false)
	{
		const float extent = kDirectionalShadowBaseHalfExtent * resolution / kDirectionalShadowBaseResolution;
		const float lightOffset = sceneCoverage ? extent + 20.0f : 60.0f;
		const float farPlane = sceneCoverage ? SceneDirectionalShadowFarPlane(resolution) : kDirectionalShadowBaseFarPlane;
		constexpr double texelWorldSize = (kDirectionalShadowBaseHalfExtent * 2.0) / kDirectionalShadowBaseResolution;
		const float length = glm::length(lightDirection);
		// Light transforms point along their emitted rays. Start upstream and
		// record depth along those rays, just as the lighting shader expects.
		const glm::vec3 direction = std::isfinite(length) && length > 0.00001f
			? lightDirection / length : glm::vec3(0.0f, -1.0f, 0.0f);
		const glm::vec3 worldUp = std::abs(direction.y) > 0.95f
			? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

		// Build rotation at the origin so subtracting large local eye/target
		// coordinates cannot perturb the light axes when the view origin moves.
		const glm::mat4 rotation = glm::lookAtRH(glm::vec3(0.0f), direction, worldUp);
		const glm::dvec3 right(rotation[0][0], rotation[1][0], rotation[2][0]);
		const glm::dvec3 up(rotation[0][1], rotation[1][1], rotation[2][1]);
		const auto snapCorrection = [&](const glm::dvec3& axis) {
			const double position = glm::dot(axis, anchorInWorldCell);
			return std::round(position / texelWorldSize) * texelWorldSize - position;
		};
		const glm::vec3 center = anchor + glm::vec3(right * snapCorrection(right) + up * snapCorrection(up));
		const glm::mat4 view = glm::translate(rotation, direction * lightOffset - center);
		glm::mat4 projection = glm::orthoRH_ZO(-extent, extent, -extent, extent, kDirectionalShadowNearPlane, farPlane);
		projection[1][1] *= -1.0f;
		return {center, projection * view};
	}

	inline DirectionalShadowProjection BuildDirectionalShadowProjection(const glm::vec3& lightDirection,
		const glm::vec3& anchor, const Scene::WorldPosition& referenceOrigin = {})
	{
		const auto canonical = Scene::ApplyWorldDelta(referenceOrigin, {{}, glm::dvec3(anchor)});
		return BuildDirectionalShadowProjection(lightDirection, anchor,
			canonical.Succeeded() ? canonical.position.offsetMetres : glm::dvec3(anchor));
	}
}
