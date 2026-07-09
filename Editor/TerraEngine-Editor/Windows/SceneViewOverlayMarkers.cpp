// SceneViewOverlayMarkers.cpp - Scene View marker projection and hit-testing.

#include "SceneViewOverlayMarkers.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Editor::SceneViewOverlay
{
	std::uint32_t MarkerCategoryPriority(MarkerCategory category, bool selected)
	{
		if (selected)
		{
			return 1000u;
		}
		switch (category)
		{
		case MarkerCategory::SelectedObject: return 900u;
		case MarkerCategory::Camera: return 700u;
		case MarkerCategory::DirectionalLight: return 650u;
		case MarkerCategory::PointLight: return 600u;
		case MarkerCategory::SpotLight: return 600u;
		case MarkerCategory::EmptyObject: return 300u;
		case MarkerCategory::Unsupported: return 0u;
		default: return 0u;
		}
	}

	bool ProjectMarkerToScreen(SceneObjectMarker& marker, const glm::mat4& viewProjection, const glm::vec2& viewportPos, const glm::vec2& viewportSize)
	{
		const glm::vec4 clip = viewProjection * glm::vec4(marker.worldPosition, 1.0f);
		if (clip.w <= 0.0001f || viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
		{
			marker.visible = false;
			return false;
		}

		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		if (ndc.x < -1.25f || ndc.x > 1.25f || ndc.y < -1.25f || ndc.y > 1.25f)
		{
			marker.visible = false;
			return false;
		}

		marker.screenPosition = glm::vec2(
			viewportPos.x + (ndc.x + 1.0f) * 0.5f * viewportSize.x,
			viewportPos.y + (1.0f - ndc.y) * 0.5f * viewportSize.y);
		marker.depth = ndc.z;
		marker.visible = true;
		marker.visualPriority = MarkerCategoryPriority(marker.category, marker.selected);
		return true;
	}

	void ResolveMarkerOverlapGroups(std::vector<SceneObjectMarker>& markers, float radiusPixels)
	{
		std::uint32_t nextGroup = 1u;
		const float radiusSquared = radiusPixels * radiusPixels;
		for (std::size_t index = 0u; index < markers.size(); ++index)
		{
			if (!markers[index].visible)
			{
				continue;
			}
			if (markers[index].overlapGroup == 0u)
			{
				markers[index].overlapGroup = nextGroup++;
			}
			for (std::size_t other = index + 1u; other < markers.size(); ++other)
			{
				if (!markers[other].visible)
				{
					continue;
				}
				const glm::vec2 delta = markers[index].screenPosition - markers[other].screenPosition;
				if (glm::dot(delta, delta) <= radiusSquared)
				{
					markers[other].overlapGroup = markers[index].overlapGroup;
				}
			}
		}
	}

	std::optional<std::size_t> HitTestMarkers(const std::vector<SceneObjectMarker>& markers, const glm::vec2& pointer, float radiusPixels)
	{
		std::vector<std::size_t> candidates;
		const float radiusSquared = radiusPixels * radiusPixels;
		for (std::size_t index = 0u; index < markers.size(); ++index)
		{
			const SceneObjectMarker& marker = markers[index];
			if (!marker.visible)
			{
				continue;
			}
			const glm::vec2 delta = marker.screenPosition - pointer;
			if (glm::dot(delta, delta) <= radiusSquared)
			{
				candidates.push_back(index);
			}
		}

		if (candidates.empty())
		{
			return std::nullopt;
		}

		std::sort(candidates.begin(), candidates.end(), [&](std::size_t left, std::size_t right) {
			const SceneObjectMarker& a = markers[left];
			const SceneObjectMarker& b = markers[right];
			if (a.selected != b.selected)
			{
				return a.selected;
			}
			if (a.visualPriority != b.visualPriority)
			{
				return a.visualPriority > b.visualPriority;
			}
			if (a.depth != b.depth)
			{
				return a.depth < b.depth;
			}
			if (a.markerId != b.markerId)
			{
				return a.markerId < b.markerId;
			}
			return a.displayName < b.displayName;
		});
		return candidates.front();
	}

	SceneViewDiagnosticsSnapshot CountMarkersIntoSnapshot(SceneViewDiagnosticsSnapshot snapshot, const std::vector<SceneObjectMarker>& markers)
	{
		snapshot.cameraMarkerCount = 0u;
		snapshot.directionalLightMarkerCount = 0u;
		snapshot.pointLightMarkerCount = 0u;
		snapshot.spotLightMarkerCount = 0u;
		snapshot.emptyObjectMarkerCount = 0u;
		snapshot.selectedObjectMarkerCount = 0u;
		for (const SceneObjectMarker& marker : markers)
		{
			if (!marker.visible)
			{
				continue;
			}
			switch (marker.category)
			{
			case MarkerCategory::Camera: ++snapshot.cameraMarkerCount; break;
			case MarkerCategory::DirectionalLight: ++snapshot.directionalLightMarkerCount; break;
			case MarkerCategory::PointLight: ++snapshot.pointLightMarkerCount; break;
			case MarkerCategory::SpotLight: ++snapshot.spotLightMarkerCount; break;
			case MarkerCategory::EmptyObject: ++snapshot.emptyObjectMarkerCount; break;
			case MarkerCategory::SelectedObject: ++snapshot.selectedObjectMarkerCount; break;
			default: break;
			}
		}
		return snapshot;
	}
}
