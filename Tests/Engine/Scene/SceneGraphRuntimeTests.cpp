#include "../TestHarness.h"
#include "SceneGraphTestFixtures.h"

#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/Serializer.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(const glm::vec3& lhs, const glm::vec3& rhs)
		{
			constexpr float kTolerance = 0.001f;
			return glm::length(lhs - rhs) < kTolerance;
		}
	}

	void RegisterSceneGraphRuntimeTests(TestRegistry& registry)
	{
		registry.Add("SceneGraphRuntime.DeterministicTraversalParentsBeforeChildren", [](TestContext& context) {
			auto graph = SceneFixtures::MakeThreeLevelGraph();
			context.Expect(TerraEngine::Scene::ValidateSceneGraph(graph), "The fixture graph should validate before traversal.");

			const auto order = TerraEngine::Scene::BuildTraversalOrder(graph);
			context.ExpectEqual(static_cast<std::size_t>(3), order.size(), "The traversal should include every object once.");
			context.Expect(order[0] == SceneFixtures::MakeStableUuid(1), "The root should be visited first.");
			context.Expect(order[1] == SceneFixtures::MakeStableUuid(2), "The child should be visited after its parent.");
			context.Expect(order[2] == SceneFixtures::MakeStableUuid(3), "The grandchild should be visited last.");
		});

		registry.Add("SceneGraphRuntime.ValidationReportsInvalidHierarchy", [](TestContext& context) {
			auto graph = SceneFixtures::MakeThreeLevelGraph();
			graph.objects[SceneFixtures::MakeStableUuid(2)].parentId = SceneFixtures::MakeStableUuid(99);
			graph.objects[SceneFixtures::MakeStableUuid(3)].children.push_back(SceneFixtures::MakeStableUuid(1));

			context.Expect(!TerraEngine::Scene::ValidateSceneGraph(graph), "Missing parents and cycles should block validation.");
			const bool hasMissingParent = std::any_of(graph.diagnostics.begin(), graph.diagnostics.end(), [](const TerraEngine::Scene::SceneDiagnostic& diagnostic) {
				return diagnostic.message.find("missing parent") != std::string::npos;
			});
			const bool hasCycle = std::any_of(graph.diagnostics.begin(), graph.diagnostics.end(), [](const TerraEngine::Scene::SceneDiagnostic& diagnostic) {
				return diagnostic.message.find("Cycle") != std::string::npos;
			});
			context.Expect(hasMissingParent, "Validation should explain missing parent references.");
			context.Expect(hasCycle, "Validation should explain hierarchy cycles.");
		});

		registry.Add("SceneGraphRuntime.JsonBuilderReportsDuplicateIdentity", [](TestContext& context) {
			nlohmann::json sceneJson;
			sceneJson["objects"] = nlohmann::json::array({
				{{"uuid", TerraEngine::ToHexString(SceneFixtures::MakeStableUuid(7))}, {"name", "A"}},
				{{"uuid", TerraEngine::ToHexString(SceneFixtures::MakeStableUuid(7))}, {"name", "B"}}
			});

			auto graph = TerraEngine::Scene::BuildSceneGraphFromJson(sceneJson);
			const bool hasDuplicate = std::any_of(graph.diagnostics.begin(), graph.diagnostics.end(), [](const TerraEngine::Scene::SceneDiagnostic& diagnostic) {
				return diagnostic.message.find("Duplicate") != std::string::npos;
			});
			context.Expect(hasDuplicate, "Authored duplicate UUIDs should be reported.");
		});

		registry.Add("SceneGraphRuntime.ReparentPreservesWorldPosition", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			Object parent = objects.createObject("Parent");
			Object child = objects.createObject("Child");
			Object newParent = objects.createObject("NewParent");

			parent.getComponent<Transform>()->position = glm::vec3(10.0f, 0.0f, 0.0f);
			child.getComponent<Transform>()->position = glm::vec3(1.0f, 0.0f, 0.0f);
			newParent.getComponent<Transform>()->position = glm::vec3(-5.0f, 0.0f, 0.0f);

			context.Expect(objects.setParent(child, parent), "Initial parenting should succeed.");
			auto before = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const glm::vec3 worldBefore = before.objects.at(objects.getUUID(child.EntityId())).worldPosition;

			context.Expect(TerraEngine::Scene::ReparentPreserveWorld(objects, child, newParent), "Graph-backed reparent should succeed.");
			auto after = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const glm::vec3 worldAfter = after.objects.at(objects.getUUID(child.EntityId())).worldPosition;

			context.Expect(NearlyEqual(worldBefore, worldAfter), "World position should remain stable after reparenting.");
			objects.clear();
		});

		registry.Add("SceneGraphRuntime.JsonReloadKeepsIdentityAndHierarchyOrder", [](TestContext& context) {
			const TerraEngine::UUID rootId = SceneFixtures::MakeStableUuid(10);
			const TerraEngine::UUID childId = SceneFixtures::MakeStableUuid(11);

			nlohmann::json sceneJson;
			sceneJson["objects"] = nlohmann::json::array({
				{
					{"uuid", TerraEngine::ToHexString(rootId)},
					{"name", "Root"},
					{"children", nlohmann::json::array({
						{{"uuid", TerraEngine::ToHexString(childId)}, {"name", "Child"}}
					})}
				}
			});

			auto graph = TerraEngine::Scene::BuildSceneGraphFromJson(sceneJson);
			const auto order = TerraEngine::Scene::BuildTraversalOrder(graph);
			context.ExpectEqual(static_cast<std::size_t>(2), order.size(), "Reloaded JSON should produce two graph objects.");
			context.Expect(order[0] == rootId, "Reloaded traversal should keep the root identity first.");
			context.Expect(order[1] == childId, "Reloaded traversal should keep the child identity second.");
		});

		registry.Add("SceneGraphRuntime.ValidatesLargeFiveLevelHierarchy", [](TestContext& context) {
			auto graph = SceneFixtures::MakeLargeFiveLevelGraph(1000);
			context.Expect(TerraEngine::Scene::ValidateSceneGraph(graph), "A 1,000-object hierarchy should validate without errors.");
			const auto order = TerraEngine::Scene::BuildTraversalOrder(graph);
			context.ExpectEqual(static_cast<std::size_t>(1000), order.size(), "Traversal should include all 1,000 objects.");
		});

		registry.Add("SceneGraphRuntime.ObjectManagerMarksTransformRevisionsForHierarchyChanges", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			const std::uint64_t beforeTransformRevision = objects.GetObjectTransformRevision(child.EntityId());

			context.Expect(objects.setParent(child, root), "Parenting should succeed before checking dirty transform revisions.");
			const std::uint64_t afterTransformRevision = objects.GetObjectTransformRevision(child.EntityId());
			const std::uint64_t afterSpatialRevision = objects.GetObjectSpatialRevision(child.EntityId());

			context.Expect(afterTransformRevision > beforeTransformRevision, "Hierarchy changes should mark the moved branch transform-dirty.");
			context.Expect(afterSpatialRevision > 0u, "Hierarchy changes should mark dependent spatial bounds dirty.");
			objects.clear();
		});

		registry.Add("SceneGraphRuntime.SaveLoadPreservesActiveState", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, root), "Fixture parent assignment should succeed.");
			objects.setObjectActive(child, false);

			const nlohmann::json saved = Serializer::SerializeObject(root);
			objects.clear();
			Object reloadedRoot = Serializer::DeserializeObject(saved);
			const std::vector<Object> reloadedChildren = objects.getChildren(reloadedRoot);

			context.ExpectEqual(static_cast<std::size_t>(1), reloadedChildren.size(), "Reloaded root should keep its child.");
			context.Expect(reloadedRoot.isActive(), "Root should keep the default active state after reload.");
			context.Expect(!reloadedChildren.front().isActive(), "Inactive child should remain inactive after save/load.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getObjectsWithComponents<Transform>().size(), "Active-only queries should still skip the reloaded inactive child.");
			objects.clear();
		});
	}
}
