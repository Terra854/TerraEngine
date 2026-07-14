#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.DiagnosticsAccumulateCommittedAndRejectedResults", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionDiagnostics diagnostics;
			TerraEngine::Scene::Transaction accepted = TerraEngine::Scene::TransactionalMutationBuilder("tx-ok", TerraEngine::Scene::TransactionOrigin::Test, "diagnostics")
				.SetName(fixture.rootId, "DiagnosticsCommit")
				.Build();
			TerraEngine::Scene::Transaction rejected = TerraEngine::Scene::TransactionalMutationBuilder("tx-budget", TerraEngine::Scene::TransactionOrigin::Test, "diagnostics")
				.SetBudget(TerraEngine::Scene::TransactionBudget{ 0, 128 })
				.SetName(fixture.rootId, "BudgetReject")
				.Build();

			const TerraEngine::Scene::TransactionResult acceptedResult = TerraEngine::Scene::CommitTransaction(fixture.objects, accepted);
			const TerraEngine::Scene::TransactionResult rejectedResult = TerraEngine::Scene::CommitTransaction(fixture.objects, rejected);
			TerraEngine::Scene::AccumulateTransactionDiagnostics(diagnostics, accepted, acceptedResult);
			TerraEngine::Scene::AccumulateTransactionDiagnostics(diagnostics, rejected, rejectedResult);

			context.ExpectEqual(static_cast<std::uint64_t>(2), diagnostics.submittedCount, "Diagnostics should count submitted transactions.");
			context.ExpectEqual(static_cast<std::uint64_t>(1), diagnostics.committedCount, "Diagnostics should count committed transactions.");
			context.ExpectEqual(static_cast<std::uint64_t>(1), diagnostics.rejectedCount, "Diagnostics should count rejected transactions.");
			context.Expect(!diagnostics.recentResults.empty(), "Recent results should be retained.");
			context.Expect(diagnostics.recentResults.front().inverseDeltas.empty(),
				"Bounded diagnostics must not retain heavyweight undo payloads after the caller receives the full result.");
			context.ExpectEqual(static_cast<std::size_t>(1), acceptedResult.inverseDeltas.size(),
				"Discarding diagnostic payload copies must not remove the caller-owned inverse used by undo and rollback.");
		});
	}
}
