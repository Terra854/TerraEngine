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
			context.Expect(activeObjects.front().EntityId() == active.EntityId(), "Active object should survive compact membership filtering.");
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
			objects.clear();
		});
	}
}
