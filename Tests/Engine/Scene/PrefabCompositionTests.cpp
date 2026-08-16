#include "../TestHarness.h"
#include "SceneGraphTestFixtures.h"

#include "Scene/PrefabComposition.h"

#include <unordered_map>

namespace TerraEngine::Tests {
	void RegisterPrefabCompositionTests(TestRegistry& registry)
	{
		registry.Add("PrefabComposition.ValidSourceAndInstanceResolve", [](TestContext& context) {
			TerraEngine::Scene::PrefabSource source;
			source.sourceId = SceneFixtures::MakeStableUuid(20);
			source.sourceGraph = SceneFixtures::MakeThreeLevelGraph();
			context.Expect(TerraEngine::Scene::ValidatePrefabSource(source), "A valid prefab source should pass validation.");

			TerraEngine::Scene::PrefabInstance instance;
			instance.instanceId = SceneFixtures::MakeStableUuid(21);
			instance.sourceId = source.sourceId;
			instance.rootObjectId = source.sourceGraph.rootObjects.front();
			const auto result = TerraEngine::Scene::ResolvePrefabInstance(source, instance);
			context.Expect(result.success, "A prefab instance should resolve from a valid source.");
			context.ExpectEqual(static_cast<std::size_t>(3), result.resolvedGraph.objects.size(), "Resolved prefab graph should include the source hierarchy.");
		});

		registry.Add("PrefabComposition.NestedPrefabCycleDiagnostics", [](TestContext& context) {
			TerraEngine::Scene::PrefabSource a;
			TerraEngine::Scene::PrefabSource b;
			a.sourceId = SceneFixtures::MakeStableUuid(30);
			b.sourceId = SceneFixtures::MakeStableUuid(31);
			a.nestedSourceIds.push_back(b.sourceId);
			b.nestedSourceIds.push_back(a.sourceId);

			std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::PrefabSource> sources;
			sources.emplace(a.sourceId, a);
			sources.emplace(b.sourceId, b);

			const auto diagnostics = TerraEngine::Scene::DetectPrefabDependencyCycles(sources);
			context.Expect(!diagnostics.empty(), "Cyclic nested prefab dependencies should be diagnosed.");
		});

		registry.Add("PrefabComposition.StaleOverrideDetectionAndReset", [](TestContext& context) {
			TerraEngine::Scene::PrefabSource source;
			source.sourceId = SceneFixtures::MakeStableUuid(40);
			source.sourceGraph = SceneFixtures::MakeThreeLevelGraph();

			TerraEngine::Scene::PrefabInstance instance;
			instance.sourceId = source.sourceId;
			instance.overrides.push_back({SceneFixtures::MakeStableUuid(2), "Transform", "WorldPose", "canonical-a"});
			instance.overrides.push_back({SceneFixtures::MakeStableUuid(99), "Transform", "WorldPose", "canonical-b"});

			TerraEngine::Scene::MarkStaleOverrides(instance, source);
			context.Expect(!instance.overrides[0].stale, "Overrides targeting source objects should stay active.");
			context.Expect(instance.overrides[1].stale, "Overrides targeting missing source objects should be stale.");

			TerraEngine::Scene::ResetOverridesToDefaults(instance, SceneFixtures::MakeStableUuid(2));
			context.ExpectEqual(static_cast<std::size_t>(1), instance.overrides.size(), "Targeted reset should remove matching overrides only.");
		});
	}
}
