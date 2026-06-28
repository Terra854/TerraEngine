#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Scene/SceneGraphRuntime.h"

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationSerializationTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.CommittedStateAppearsInSceneGraphSnapshot", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-snapshot", TerraEngine::Scene::TransactionOrigin::Test, "serialization")
				.SetName(fixture.rootId, "SnapshotName")
				.Build();

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			const TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(fixture.objects);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "Transaction should commit before snapshot.");
			context.Expect(graph.objects.contains(fixture.rootId), "Committed object should remain in snapshot.");
			context.ExpectEqual(std::string("SnapshotName"), graph.objects.at(fixture.rootId).name, "Snapshot should contain committed metadata.");
		});
	}
}
