#include "../TestHarness.h"

#include "Core/Object/ActiveMembership.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"

#include <entt/entity/entity.hpp>

namespace TerraEngine::Tests {
	void RegisterActiveMembershipTests(TestRegistry& registry)
	{
		registry.Add("Engine.ActiveMembership.ActiveOnlyQueriesSkipInactiveObjects", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object active = objects.createObject("Active");
			Object inactive = objects.createObject("Inactive");
			objects.setObjectActive(inactive, false);

			const auto activeObjects = objects.getObjectsWithComponents<Transform>();
			const auto allObjects = objects.getObjectsWithComponentsIncludingInactive<Transform>();

			context.ExpectEqual(static_cast<std::size_t>(1), activeObjects.size(), "Active query should only return active objects.");
			context.ExpectEqual(entt::to_integral(active.EntityId()), entt::to_integral(activeObjects.front().EntityId()), "The active object should remain in query membership.");
			context.ExpectEqual(static_cast<std::size_t>(2), allObjects.size(), "Include-inactive query should preserve editor/serialization visibility.");
			objects.clear();
		});

		registry.Add("Engine.ActiveMembership.ToggleInvalidatesMembership", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Toggle");
			objects.setObjectActive(object, false);
			context.Expect(objects.getObjectsWithComponents<Transform>().empty(), "Inactive object should leave active membership.");
			objects.setObjectActive(object, true);
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getObjectsWithComponents<Transform>().size(), "Reactivated object should re-enter active membership.");
			objects.clear();
		});

		registry.Add("Engine.ActiveMembership.UnchangedSnapshotReportsCacheReuse", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			objects.createObject("Reusable");

			const auto& firstSnapshot = objects.GetRuntimeActiveMembershipSnapshot();
			const std::uint64_t firstRevision = objects.GetRuntimeActiveMembershipRevision();
			const auto& secondSnapshot = objects.GetRuntimeActiveMembershipSnapshot();

			context.ExpectEqual(firstRevision, objects.GetRuntimeActiveMembershipRevision(), "Unchanged membership reads should keep the same revision.");
			context.ExpectEqual(firstSnapshot.size(), secondSnapshot.size(), "Reused membership snapshot should preserve row count.");
			context.Expect(!secondSnapshot.empty() && secondSnapshot.front().reusedFromCache, "Second unchanged membership read should be tagged as cache reuse.");
			context.Expect(!secondSnapshot.empty() && !secondSnapshot.front().fallbackScannedFromMetadata, "Reused membership rows should not report a metadata scan fallback.");
			objects.clear();
		});

		registry.Add("Engine.ActiveMembership.StateHelperReportsActive", [](TestContext& context) {
			TerraEngine::ActiveMembershipState state;
			context.Expect(TerraEngine::IsActiveMember(state), "Default active membership state should be active.");
			state.objectActive = false;
			context.Expect(!TerraEngine::IsActiveMember(state), "Inactive membership state should report inactive.");
		});

		registry.Add("Engine.ActiveMembership.FiltersTenThousandObjectsDeterministically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			constexpr std::size_t objectCount = 10000;
			constexpr std::size_t inactiveEvery = 4;
			for (std::size_t index = 0; index < objectCount; ++index)
			{
				Object object = objects.createObject("ActiveStress");
				if (index % inactiveEvery == 0)
				{
					objects.setObjectActive(object, false);
				}
			}

			// The active query uses the membership tag instead of asking every
			// component owner for its state. This keeps large scene filtering on
			// the ECS view path and makes the expected count deterministic.
			const auto activeObjects = objects.getObjectsWithComponents<Transform>();
			const auto allObjects = objects.getObjectsWithComponentsIncludingInactive<Transform>();

			context.ExpectEqual(objectCount - (objectCount / inactiveEvery), activeObjects.size(), "Active-only query should skip exactly the inactive quarter.");
			context.ExpectEqual(objectCount, allObjects.size(), "Include-inactive query should still expose the whole scene for editor and serialization work.");
			objects.clear();
		});
	}
}
