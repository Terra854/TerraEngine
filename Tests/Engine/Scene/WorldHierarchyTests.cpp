// WorldHierarchyTests.cpp - canonical hierarchy and transform-space tests.

#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneGraphRuntime.h"

#include <optional>

namespace TerraEngine::Tests {
	void RegisterWorldHierarchyTests(TestRegistry& registry)
	{
		registry.Add("WorldHierarchy.KeepWorldPreservesCrossCellCanonicalPose", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Parent");
			const Object child = objects.createObject("Child");
			TerraEngine::Scene::WorldPose parentPose{};
			parentPose.position.cell.x = 20;
			TerraEngine::Scene::WorldPose childPose{};
			childPose.position.cell.x = 20;
			childPose.position.offsetMetres.x = 1.0;
			context.Expect(objects.SetCanonicalWorldPose(parent, parentPose) == TerraEngine::Scene::WorldCoordinateError::None, "Parent fixture pose should normalize.");
			context.Expect(objects.SetCanonicalWorldPose(child, childPose) == TerraEngine::Scene::WorldCoordinateError::None, "Child fixture pose should normalize.");
			context.Expect(objects.setParent(child, parent, ReparentMode::KeepWorld), "KeepWorld should establish a bounded local relationship.");
			context.Expect(objects.GetCanonicalWorldPose(child)->position == childPose.position, "KeepWorld must not round or alter the canonical child address.");
			objects.clear();
		});

		registry.Add("WorldHierarchy.RejectsTransformSpaceRangeWithoutMutation", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Parent");
			const Object child = objects.createObject("Child");
			TerraEngine::Scene::WorldPose childPose{};
			childPose.position.offsetMetres.x = TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres + 1.0;
			context.Expect(objects.SetCanonicalWorldPose(child, childPose) == TerraEngine::Scene::WorldCoordinateError::None, "Fixture pose is a valid world position even when too far for one transform space.");
			context.Expect(!objects.setParent(child, parent, ReparentMode::KeepWorld), "Out-of-range hierarchy links must reject rather than silently lose precision.");
			context.Expect(!objects.getParent(child).has_value(), "Rejected KeepWorld reparenting must leave the live hierarchy unchanged.");
			objects.clear();
		});

		registry.Add("WorldHierarchy.KeepLocalPropagatesCanonicalPoseAcrossCellBoundary", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Parent");
			const Object child = objects.createObject("Child");
			TerraEngine::Scene::WorldPose parentPose{};
			parentPose.position.cell.x = 2;
			parentPose.position.offsetMetres.x = 32'767.0;
			context.Expect(objects.SetCanonicalWorldPose(parent, parentPose) == TerraEngine::Scene::WorldCoordinateError::None, "Parent fixture pose should normalize.");
			Transform* childTransform = objects.GetComponent<Transform>(child);
			context.Expect(childTransform != nullptr, "A new object must expose its canonical transform component.");
			if (childTransform != nullptr)
			{
				childTransform->parentRelativePose.translationMetres.x = 2.0;
				context.Expect(objects.setParent(child, parent, ReparentMode::KeepLocal), "KeepLocal should accept a bounded parent-relative pose.");
				const std::optional<TerraEngine::Scene::WorldPose> propagated = objects.GetCanonicalWorldPose(child);
				context.Expect(propagated.has_value() && propagated->position.cell.x == 3, "KeepLocal must normalize a child that crosses the parent cell boundary.");
				context.Expect(propagated.has_value() && propagated->position.offsetMetres.x == -32'767.0, "KeepLocal must retain the exact bounded local displacement after normalization.");
			}
			objects.clear();
		});
	}
}
