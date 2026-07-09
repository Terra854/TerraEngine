#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/EditorPreferences.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewOverlayPreferenceTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Preferences.RoundTripAndFallback", [](TestContext& context) {
			EditorPreferences preferences;
			preferences.sceneViewOverlay.iconsVisible = false;
			preferences.sceneViewOverlay.projectionMode = TerraEngine::Editor::SceneViewOverlay::ProjectionMode::Orthographic;
			preferences.sceneViewOverlay.translationSnap = 2.0f;

			nlohmann::json json = preferences;
			const EditorPreferences restored = json.get<EditorPreferences>();
			context.Expect(!restored.sceneViewOverlay.iconsVisible, "Icon visibility should persist.");
			context.ExpectEqual(std::string("orthographic"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(restored.sceneViewOverlay.projectionMode)), "Projection mode should persist.");
			context.ExpectEqual(2.0f, restored.sceneViewOverlay.translationSnap, "Valid snap values should persist.");

			nlohmann::json invalid = nlohmann::json::object({{"sceneViewOverlay", nlohmann::json::object({{"translationSnap", -1.0f}})}});
			const EditorPreferences fallback = invalid.get<EditorPreferences>();
			context.ExpectEqual(0.5f, fallback.sceneViewOverlay.translationSnap, "Invalid snap values should fall back during preference load.");
		});
	}
}
