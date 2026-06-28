#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationPolicyTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.RejectsInvalidComponentPayloadBeforeCommit", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-invalid-component", TerraEngine::Scene::TransactionOrigin::Test, "policy")
				.UpdateComponentProperty(fixture.rootId, "Transform", "", {})
				.Build();

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected, "Invalid reflected component payload should be rejected.");
			context.Expect(!result.conflicts.empty(), "Policy rejection should carry a conflict record.");
		});
	}
}
