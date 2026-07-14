#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayMarkers.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewMarkerSelectionTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Markers.HitTestIsDeterministic", [](TestContext& context) {
			std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> markers(2u);
			markers[0].markerId = "z-light";
			markers[0].category = TerraEngine::Editor::SceneViewOverlay::MarkerCategory::PointLight;
			markers[0].screenPosition = glm::vec2(50.0f);
			markers[0].visible = true;
			markers[0].visualPriority = TerraEngine::Editor::SceneViewOverlay::MarkerCategoryPriority(markers[0].category, false);
			markers[1].markerId = "a-camera";
			markers[1].category = TerraEngine::Editor::SceneViewOverlay::MarkerCategory::Camera;
			markers[1].screenPosition = glm::vec2(50.0f);
			markers[1].visible = true;
			markers[1].visualPriority = TerraEngine::Editor::SceneViewOverlay::MarkerCategoryPriority(markers[1].category, false);

			const auto hit = TerraEngine::Editor::SceneViewOverlay::HitTestMarkers(markers, glm::vec2(50.0f), 12.0f);
			context.Expect(hit.has_value(), "Overlapping marker hit should return a candidate.");
			context.ExpectEqual(std::string("a-camera"), markers[*hit].markerId, "Higher-priority camera marker should win deterministic overlap selection.");
		});

		registry.Add("SceneViewOverlay.Markers.SpatialOverlapGroupsRemainConnectedAndReset", [](TestContext& context) {
			std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> markers(4u);
			markers[0].screenPosition = glm::vec2(0.0f, 0.0f);
			markers[1].screenPosition = glm::vec2(12.0f, 0.0f);
			markers[2].screenPosition = glm::vec2(24.0f, 0.0f);
			markers[3].screenPosition = glm::vec2(96.0f, 0.0f);
			for (TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker& marker : markers)
			{
				marker.visible = true;
			}

			TerraEngine::Editor::SceneViewOverlay::ResolveMarkerOverlapGroups(markers, 18.0f);
			context.Expect(markers[0].overlapGroup != 0U, "Visible markers must receive an overlap group.");
			context.Expect(markers[0].overlapGroup == markers[1].overlapGroup && markers[1].overlapGroup == markers[2].overlapGroup, "A chain of overlapping markers must resolve as one connected group.");
			context.Expect(markers[3].overlapGroup != markers[0].overlapGroup, "Distant markers must retain an independent overlap group.");

			markers[2].visible = false;
			TerraEngine::Editor::SceneViewOverlay::ResolveMarkerOverlapGroups(markers, 18.0f);
			context.Expect(markers[2].overlapGroup == 0U, "Reused marker snapshots must clear overlap data when a marker becomes hidden.");
		});
	}
}
