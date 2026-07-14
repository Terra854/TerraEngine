// SceneViewOverlayMarkers.cpp - Scene View marker projection and hit-testing.

#include "SceneViewOverlayMarkers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

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
		// A uniform screen-space grid compares only nearby markers. The previous
		// all-pairs loop made a dense ten-thousand-body Scene View quadratic even
		// though markers farther than one radius can never share an overlap group.
		// A cell diagonal is no wider than the overlap radius, so every marker in
		// one cell belongs to the same connected overlap group. This avoids the
		// pathological all-pairs case when thousands of bodies project together.
		const float cellSize = std::max(radiusPixels * 0.70710678f, 1.0f);
		const int neighborRange = static_cast<int>(std::ceil(radiusPixels / cellSize));
		const float radiusSquared = radiusPixels * radiusPixels;
		std::vector<std::size_t> parents(markers.size());
		std::unordered_map<std::uint64_t, std::vector<std::size_t>> cells;
		cells.reserve(markers.size());
		for (std::size_t index = 0u; index < markers.size(); ++index)
		{
			parents[index] = index;
			markers[index].overlapGroup = 0u;
		}

		auto findRoot = [&parents](std::size_t index) {
			while (parents[index] != index)
			{
				parents[index] = parents[parents[index]];
				index = parents[index];
			}
			return index;
		};
		auto mergeRoots = [&findRoot, &parents](std::size_t left, std::size_t right) {
			const std::size_t leftRoot = findRoot(left);
			const std::size_t rightRoot = findRoot(right);
			if (leftRoot != rightRoot)
			{
				// Attach the later marker to the earlier root so group numbering stays
				// deterministic in the same order as the visible marker list.
				parents[rightRoot] = leftRoot;
			}
		};
		auto cellKey = [](int x, int y) -> std::uint64_t {
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U)
				| static_cast<std::uint32_t>(y);
		};
		for (std::size_t index = 0u; index < markers.size(); ++index)
		{
			if (!markers[index].visible)
			{
				continue;
			}

			const int cellX = static_cast<int>(std::floor(markers[index].screenPosition.x / cellSize));
			const int cellY = static_cast<int>(std::floor(markers[index].screenPosition.y / cellSize));
			for (int offsetY = -neighborRange; offsetY <= neighborRange; ++offsetY)
			{
				for (int offsetX = -neighborRange; offsetX <= neighborRange; ++offsetX)
				{
					const auto cell = cells.find(cellKey(cellX + offsetX, cellY + offsetY));
					if (cell == cells.end())
					{
						continue;
					}
					if (offsetX == 0 && offsetY == 0 && !cell->second.empty())
					{
						mergeRoots(cell->second.front(), index);
						continue;
					}
					for (const std::size_t other : cell->second)
					{
						const glm::vec2 delta = markers[index].screenPosition - markers[other].screenPosition;
						if (glm::dot(delta, delta) <= radiusSquared)
						{
							mergeRoots(other, index);
						}
					}
				}
			}
			cells[cellKey(cellX, cellY)].push_back(index);
		}

		std::unordered_map<std::size_t, std::uint32_t> groupIdsByRoot;
		groupIdsByRoot.reserve(markers.size());
		std::uint32_t nextGroup = 1u;
		for (std::size_t index = 0u; index < markers.size(); ++index)
		{
			if (!markers[index].visible)
			{
				continue;
			}
			const std::size_t root = findRoot(index);
			auto [group, inserted] = groupIdsByRoot.emplace(root, nextGroup);
			if (inserted)
			{
				++nextGroup;
			}
			markers[index].overlapGroup = group->second;
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
