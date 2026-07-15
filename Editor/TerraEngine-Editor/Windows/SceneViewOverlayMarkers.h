// SceneViewOverlayMarkers.h - Scene View marker projection and hit-testing.

#pragma once

#include "SceneViewOverlayTypes.h"

#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace TerraEngine::Editor::SceneViewOverlay
{
	// Component flags are captured at the protected editor boundary. The marker
	// classifier can then run without reading a live registry from worker code.
	struct MarkerComponentClassification
	{
		bool selected = false;
		bool hasCamera = false;
		bool hasDirectionalLight = false;
		bool hasPointLight = false;
		bool hasSpotLight = false;
		bool hasRenderable = false;
		bool hasRigidBody = false;
		bool hasColliderSet = false;
		bool hasConstraintSet = false;
		bool hasCharacterController = false;
		bool hasOtherMarkerRelevantComponent = false;
	};

	[[nodiscard]] bool IsPhysicsBearing(const MarkerComponentClassification& components);
	[[nodiscard]] bool IsGenuinelyEmpty(const MarkerComponentClassification& components);
	[[nodiscard]] MarkerCategory ClassifyMarkerCategory(const MarkerComponentClassification& components);
	[[nodiscard]] bool IsMarkerReserved(MarkerCategory category, bool selected);
	[[nodiscard]] std::uint32_t MarkerCategoryPriority(MarkerCategory category, bool selected);
	[[nodiscard]] bool ProjectMarkerToScreen(SceneObjectMarker& marker, const glm::mat4& viewProjection, const glm::vec2& viewportPos, const glm::vec2& viewportSize);
	void ResolveMarkerOverlapGroups(std::vector<SceneObjectMarker>& markers, float radiusPixels = 18.0f);
	[[nodiscard]] std::optional<std::size_t> HitTestMarkers(const std::vector<SceneObjectMarker>& markers, const glm::vec2& pointer, float radiusPixels = 12.0f);
	[[nodiscard]] SceneViewDiagnosticsSnapshot CountMarkersIntoSnapshot(SceneViewDiagnosticsSnapshot snapshot, const std::vector<SceneObjectMarker>& markers);
}
