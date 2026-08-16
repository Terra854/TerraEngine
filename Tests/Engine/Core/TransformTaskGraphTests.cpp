#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Scene/SceneGraphRuntime.h"
#include "../Scene/SceneGraphTestFixtures.h"
#include "ThreadPool/ThreadPool.h"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		class ThreadPoolSession {
		public:
			ThreadPoolSession()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession()
			{
				ThreadPool::instance().shutdown();
			}
		};
	}

	void RegisterTransformTaskGraphTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransformTaskGraph.BuildsDeterministicLevelWorkUnits", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			std::vector<Object> parents{ root };
			for (int depth = 1; depth < 5; ++depth)
			{
				std::vector<Object> next;
				for (Object parent : parents)
				{
					for (int child = 0; child < 6; ++child)
					{
						Object object = objects.createObject("Node " + std::to_string(depth) + "." + std::to_string(child));
						objects.setParent(object, parent);
						next.push_back(object);
					}
				}
				parents = std::move(next);
			}

			auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const auto first = TerraEngine::Scene::BuildTransformWorkPlan(graph);
			const auto second = TerraEngine::Scene::BuildTransformWorkPlan(graph);

			context.Expect(first.valid, "Valid hierarchies should produce transform work plans.");
			context.ExpectEqual(static_cast<std::size_t>(5), first.units.size(), "Five hierarchy levels should produce five work units.");
			context.ExpectEqual(first.units.size(), second.units.size(), "Repeated transform planning should be deterministic.");
			context.ExpectEqual(first.units.back().objects.size(), second.units.back().objects.size(), "Deepest level object count should stay stable.");
			objects.clear();
		});

		registry.Add("Engine.TransformTaskGraph.RejectsInvalidHierarchyBeforeWorkerExecution", [](TestContext& context) {
			TerraEngine::Scene::SceneGraph graph;
			TerraEngine::Scene::SceneGraphObject root;
			root.objectId = TerraEngine::GenerateUUID();
			root.children.push_back(TerraEngine::GenerateUUID());
			graph.rootObjects.push_back(root.objectId);
			graph.objects.emplace(root.objectId, root);

			TerraEngine::Scene::ValidateSceneGraph(graph);
			const auto plan = TerraEngine::Scene::BuildTransformWorkPlan(graph);

			context.Expect(!plan.valid, "Missing child references should block transform worker planning.");
			context.Expect(!plan.diagnostics.empty(), "Invalid transform planning should carry scene diagnostics.");
		});

		registry.Add("Engine.TransformTaskGraph.LowersTransformUnitsIntoEcsGraphSummary", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "transform", "Transform" });
			TerraEngine::Scheduling::EcsTransformWorkUnit root;
			root.workUnitId = "level-0";
			root.hierarchyLevel = 0;
			root.profilingLabel = "Transform.level-0";
			graph.AddTransformWorkUnit(root);
			TerraEngine::Scheduling::EcsTransformWorkUnit child;
			child.workUnitId = "level-1";
			child.hierarchyLevel = 1;
			child.parentDependencies = { "level-0" };
			child.profilingLabel = "Transform.level-1";
			graph.AddTransformWorkUnit(child);

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Transform-only ECS graph should execute.");
			context.ExpectEqual(static_cast<std::size_t>(2), summary.transformWorkUnits.size(), "Transform work units should appear in ECS summaries.");
			context.Expect(summary.formatted.find("level-1") != std::string::npos, "Formatted summary should include transform work labels.");
		});

		registry.Add("Engine.TransformTaskGraph.PropagatesOnlyDirtyBranchesDeterministically", [](TestContext& context) {
			auto graph = TerraEngine::Scene::BuildSceneGraphFromJson({
				{"objects", nlohmann::json::array({
					{
						{"uuid", TerraEngine::ToHexString(TerraEngine::Tests::SceneFixtures::MakeStableUuid(1))},
						{"name", "Root"},
						{"children", nlohmann::json::array({
							{
								{"uuid", TerraEngine::ToHexString(TerraEngine::Tests::SceneFixtures::MakeStableUuid(2))},
								{"name", "DirtyChild"},
								{"children", nlohmann::json::array({
									{
										{"uuid", TerraEngine::ToHexString(TerraEngine::Tests::SceneFixtures::MakeStableUuid(3))},
										{"name", "Grandchild"},
									}
								})}
							},
							{
								{"uuid", TerraEngine::ToHexString(TerraEngine::Tests::SceneFixtures::MakeStableUuid(4))},
								{"name", "CleanSibling"},
							}
						})}
					}
				})}
			});
			graph.objects[TerraEngine::Tests::SceneFixtures::MakeStableUuid(2)].parentRelativePose.translationMetres = glm::dvec3(5.0, 0.0, 0.0);
			graph.objects[TerraEngine::Tests::SceneFixtures::MakeStableUuid(3)].parentRelativePose.translationMetres = glm::dvec3(2.0, 0.0, 0.0);

			const auto dirty = TerraEngine::Scene::BuildDirtyTransformState(
				graph,
				{ TerraEngine::Tests::SceneFixtures::MakeStableUuid(2) },
				TerraEngine::Scene::SceneTransformDirtyReason::LocalTransform);
			const auto result = TerraEngine::Scene::PropagateWorldTransforms(graph, dirty);

			context.Expect(result.valid, "Dirty branch propagation should remain valid for a valid hierarchy.");
			context.ExpectEqual(1u, result.dirtyRootCount, "Only the edited child should be a dirty root.");
			context.ExpectEqual(2u, result.processedObjectCount, "The dirty child and its grandchild should be recomputed.");
			context.ExpectEqual(2u, result.skippedCleanObjectCount, "The root and clean sibling should be skipped.");
			context.Expect(graph.objects.at(TerraEngine::Tests::SceneFixtures::MakeStableUuid(3)).worldPose.position.offsetMetres.x > 6.9, "Grandchild world transform should consume the updated parent transform.");
		});
	}
}
