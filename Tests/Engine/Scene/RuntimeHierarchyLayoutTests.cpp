#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"
#include "Scene/RuntimeLayout.h"
#include "Scene/SceneChangePacket.h"
#include "Scene/SceneGraphRuntime.h"

#include <algorithm>

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
			context.ExpectEqual(first.sourceMembershipRevision, reused.sourceMembershipRevision, "Reused layout should preserve membership freshness.");
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
			context.Expect(!layout.entries[0].objectHandle.IsEmpty(), "Flat hierarchy entries should carry compact references.");
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
			context.Expect(!records.front().objectHandle.IsEmpty(), "Transform runtime records should preserve compact object references.");
			context.Expect(records[0].processed || records[1].processed, "At least one runtime transform record should be marked processed from propagation.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.Hierarchy.DirtyRootPublishesOnlyAffectedDescendantRuntimeChanges", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Static Root");
			Object child = objects.createObject("Movable Child");
			context.Expect(objects.setParent(child, root), "Hierarchy fixture should be valid before dirty propagation.");
			objects.setObjectStatic(root, true);
			(void)objects.PublishSceneChangePacket();

			objects.MarkObjectTransformDirty(root);
			const TerraEngine::Scene::SceneChangePacket changes = objects.PublishSceneChangePacket();
			TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects, false);
			const TerraEngine::Scene::SceneDirtyTransformState dirtyState = TerraEngine::Scene::BuildDirtyTransformState(graph, changes);
			const TerraEngine::Scene::SceneTransformPropagationResult propagation = TerraEngine::Scene::PropagateWorldTransforms(graph, dirtyState);
			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);
			const TerraEngine::Scene::FlatHierarchyLayout layout = TerraEngine::Scene::BuildFlatHierarchyLayout(objects, snapshot);
			const TerraEngine::Scene::RuntimeLayoutChangeBatch batch = TerraEngine::Scene::BuildRuntimeLayoutChangeBatch(layout, propagation, dirtyState, changes);

			context.Expect(batch.valid, "Generation-valid dirty propagation should produce an exact compact runtime batch.");
			context.ExpectEqual(static_cast<std::size_t>(2), batch.transformChanges.size(), "A dirty root should include exactly its affected active descendant records.");
			context.ExpectEqual(static_cast<std::size_t>(1), batch.staticSpatialChanges.size(), "The static root update should remain in the static spatial partition.");
			context.ExpectEqual(static_cast<std::size_t>(1), batch.movableSpatialChanges.size(), "The child update should remain in the movable spatial partition.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.Hierarchy.CompactTransformsReuseGenerationTaggedParentWorldState", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object changedParent = objects.createObject("Changed Parent");
			Object changedChild = objects.createObject("Changed Child");
			Object stableParent = objects.createObject("Stable Parent");
			Object stableChild = objects.createObject("Stable Child");
			context.Expect(objects.setParent(changedChild, changedParent), "The changed branch fixture should be valid.");
			context.Expect(objects.setParent(stableChild, stableParent), "The stable branch fixture should be valid.");

			const TerraEngine::Scene::HierarchySnapshot hierarchy = TerraEngine::Scene::BuildHierarchySnapshot(objects);
			const TerraEngine::Scene::FlatHierarchyLayout layout = TerraEngine::Scene::BuildFlatHierarchyLayout(objects, hierarchy);
			const TerraEngine::Scene::SceneGraph firstGraph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const TerraEngine::Scene::CompactTransformRuntimeSnapshot first = TerraEngine::Scene::BuildCompactTransformRuntimeSnapshot(layout, firstGraph);
			const TerraEngine::Scene::CompactTransformRuntimeSnapshot reused = TerraEngine::Scene::BuildCompactTransformRuntimeSnapshot(layout, firstGraph, &first);

			context.Expect(first.valid, "A valid parent-before-child graph should produce a compact runtime snapshot.");
			context.ExpectEqual(static_cast<std::size_t>(4), first.records.size(), "The compact snapshot should retain all four high-frequency transforms.");
			context.Expect(sizeof(TerraEngine::Scene::CompactTransformRuntimeRecord) < sizeof(TerraEngine::Scene::TransformRuntimeRecord), "The high-frequency record should stay smaller than the compatibility record with an Object facade and matrix.");
			context.ExpectEqual(static_cast<std::size_t>(4), reused.reusedParentStateCount, "An unchanged graph should reuse every generation-tagged parent world state.");

			Transform* changedTransform = objects.GetComponent<Transform>(changedParent);
			changedTransform->position.x += 5.0f;
			objects.MarkObjectTransformDirty(changedParent);
			const TerraEngine::Scene::SceneGraph changedGraph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const TerraEngine::Scene::CompactTransformRuntimeSnapshot changed = TerraEngine::Scene::BuildCompactTransformRuntimeSnapshot(layout, changedGraph, &first);
			const auto findParentState = [&changed](const TerraEngine::UUID& objectId) {
				return std::find_if(changed.parentWorldStates.begin(), changed.parentWorldStates.end(), [&objectId](const auto& state) {
					return state.objectId == objectId;
				});
			};
			const auto firstChangedChildState = std::find_if(first.parentWorldStates.begin(), first.parentWorldStates.end(), [&objects, &changedChild](const auto& state) {
				return state.objectId == objects.getUUID(changedChild);
			});
			const auto changedChildState = findParentState(objects.getUUID(changedChild));
			const auto stableChildState = findParentState(objects.getUUID(stableChild));

			context.Expect(firstChangedChildState != first.parentWorldStates.end() && changedChildState != changed.parentWorldStates.end(), "The changed child should retain parent-cache records across snapshots.");
			context.Expect(changedChildState->generation != firstChangedChildState->generation, "Changing a parent should advance its child's parent-world generation.");
			context.Expect(stableChildState != changed.parentWorldStates.end() && stableChildState->reused, "An unrelated branch should preserve its reusable parent cache.");
			objects.clear();
		});
	}
}
