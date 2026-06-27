#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"
#include "Scene/RuntimeLayout.h"
#include "Scene/SceneGraphRuntime.h"

namespace TerraEngine::Tests {
	void RegisterRuntimeHierarchyLayoutTests(TestRegistry& registry)
	{
		registry.Add("Engine.RuntimeLayout.Hierarchy.ReusesUnchangedHierarchyAndMembership", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, root), "Hierarchy fixture should be valid.");
			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);
			const TerraEngine::Scene::FlatHierarchyLayout first = TerraEngine::Scene::BuildFlatHierarchyLayout(objects, snapshot);

			const TerraEngine::Scene::FlatHierarchyLayout reused = TerraEngine::Scene::BuildFlatHierarchyLayout(objects, snapshot, &first);

			context.Expect(reused.reused, "Unchanged hierarchy and active membership revisions should reuse retained layout.");
			context.ExpectEqual(first.entries.size(), reused.entries.size(), "Reused layout should preserve entries.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.Hierarchy.ParentBeforeChildAndDiagnosticsFormat", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, root), "Hierarchy fixture should be valid.");
			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);
			const TerraEngine::Scene::FlatHierarchyLayout layout = TerraEngine::Scene::BuildFlatHierarchyLayout(objects, snapshot);
			const TerraEngine::Scene::LayoutDiagnosticsSummary summary = TerraEngine::Scene::BuildLayoutDiagnosticsSummary(layout, 2, 0, 42);
			const std::string formatted = TerraEngine::Scene::FormatLayoutDiagnosticsSummary(summary);

			context.Expect(layout.valid, "Flat hierarchy layout should validate.");
			context.Expect(layout.entries[1].parentIndex.has_value(), "Child should carry a parent index.");
			context.Expect(*layout.entries[1].parentIndex < 1, "Parent index should point before the child.");
			context.Expect(formatted.find("active=2") != std::string::npos, "Diagnostics should include active count.");
			context.Expect(formatted.find("frame=42") != std::string::npos, "Diagnostics should include frame id.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.Hierarchy.TransformRecordsUsePropagationOutput", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, root), "Hierarchy fixture should be valid.");
			TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects, false);
			const TerraEngine::Scene::SceneTransformPropagationResult propagation = TerraEngine::Scene::PropagateWorldTransforms(graph, TerraEngine::Scene::BuildDirtyTransformState(graph));
			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);
			const TerraEngine::Scene::FlatHierarchyLayout layout = TerraEngine::Scene::BuildFlatHierarchyLayout(objects, snapshot);

			const std::vector<TerraEngine::Scene::TransformRuntimeRecord> records = TerraEngine::Scene::BuildTransformRuntimeRecords(layout, propagation);

			context.ExpectEqual(static_cast<std::size_t>(2), records.size(), "Transform runtime records should cover active hierarchy entries.");
			context.Expect(records[0].processed || records[1].processed, "At least one runtime transform record should be marked processed from propagation.");
			objects.clear();
		});
	}
}
