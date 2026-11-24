#pragma once

#include "Engine/Renderer/RenderTypes.h"
#include <entt/entt.hpp>
#include <cmath>
#include <optional>

namespace TerraEngine::Editor
{
	// A click owns one asynchronous GPU request. Keep its screen/camera/scene
	// context so a later completion cannot overwrite a newer editor interaction.
	struct SceneViewPickRequest
	{
		glm::uvec2 pixel{};
		glm::vec4 viewport{};
		glm::mat4 viewProjection{1.0F};
		TerraEngine::Scene::WorldPosition camera{};
		std::uint64_t sceneGeneration = 0;
		// These identify the actual identity attachment behind the displayed image.
		// A render-scale or target replacement invalidates a pending click even when
		// the editor image rectangle and camera projection have not changed.
		glm::uvec2 identityExtent{};
		std::uint64_t identityTargetGeneration = 0;
		std::optional<entt::entity> selection;
		TerraEngine::Renderer::RenderReferenceFrame referenceFrame;
		double expiresAt = 0.0;

		[[nodiscard]] bool Accepts(const VirtualGeometryVisibilityPick& pick,
			const TerraEngine::Renderer::RenderReferenceFrame& current) const
		{
			// Origin identity is frame-scoped, even with a stationary camera. Prove
			// the hit against its requested frame, then check data freshness against
			// today's publication; IsCurrent separately guards actual camera motion.
			return pick.frameId == referenceFrame.frameId && pick.IsQualifiedFor(referenceFrame)
				&& current.IsValid() && current.viewId == referenceFrame.viewId
				&& current.outputId == referenceFrame.outputId
				&& current.canonicalOrigin == referenceFrame.canonicalOrigin
				&& current.spatialGeneration == referenceFrame.spatialGeneration
				&& current.residencyGeneration == referenceFrame.residencyGeneration;
		}

		[[nodiscard]] bool IsCurrent(const SceneViewPickRequest& current, double now) const
		{
			return now < expiresAt && viewport == current.viewport
				&& viewProjection == current.viewProjection && camera == current.camera
				&& sceneGeneration == current.sceneGeneration && selection == current.selection
				&& identityExtent == current.identityExtent
				&& identityTargetGeneration == current.identityTargetGeneration;
		}
	};

	// ImGui displays this Vulkan image with UV (0,0) at its top-left. The
	// projection already flips Y; image-copy texels must not flip it a second time.
	[[nodiscard]] inline glm::uvec2 SceneViewPickPixel(glm::vec2 local, glm::vec2 size)
	{
		return glm::uvec2(glm::clamp(local, glm::vec2(0.0F), glm::max(size - 1.0F, glm::vec2(0.0F))));
	}

	// Map the editor's image rectangle into the published identity attachment.
	// The rectangle may have fractional UI dimensions and a different resolution
	// from the renderer. Invalid input has no meaningful GPU pixel and is rejected.
	[[nodiscard]] inline std::optional<glm::uvec2> SceneViewPickPixel(
		glm::vec2 local, glm::vec2 displayedSize, glm::uvec2 identityExtent)
	{
		if (!std::isfinite(local.x) || !std::isfinite(local.y)
			|| !std::isfinite(displayedSize.x) || !std::isfinite(displayedSize.y)
			|| displayedSize.x <= 0.0F || displayedSize.y <= 0.0F
			|| identityExtent.x == 0U || identityExtent.y == 0U)
			return std::nullopt;

		// Use double intermediates so even the largest uint32 extent remains
		// representable before truncation. At the right/bottom edge, clamp to the
		// last valid texel. Y already shares ImGui's top-left orientation.
		glm::uvec2 pixel;
		for (glm::length_t axis = 0; axis < 2; ++axis)
		{
			const double normalized = glm::clamp(
				static_cast<double>(local[axis]) / displayedSize[axis], 0.0, 1.0);
			const double texel = glm::min(normalized * identityExtent[axis],
				static_cast<double>(identityExtent[axis] - 1U));
			pixel[axis] = static_cast<std::uint32_t>(texel);
		}
		return pixel;
	}
}
