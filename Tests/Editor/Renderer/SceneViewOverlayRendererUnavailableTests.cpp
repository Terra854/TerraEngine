#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewOverlayRendererUnavailableTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.RendererUnavailable.DiagnosticsRemainReadable", [](TestContext& context) {
			TerraEngine::Editor::SceneViewOverlay::SceneViewDiagnosticsSnapshot snapshot;
			snapshot.rendererAvailable = false;
			snapshot.disabledOverlayReason = "Renderer unavailable";
			const auto json = TerraEngine::Editor::SceneViewOverlay::SerializeDiagnosticsSnapshot(snapshot);
			context.Expect(!json.value("rendererAvailable", true), "Renderer availability should be published.");
			context.Expect(json.value("disabledOverlayReason", std::string()).find("Renderer") != std::string::npos, "Disabled reason should be readable.");
		});
	}
}
