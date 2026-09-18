#pragma once

#include "Engine/Renderer/RenderTypes.h"
#include <entt/entt.hpp>
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
				&& sceneGeneration == current.sceneGeneration && selection == current.selection;
		}
	};

	// ImGui displays this Vulkan image with UV (0,0) at its top-left. The
	// projection already flips Y; image-copy texels must not flip it a second time.
	[[nodiscard]] inline glm::uvec2 SceneViewPickPixel(glm::vec2 local, glm::vec2 size)
	{
		return glm::uvec2(glm::clamp(local, glm::vec2(0.0F), glm::max(size - 1.0F, glm::vec2(0.0F))));
	}
}
