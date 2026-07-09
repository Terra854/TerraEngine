#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewOverlayAccessibilityTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Accessibility.IconOnlyControlsAreNamed", [](TestContext& context) {
			const auto icons = TerraEngine::Editor::SceneViewOverlay::BuildDefaultIconDefinitions();
			for (const auto& icon : icons)
			{
				context.Expect(!icon.tooltip.empty(), icon.iconId + " should have tooltip text.");
				context.Expect(!icon.accessibilityName.empty(), icon.iconId + " should have an accessibility name.");
				context.Expect(icon.minimumHitSize >= 20.0f, icon.iconId + " should keep a compact but usable hit target.");
			}
		});
	}
}
