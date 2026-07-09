#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.h"

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewOverlaySchedulerTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Scheduler.DeclaresResourcesAndBoundaries", [](TestContext& context) {
			const auto declaration = TerraEngine::Editor::SceneViewOverlay::BuildSceneViewOverlaySchedulerDeclaration();

			context.ExpectEqual(std::string("EditorUI"), declaration.schedulerPhase, "Scene View overlays should run in the editor UI phase.");
			context.Expect(!declaration.readResources.empty(), "Scene View overlay declaration should name read resources.");
			context.Expect(!declaration.writeResources.empty(), "Scene View overlay declaration should name write resources.");
			context.Expect(declaration.workerSafePreparation.find("immutable") != std::string::npos, "Worker-safe overlay preparation should use immutable snapshots.");
			context.Expect(declaration.protectedBoundary.find("ImGui") != std::string::npos, "Protected boundary should identify ImGui drawing/input ownership.");
			context.Expect(declaration.visibilityBoundary.find("transactional") != std::string::npos, "Visibility boundary should mention transactional/deferred mutation behavior.");
		});
	}
}
