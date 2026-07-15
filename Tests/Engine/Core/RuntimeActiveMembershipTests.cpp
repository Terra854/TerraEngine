#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Scene/RuntimeLayout.h"

namespace TerraEngine::Tests {
	void RegisterRuntimeActiveMembershipTests(TestRegistry& registry)
	{
		registry.Add("Engine.RuntimeLayout.ActiveMembership.EmptySceneIsStable", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			const auto& snapshot = objects.GetRuntimeActiveMembershipSnapshot();

			context.Expect(snapshot.empty(), "Empty scenes should publish an empty membership snapshot.");
			context.ExpectEqual(static_cast<std::uint64_t>(0), objects.GetRuntimeActiveMembershipRevision(), "Cleared scenes should reset membership revision.");
		});

		registry.Add("Engine.RuntimeLayout.ActiveMembership.InactiveAuthoredStateRemainsVisible", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object active = objects.createObject("Active");
			Object inactive = objects.createObject("Inactive");
			objects.setObjectActive(inactive, false);

			const auto activeObjects = objects.getObjectsWithComponents<Transform>();
			const auto authoredObjects = objects.getObjectsWithComponentsIncludingInactive<Transform>();
			const auto& membership = objects.GetRuntimeActiveMembershipSnapshot();

			context.ExpectEqual(static_cast<std::size_t>(1), activeObjects.size(), "Runtime membership should enumerate only active objects.");
			context.ExpectEqual(static_cast<std::size_t>(2), authoredObjects.size(), "Authored queries should still see inactive objects.");
			context.ExpectEqual(static_cast<std::size_t>(2), membership.size(), "Membership snapshot should retain authored rows for diagnostics.");
			context.Expect(membership[0].authoredStateVisible && membership[1].authoredStateVisible, "Runtime snapshot must not hide authored state.");
			context.Expect(!membership[0].objectHandle.IsEmpty(), "Runtime membership rows should carry compact references for worker-readable snapshots.");
			context.Expect(activeObjects.front().EntityId() == active.EntityId(), "Active object should survive compact membership filtering.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.ActiveMembership.ParentDisableSuppressesDescendants", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			Object grandchild = objects.createObject("Grandchild");
			context.Expect(objects.setParent(child, root), "Child should attach below the active root.");
			context.Expect(objects.setParent(grandchild, child), "Grandchild should attach below the active child.");

			objects.setObjectActive(root, false);
			const auto disabledBranch = objects.getObjectsWithComponents<Transform>();

			context.Expect(disabledBranch.empty(), "Disabling a root should remove its entire branch from runtime queries.");
			context.Expect(objects.isObjectActive(child), "Parent disable must preserve the child's authored active checkbox.");
			context.Expect(!objects.isObjectActiveInHierarchy(child), "Child should be effectively inactive while its parent is disabled.");
			context.Expect(!objects.isObjectActiveInHierarchy(grandchild), "Inherited inactivity should propagate through every descendant level.");

			objects.setObjectActive(root, true);
			const auto restoredBranch = objects.getObjectsWithComponents<Transform>();
			context.ExpectEqual(static_cast<std::size_t>(3), restoredBranch.size(), "Re-enabling the parent should restore authored-active descendants.");
			context.Expect(objects.isObjectActiveInHierarchy(grandchild), "Grandchild should become effectively active again.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.ActiveMembership.ReparentInvalidatesInheritedActivity", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object activeRoot = objects.createObject("ActiveRoot");
			Object inactiveRoot = objects.createObject("InactiveRoot");
			Object child = objects.createObject("Child");
			objects.setObjectActive(inactiveRoot, false);
			context.Expect(objects.setParent(child, activeRoot), "Child should begin below the active root.");
			(void)objects.GetRuntimeActiveMembershipSnapshot();

			context.Expect(objects.setParent(child, inactiveRoot), "Child should reparent below the inactive root.");
			const auto runtimeObjects = objects.getObjectsWithComponents<Transform>();

			context.ExpectEqual(static_cast<std::size_t>(1), runtimeObjects.size(), "Reparenting should rebuild inherited activity and leave only the active root visible.");
			context.Expect(!objects.isObjectActiveInHierarchy(child), "The moved child should inherit its new parent's inactive state immediately.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.ActiveMembership.RevisionChangesOnTogglesAndStructure", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Revision");
			const std::uint64_t createdRevision = objects.GetRuntimeActiveMembershipRevision();
			objects.setObjectActive(object, false);
			const std::uint64_t inactiveRevision = objects.GetRuntimeActiveMembershipRevision();
			objects.notifyObjectStructureChanged(object);

			context.Expect(inactiveRevision > createdRevision, "Active toggles should invalidate runtime membership.");
			context.Expect(objects.GetRuntimeActiveMembershipRevision() > inactiveRevision, "Component structure changes should invalidate runtime membership.");
			objects.clear();
		});

		registry.Add("Engine.RuntimeLayout.ActiveMembership.LargeMixedSceneMatchesCompatibility", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			constexpr std::size_t objectCount = 10000;
			constexpr std::size_t inactiveEvery = 5;
			for (std::size_t index = 0; index < objectCount; ++index)
			{
				Object object = objects.createObject("Stress");
				if (index % inactiveEvery == 0)
				{
					objects.setObjectActive(object, false);
				}
			}

			const auto runtimeObjects = objects.getObjectsWithComponents<Transform>();
			const TerraEngine::Scene::CompatibilitySnapshot compatibility = TerraEngine::Scene::BuildCompatibilitySnapshot(objects);

			context.ExpectEqual(compatibility.activeObjectIds.size(), runtimeObjects.size(), "Runtime active query should match compatibility active count.");
			context.ExpectEqual(objectCount - (objectCount / inactiveEvery), runtimeObjects.size(), "Large mixed scene active count should stay deterministic.");
			for (Object object : runtimeObjects)
			{
				context.Expect(objects.isObjectActive(object), "Large mixed scene runtime query should never return authored-inactive objects.");
			}
			objects.clear();
		});
	}
}
