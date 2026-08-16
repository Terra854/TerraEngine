// WorldSpatialTransitionTests.cpp - atomic canonical transition tests.

#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/TransactionalMutation.h"

namespace TerraEngine::Tests {
	void RegisterWorldSpatialTransitionTests(TestRegistry& registry)
	{
		registry.Add("WorldSpatialTransition.MultiCellTranslateIsAtomic", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Mover");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			TerraEngine::Scene::TransactionalMutationBuilder builder("multi-cell", TerraEngine::Scene::TransactionOrigin::Test, "world transition test");
			TerraEngine::Scene::WorldDelta delta{};
			delta.cellDelta.x = 3;
			delta.offsetMetres.x = 2.0;
			builder.ObserveScene(objects).TranslateWorld(objects, objectId, delta);
			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(objects, builder.Build());
			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "A valid typed multi-cell movement should commit atomically.");
			context.Expect(objects.GetCanonicalWorldPose(object)->position.cell.x == 3, "The exact cell delta should reach the protected live pose.");
			objects.clear();
		});
	}
}
