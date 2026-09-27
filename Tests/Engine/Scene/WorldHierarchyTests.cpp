// WorldHierarchyTests.cpp - canonical hierarchy and transform-space tests.

#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneGraphRuntime.h"

#include <optional>

namespace TerraEngine::Tests {
	void RegisterWorldHierarchyTests(TestRegistry& registry)
	{
		registry.Add("WorldHierarchy.WorldEditUpdatesRenderedChildPose", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Capsule root");
			const Object child = objects.createObject("Character visual");
			TerraEngine::Scene::WorldPose parentPose{};
			parentPose.position.cell.x = 20;
			parentPose.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
			parentPose.scale = {2, 3, 4};
			context.Expect(objects.SetCanonicalWorldPose(parent, parentPose) == TerraEngine::Scene::WorldCoordinateError::None, "Parent fixture must be valid.");
			context.Expect(objects.setParent(child, parent, ReparentMode::KeepLocal), "Attach the visual in the parent's bounded space.");
			// A world-space editor command must update the local data actually used by
			// parallel hierarchy propagation, including under rotation/nonuniform scale.
			auto desired = parentPose;
			desired.position.offsetMetres = {4, 6, -2};
			desired.rotation = glm::quat(1, 0, 0, 0);
			desired.scale = glm::vec3(0.321496f);
			context.Expect(objects.SetCanonicalWorldPose(child, desired) == TerraEngine::Scene::WorldCoordinateError::None, "World-space visual edit must succeed.");
			for (int frame = 0; frame != 3; ++frame)
			{
				const auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
				const auto& actual = graph.objects.at(objects.getUUID(child)).worldPose;
				context.Expect(actual.position.cell == desired.position.cell && glm::length(actual.position.offsetMetres - desired.position.offsetMetres) < 1e-5, "Propagation must retain the requested world position across frames.");
				context.Expect(glm::length(actual.scale - desired.scale) < 1e-6f, "Propagation must retain the visual-to-capsule scale.");
				context.Expect(glm::length(actual.rotation - desired.rotation) < 1e-6f, "Propagation must retain the requested world orientation.");
			}
			objects.clear();
		});

		registry.Add("WorldHierarchy.WorldEditResolvesMovedAncestor", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object root = objects.createObject("Root");
			const Object middle = objects.createObject("Middle");
			const Object child = objects.createObject("Visual");
			context.Expect(objects.setParent(middle, root, ReparentMode::KeepLocal) && objects.setParent(child, middle, ReparentMode::KeepLocal), "Three-level fixture must attach.");
			TerraEngine::Scene::WorldPose moved{};
			moved.position.offsetMetres.x = 10;
			moved.scale = glm::vec3(2);
			context.Expect(objects.SetCanonicalWorldPose(root, moved) == TerraEngine::Scene::WorldCoordinateError::None, "Move only the ancestor, leaving the middle's stored world pose stale.");
			auto desired = moved;
			desired.position.offsetMetres.x = 12;
			desired.scale = glm::vec3(0.3f);
			context.Expect(objects.SetCanonicalWorldPose(child, desired) == TerraEngine::Scene::WorldCoordinateError::None, "Nested world edit must use the resolved parent.");
			const auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const auto& actual = graph.objects.at(objects.getUUID(child)).worldPose;
			context.Expect(glm::length(actual.position.offsetMetres - desired.position.offsetMetres) < 1e-6 && glm::length(actual.scale - desired.scale) < 1e-6f, "An ancestor move must not be applied twice to a later child world edit.");
			objects.clear();
		});

		registry.Add("WorldHierarchy.SingularParentRejectsWorldEditAtomically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Flattened parent");
			const Object child = objects.createObject("Child");
			context.Expect(objects.setParent(child, parent, ReparentMode::KeepLocal), "Fixture must attach before flattening.");
			auto parentPose = *objects.GetCanonicalWorldPose(parent);
			parentPose.scale.x = 0;
			context.Expect(objects.SetCanonicalWorldPose(parent, parentPose) == TerraEngine::Scene::WorldCoordinateError::None, "A root may have an authored zero scale.");
			const Transform before = *objects.GetComponent<Transform>(child);
			const auto revision = objects.GetObjectTransformRevision(child);
			auto desired = before.worldPose;
			desired.scale = glm::vec3(0.3f);
			context.Expect(objects.SetCanonicalWorldPose(child, desired) != TerraEngine::Scene::WorldCoordinateError::None, "A singular parent has no inverse for a child world edit.");
			const auto* after = objects.GetComponent<Transform>(child);
			context.Expect(after->worldPose.scale == before.worldPose.scale && after->parentRelativePose.scale == before.parentRelativePose.scale && objects.GetObjectTransformRevision(child) == revision, "Rejected conversion must change neither pose nor its revision.");
			objects.clear();
		});

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
