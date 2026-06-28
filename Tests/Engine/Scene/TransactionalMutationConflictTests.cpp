#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Core/Object/ObjectManager.h"

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationConflictTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.RejectsStaleObservedObjectWithoutPartialWrites", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-stale", TerraEngine::Scene::TransactionOrigin::Test, "test")
				.ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "ShouldNotApply")
				.Build();
			fixture.objects.setObjectName(fixture.root.EntityId(), "ChangedOutsideTransaction");

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Conflicted, "Stale metadata observation should conflict.");
			context.ExpectEqual(std::string("ChangedOutsideTransaction"), fixture.objects.getObjectName(fixture.root.EntityId()), "Rejected transaction must not apply partial writes.");
		});

		registry.Add("Engine.TransactionalMutation.RejectsMissingTarget", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-missing", TerraEngine::Scene::TransactionOrigin::Test, "test")
				.SetName(fixture.rootId, "DeletedTarget")
				.Build();
			fixture.objects.removeObject(fixture.root.EntityId());

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Conflicted, "Missing target should conflict.");
			context.Expect(!result.conflicts.empty(), "Missing target should be reported.");
		});
	}
}
