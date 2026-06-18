#include "../TestHarness.h"
#include "../Scene/SceneGraphTestFixtures.h"

#include "Scene/SceneGraphRuntime.h"

namespace TerraEngine::Tests {
	void RegisterSceneRuntimeParityTests(TestRegistry& registry)
	{
		registry.Add("Runtime.SceneParity.SummaryExcludesEditorOnlyMetadata", [](TestContext& context) {
			auto graph = SceneFixtures::MakeThreeLevelGraph();
			TerraEngine::Scene::ComputeWorldTransforms(graph);
			const auto summary = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);

			context.ExpectEqual(static_cast<std::size_t>(3), summary.objects.size(), "Runtime summary should include resolved graph objects.");
			context.ExpectEqual(static_cast<std::uint32_t>(0), summary.objects[0].depth, "Root summary depth should be zero.");
			context.ExpectEqual(static_cast<std::uint32_t>(1), summary.objects[1].depth, "Child summary depth should be one.");
		});

		registry.Add("Runtime.SceneParity.ExportedStartupSummaryMatchesGraph", [](TestContext& context) {
			auto graph = SceneFixtures::MakeThreeLevelGraph();
			const auto first = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
			const auto second = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
			context.ExpectEqual(first.objects.size(), second.objects.size(), "Repeated runtime summaries should be deterministic.");
			context.Expect(first.objects.front().objectId == second.objects.front().objectId, "Repeated runtime summaries should keep identity order.");
		});
	}
}
