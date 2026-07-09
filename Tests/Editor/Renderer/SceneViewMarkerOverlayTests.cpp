#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewMarkerOverlayTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Markers.ProjectAndCountCategories", [](TestContext& context) {
			std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> markers;
			markers.push_back({ "camera-a", TerraEngine::EMPTY_UUID, entt::null, "Camera", TerraEngine::Editor::SceneViewOverlay::MarkerCategory::Camera, glm::vec3(0.0f, 0.0f, -2.0f) });
			markers.push_back({ "light-a", TerraEngine::EMPTY_UUID, entt::null, "Key", TerraEngine::Editor::SceneViewOverlay::MarkerCategory::PointLight, glm::vec3(0.25f, 0.0f, -2.0f) });
			for (auto& marker : markers)
			{
				const bool projected = TerraEngine::Editor::SceneViewOverlay::ProjectMarkerToScreen(marker, glm::mat4(1.0f), glm::vec2(0.0f), glm::vec2(100.0f));
				context.Expect(projected, marker.markerId + " should project into the viewport.");
			}
			auto snapshot = TerraEngine::Editor::SceneViewOverlay::CountMarkersIntoSnapshot({}, markers);
			context.ExpectEqual(1u, snapshot.cameraMarkerCount, "Visible camera marker should be counted.");
			context.ExpectEqual(1u, snapshot.pointLightMarkerCount, "Visible point light marker should be counted.");
		});
	}
}
