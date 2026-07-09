#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

#include <limits>

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewToolbarSnapTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Toolbar.SnapValuesAreSanitized", [](TestContext& context) {
			TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference preference;
			preference.snapEnabled = true;
			preference.translationSnap = -5.0f;
			preference.rotationSnapDegrees = 0.0f;
			preference.scaleSnap = std::numeric_limits<float>::infinity();
			preference.toolbarCompactMode = "wide";

			const auto sanitized = TerraEngine::Editor::SceneViewOverlay::SanitizePreference(preference);
			context.ExpectEqual(0.5f, sanitized.translationSnap, "Invalid translation snap should fall back.");
			context.ExpectEqual(15.0f, sanitized.rotationSnapDegrees, "Invalid rotation snap should fall back.");
			context.ExpectEqual(0.1f, sanitized.scaleSnap, "Invalid scale snap should fall back.");
			context.ExpectEqual(std::string("auto"), sanitized.toolbarCompactMode, "Unknown compact modes should normalize.");
		});
	}
}
