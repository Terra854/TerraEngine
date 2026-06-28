#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"

namespace TerraEngine::EditorTests {
	void RegisterTransactionalMutationUndoTests(TestRegistry& registry)
	{
		registry.Add("Editor.TransactionalMutation.CarriesUndoGroupMetadata", [](TestContext& context) {
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("editor-undo", TerraEngine::Scene::TransactionOrigin::Editor, "inspector")
				.SetUndoGroup("Inspector Edit")
				.Build();

			context.ExpectEqual(std::string("Inspector Edit"), transaction.undoGroupName, "Editor transactions should carry one undo group name.");
		});
	}
}
