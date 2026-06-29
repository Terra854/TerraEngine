#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Core/Object/ObjectManager.h"

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationStressTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.CommitsLargeSimpleBatch", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("tx-large", TerraEngine::Scene::TransactionOrigin::Test, "stress");
			for (int index = 1; index <= 1000; ++index)
			{
				builder.CreateObject(index, "BatchObject");
			}

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "Large simple batch should commit.");
			context.ExpectEqual(static_cast<std::size_t>(1000), result.appliedWriteCount, "Every batch write should apply.");
		});

		registry.Add("Engine.TransactionalMutation.SmallScenePreflightOverheadDoesNotMutate", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-preflight", TerraEngine::Scene::TransactionOrigin::Test, "stress")
				.SetMode(TerraEngine::Scene::TransactionMode::PreflightOnly)
				.SetName(fixture.rootId, "PreflightOnly")
				.Build();

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::PreflightTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::PreflightAccepted, "Valid preflight should be accepted.");
			context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root), "Preflight should not mutate.");
		});
	}
}
