#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"

#include "Engine/Scripting/ScriptingSystem.h"
#include "UndoRedoSystem.h"

#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace
	{
		// A tiny reversible integer command proves grouping semantics without
		// depending on scene serialization or component registration details.
		class IntegerDeltaCommand final : public ICommand
		{
		public:
			IntegerDeltaCommand(int& value, int before, int after)
				: value(value), before(before), after(after)
			{
			}

			void Undo() override { value = before; }
			void Redo() override { value = after; }
			std::string GetName() const override { return "Integer delta"; }

		private:
			int& value;
			int before = 0;
			int after = 0;
		};
	}

	void RegisterTransactionalMutationUndoTests(TestRegistry& registry)
	{
		registry.Add("Editor.TransactionalMutation.CarriesUndoGroupMetadata", [](TestContext& context) {
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("editor-undo", TerraEngine::Scene::TransactionOrigin::Editor, "inspector")
				.SetUndoGroup("Inspector Edit")
				.Build();

			context.ExpectEqual(std::string("Inspector Edit"), transaction.undoGroupName, "Editor transactions should carry one undo group name.");
		});

		registry.Add("Editor.TransactionalMutation.CompositeGroupUndoesAndRedoesAsOneEntry", [](TestContext& context) {
			ScriptingSystem::Instance().SetRunning(false);
			UndoRedoSystem::Instance().Clear();
			int first = 10;
			int second = 20;
			std::vector<std::unique_ptr<ICommand>> commands;
			commands.push_back(std::make_unique<IntegerDeltaCommand>(first, 1, 10));
			commands.push_back(std::make_unique<IntegerDeltaCommand>(second, 2, 20));
			UndoRedoSystem::Instance().PushTransactionGroup("Automation group", std::move(commands));

			context.Expect(UndoRedoSystem::Instance().CanUndo(), "A non-empty automation group should create one undoable entry.");
			UndoRedoSystem::Instance().Undo();
			context.ExpectEqual(1, first, "One undo should restore the first command's before state.");
			context.ExpectEqual(2, second, "The same undo should restore the second command's before state.");
			context.Expect(!UndoRedoSystem::Instance().CanUndo(), "The composite group should consume only one history slot.");
			context.Expect(UndoRedoSystem::Instance().CanRedo(), "Undoing the group should make one composite redo available.");

			UndoRedoSystem::Instance().Redo();
			context.ExpectEqual(10, first, "One redo should restore the first command's after state.");
			context.ExpectEqual(20, second, "The same redo should restore the second command's after state.");
			context.Expect(!UndoRedoSystem::Instance().CanRedo(), "Reapplying the composite group should consume its one redo entry.");
			UndoRedoSystem::Instance().Clear();
		});
	}
}
