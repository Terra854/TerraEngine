#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"

namespace TerraEngine::EditorTests {
	void RegisterTransactionalMutationDiagnosticsWindowTests(TestRegistry& registry)
	{
		registry.Add("Editor.TransactionalMutation.DiagnosticsExposeRecentResultsForWindows", [](TestContext& context) {
			TerraEngine::Scene::TransactionDiagnostics diagnostics;
			TerraEngine::Scene::Transaction transaction;
			transaction.id = "editor-diagnostics";
			TerraEngine::Scene::TransactionResult result;
			result.transactionId = transaction.id;
			result.status = TerraEngine::Scene::TransactionStatus::Committed;
			result.appliedWriteCount = 1;

			TerraEngine::Scene::AccumulateTransactionDiagnostics(diagnostics, transaction, result);

			context.ExpectEqual(static_cast<std::uint64_t>(1), diagnostics.committedCount, "Editor diagnostics should see committed transaction count.");
			context.ExpectEqual(std::string("editor-diagnostics"), diagnostics.recentResults.back().transactionId, "Recent result should identify transaction.");
		});
	}
}
