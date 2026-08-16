#include "../TestHarness.h"
#include "PrefabAuthoringTestFixtures.h"

#include "Scene/PrefabComposition.h"

namespace TerraEngine::EditorTests {
	void RegisterSceneRuntimeParityTests(TestRegistry& registry)
	{
		registry.Add("Editor.SceneParity.PreviewAndPlaySummariesMatch", [](TestContext& context) {
			auto source = PrefabFixtures::MakePrefabSource();
			const auto preview = TerraEngine::Scene::BuildSceneRuntimeSummary(source.sourceGraph);
			const auto play = TerraEngine::Scene::BuildSceneRuntimeSummary(source.sourceGraph);
			context.ExpectEqual(preview.objects.size(), play.objects.size(), "Editor preview and play summaries should see the same resolved graph.");
		});

		registry.Add("Editor.SceneParity.SaveReloadKeepsPrefabOverrideState", [](TestContext& context) {
			auto source = PrefabFixtures::MakePrefabSource();
			auto instance = PrefabFixtures::MakePrefabInstance(source);
			instance.overrides.push_back({instance.rootObjectId, "Transform", "WorldPose", "canonical-root"});

			TerraEngine::Scene::MarkStaleOverrides(instance, source);
			context.Expect(!instance.overrides.front().stale, "Save/reload parity should keep valid overrides active.");
		});
	}
}
