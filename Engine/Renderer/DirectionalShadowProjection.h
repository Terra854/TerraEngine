#pragma once

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace TerraEngine::Renderer
{
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

	// Build matching orthographic coverage for the CPU caster query and Vulkan.
	// The input position is expressed in whichever reference frame consumes it.
	inline DirectionalShadowProjection BuildDirectionalShadowProjection(const glm::vec3& lightDirection,
		const glm::vec3& anchor)
	{
		constexpr float extent = 40.0f;
		constexpr float lightOffset = 60.0f;
		constexpr float texelWorldSize = (extent * 2.0f) / 1024.0f;
		const float length = glm::length(lightDirection);
		const glm::vec3 direction = std::isfinite(length) && length > 0.00001f
			? -lightDirection / length : glm::vec3(0.0f, -1.0f, 0.0f);
		const glm::vec3 worldUp = std::abs(direction.y) > 0.95f
			? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

		// Keep the same light-space center convention as the existing shadow map.
		const glm::mat4 provisionalView = glm::lookAtRH(anchor - direction * lightOffset, anchor, worldUp);
		glm::vec4 lightSpaceCenter = provisionalView * glm::vec4(anchor, 1.0f);
		lightSpaceCenter.x = std::floor(lightSpaceCenter.x / texelWorldSize) * texelWorldSize;
		lightSpaceCenter.y = std::floor(lightSpaceCenter.y / texelWorldSize) * texelWorldSize;
		const glm::vec3 center(glm::inverse(provisionalView) * lightSpaceCenter);
		const glm::mat4 view = glm::lookAtRH(center - direction * lightOffset, center, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-extent, extent, -extent, extent, 0.1f, 180.0f);
		projection[1][1] *= -1.0f;
		return {center, projection * view};
	}
}
