#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SpatialIndex.h"

namespace TerraEngine::Tests {
	void RegisterHierarchySnapshotTests(TestRegistry& registry)
	{
		registry.Add("Engine.HierarchySnapshot.ParentsAppearBeforeChildren", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object parent = objects.createObject("Parent");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, parent), "Fixture parent assignment should succeed.");

			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);

			context.Expect(snapshot.valid, "Simple hierarchy snapshot should be valid.");
			context.ExpectEqual(static_cast<std::size_t>(2), snapshot.orderedObjects.size(), "Snapshot should include both objects.");
			context.Expect(snapshot.orderedObjects[0].objectId == objects.getUUID(parent), "Parent should appear before child.");
			context.ExpectEqual(static_cast<std::int64_t>(0), snapshot.orderedObjects[1].parentIndex, "Child should point at parent index.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.RevisionChangesAfterReparent", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object parent = objects.createObject("Parent");
			Object child = objects.createObject("Child");
			const std::uint64_t before = objects.GetHierarchyRevision();
			context.Expect(objects.setParent(child, parent), "Reparent should succeed.");
			context.Expect(objects.GetHierarchyRevision() > before, "Hierarchy revision should advance after reparent.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.ActiveToggleInvalidatesRuntimeHierarchyFreshness", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object parent = objects.createObject("Parent");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, parent), "Fixture parent assignment should succeed.");
			const std::uint64_t hierarchyBefore = objects.GetHierarchyRevision();
			const std::uint64_t membershipBefore = objects.GetRuntimeActiveMembershipRevision();

			objects.setObjectActive(child, false);

			context.Expect(objects.GetHierarchyRevision() > hierarchyBefore, "Active toggles should invalidate hierarchy freshness for runtime layout consumers.");
			context.Expect(objects.GetRuntimeActiveMembershipRevision() > membershipBefore, "Active toggles should invalidate active membership freshness.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.FormattedSummaryIsDeterministic", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			objects.createObject("A");
			objects.createObject("B");
			const std::string first = TerraEngine::Scene::BuildHierarchySnapshot(objects).formatted;
			const std::string second = TerraEngine::Scene::BuildHierarchySnapshot(objects).formatted;
			context.ExpectEqual(first, second, "Unchanged hierarchy snapshots should format deterministically.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.ReusesUnchangedRevision", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object parent = objects.createObject("Parent");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, parent), "Fixture parent assignment should succeed.");
			const TerraEngine::Scene::HierarchySnapshot first = TerraEngine::Scene::BuildHierarchySnapshot(objects);

			const TerraEngine::Scene::HierarchySnapshot reused = TerraEngine::Scene::BuildHierarchySnapshot(objects, &first);

			context.Expect(reused.valid, "Reused hierarchy snapshot should remain valid.");
			context.Expect(reused.reused, "Unchanged hierarchy revision should reuse the previous snapshot.");
			context.ExpectEqual(first.orderedObjects.size(), reused.orderedObjects.size(), "Reused snapshot should preserve object order.");
			context.Expect(reused.formatted.find("reused=true") != std::string::npos, "Formatted snapshot should expose reuse diagnostics.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.InvalidPreviousSnapshotDoesNotBlockRebuild", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			objects.createObject("Root");
			TerraEngine::Scene::HierarchySnapshot invalidPrevious;
			invalidPrevious.revision = objects.GetHierarchyRevision();
			invalidPrevious.valid = false;
			invalidPrevious.formatted = TerraEngine::Scene::FormatHierarchySnapshot(invalidPrevious);

			const TerraEngine::Scene::HierarchySnapshot rebuilt = TerraEngine::Scene::BuildHierarchySnapshot(objects, &invalidPrevious);

			context.Expect(rebuilt.valid, "Invalid cached snapshot should not block a fresh rebuild.");
			context.Expect(!rebuilt.reused, "Invalid cached snapshot should be rebuilt even when revisions match.");
			context.ExpectEqual(static_cast<std::size_t>(1), rebuilt.orderedObjects.size(), "Rebuilt snapshot should include current scene objects.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.InvalidSnapshotBlocksTransformPlanning", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			objects.createObject("Root");
			TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects, false);
			TerraEngine::Scene::HierarchySnapshot invalidSnapshot;
			invalidSnapshot.valid = false;

			const TerraEngine::Scene::SceneTransformWorkPlan plan = TerraEngine::Scene::BuildTransformWorkPlan(graph, invalidSnapshot);

			context.Expect(!plan.valid, "Invalid hierarchy snapshots should block transform work planning.");
			context.Expect(!plan.diagnostics.empty(), "Blocked transform planning should publish hierarchy diagnostics.");
			context.Expect(plan.diagnostics.back().message.find("Hierarchy snapshot is invalid") != std::string::npos, "Diagnostic should name the invalid hierarchy snapshot.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.DepthRangesDriveTransformWorkPlan", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, root), "Fixture parent assignment should succeed.");
			const TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects, false);
			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);

			const TerraEngine::Scene::SceneTransformWorkPlan plan = TerraEngine::Scene::BuildTransformWorkPlan(graph, snapshot);

			context.Expect(plan.valid, "Snapshot-backed transform work plan should validate.");
			context.ExpectEqual(snapshot.depthRanges.size(), plan.units.size(), "Transform work units should mirror hierarchy depth ranges.");
			context.ExpectEqual(std::string("level-0"), plan.units.front().workUnitId, "Root depth range should produce level-0 work.");
			context.Expect(TerraEngine::Scene::IsHierarchySnapshotWorkerReadable(snapshot, snapshot.publishedRevision), "Valid published hierarchy snapshots should be worker-readable.");
			context.Expect(snapshot.formatted.find("workerReadable=true") != std::string::npos, "Snapshot diagnostics should expose worker-readable publication state.");
			objects.clear();
		});

		registry.Add("Engine.HierarchySnapshot.RebuildInvalidatesSpatialIndex", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			index.InvalidateForHierarchySnapshotRebuild(7);
			const TerraEngine::Scene::SpatialDiagnosticsSnapshot first = index.GetDiagnosticsSnapshot();
			index.InvalidateForHierarchySnapshotRebuild(7);
			const TerraEngine::Scene::SpatialDiagnosticsSnapshot second = index.GetDiagnosticsSnapshot();

			context.ExpectEqual(static_cast<std::uint32_t>(1), first.revisionMismatchFallbackCount, "Hierarchy snapshot rebuild should request a spatial fallback.");
			context.ExpectEqual(first.revisionMismatchFallbackCount, second.revisionMismatchFallbackCount, "Repeated same-revision invalidation should not double count.");
		});

		registry.Add("Engine.HierarchySnapshot.BuildsTenThousandObjectDepthRanges", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			constexpr std::size_t objectCount = 10000;
			Object root = objects.createObject("Root");
			for (std::size_t index = 1; index < objectCount; ++index)
			{
				Object child = objects.createObject("Child");
				context.Expect(objects.setParent(child, root), "Wide hierarchy fixture should be valid.");
			}

			const TerraEngine::Scene::HierarchySnapshot snapshot = TerraEngine::Scene::BuildHierarchySnapshot(objects);

			// A wide scene should collapse into two deterministic depth ranges:
			// the root at depth zero and every child at depth one.
			context.Expect(snapshot.valid, "Large hierarchy snapshot should be valid.");
			context.ExpectEqual(objectCount, snapshot.orderedObjects.size(), "Snapshot should include the whole large hierarchy.");
			context.ExpectEqual(static_cast<std::size_t>(2), snapshot.depthRanges.size(), "Wide hierarchy should produce root and child depth ranges.");
			context.ExpectEqual(objectCount, snapshot.depthRanges.back().end, "Final depth range should cover all children.");
			objects.clear();
		});
	}
}
