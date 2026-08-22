#include "TestHarness.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"

#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
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
			IntegerDeltaCommand(int& value, int before, int after, std::string domain = {})
				: value(value), before(before), after(after), domain(std::move(domain))
			{
			}

			void Undo() override { value = before; }
			void Redo() override { value = after; }
			std::string GetName() const override { return "Integer delta"; }
			std::string_view GetHistoryDomain() const override { return domain; }

		private:
			int& value;
			int before = 0;
			int after = 0;
			std::string domain;
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

		registry.Add("Editor.UndoRedo.SavePointTracksExactHistoryState", [](TestContext& context) {
			ScriptingSystem::Instance().SetRunning(false);
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			int value = 1;
			history.PushCommand(std::make_unique<IntegerDeltaCommand>(value, 0, 1));
			history.SetSavePoint();
			context.Expect(!SceneManager::Instance().IsActiveSceneDirty(), "Saving should mark the current history revision clean.");

			value = 2;
			history.PushCommand(std::make_unique<IntegerDeltaCommand>(value, 1, 2));
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "Editing after save should make the scene dirty.");
			history.Undo();
			context.ExpectEqual(1, value, "Undo should restore the saved value.");
			context.Expect(!SceneManager::Instance().IsActiveSceneDirty(), "Undoing to the saved revision should make the scene clean.");
			history.Undo();
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "Undoing before the saved revision should make the scene dirty.");
			history.Redo();
			context.Expect(!SceneManager::Instance().IsActiveSceneDirty(), "Redoing to the saved revision should make the scene clean again.");
			history.Redo();
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "Redoing past the saved revision should make the scene dirty.");
			history.Clear();
		});

		registry.Add("Editor.UndoRedo.PrunedSavedStateNeverAppearsClean", [](TestContext& context) {
			ScriptingSystem::Instance().SetRunning(false);
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			// Terrain workflows intentionally raise the configurable production
			// history limit. Pin this legacy pruning check to its documented 100-entry
			// budget so it is independent of earlier test and project configuration.
			history.SetHistoryBudget(100U, 512U * 1024U * 1024U);
			int value = 0;
			// The history retains 100 commands. Pushing one extra command makes the
			// original clean state unreachable, so undoing everything retained must
			// not falsely claim that the unsaved first edit is on disk.
			for (int next = 1; next <= 101; ++next)
			{
				const int before = value;
				value = next;
				history.PushCommand(std::make_unique<IntegerDeltaCommand>(value, before, value));
			}
			while (history.CanUndo())
			{
				history.Undo();
			}
			context.ExpectEqual(1, value, "Pruning should leave the oldest retained edited state.");
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "An unreachable saved revision must remain dirty after all retained undo steps.");
			history.Clear();
			history.SetHistoryBudget(1000U, 512U * 1024U * 1024U);
		});

		registry.Add("Editor.TransactionalMutation.CreatedObjectUndoRedoUsesProtectedStableSnapshotReplay", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();

			const auto local = TerraEngine::Scene::LocalObject(-1);
			TerraEngine::Scene::TransactionalMutationBuilder builder(
				"editor-create-undo", TerraEngine::Scene::TransactionOrigin::Editor, "TerrainCreateRegression");
			builder.SetUndoGroup("Create Terrain Render Instance")
				.CreateObject(-1, "Terrain")
				.SetStatic(local, true);
			const TerraEngine::Scene::Transaction transaction = builder.Build();
			queue.EnqueueTransaction(transaction);
			(void)queue.DrainTransactions(objects);
			auto completions = queue.DrainCompletedEditorTransactions();
			context.Expect(completions.size() == 1U,
				"The protected queue must retain an async editor request beside its commit receipt.");
			if (completions.empty()) return;
			const TerraEngine::Scene::TransactionResult& committed = completions.front().result;
			context.Expect(committed.status == TerraEngine::Scene::TransactionStatus::Committed
				&& committed.createdObjects.contains(-1), "The terrain-style local create transaction must commit with a stable result identity.");
			const TerraEngine::UUID terrainId = committed.createdObjects.at(-1);
			context.Expect(history.RecordCommittedTransaction(completions.front().transaction, committed),
				"A committed terrain-style create must enter editor history.");

			history.Undo();
			queue.Apply(objects, "editor-create-undo-boundary");
			auto receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1U && history.CompleteDeferredReplay(receipts.front()),
				"Create undo must finish only after its protected delete transaction commits.");
			context.Expect(!objects.getObject(terrainId), "Undo must remove the created terrain object.");

			history.Redo();
			queue.Apply(objects, "editor-create-redo-boundary");
			receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1U && history.CompleteDeferredReplay(receipts.front()),
				"Create redo must finish only after its protected snapshot transaction commits.");
			const std::optional<Object> restored = objects.getObject(terrainId);
			context.Expect(restored && objects.getObjectName(*restored) == "Terrain" && objects.isObjectStatic(*restored),
				"Redo must restore the same UUID and exact authored metadata.");

			history.Clear();
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.UndoRedo.DiscardingTerrainDomainPreservesUnrelatedHistory", [](TestContext& context) {
			ScriptingSystem::Instance().SetRunning(false);
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			int value = 2;
			history.PushCommand(std::make_unique<IntegerDeltaCommand>(value, 0, 1));
			history.PushCommand(std::make_unique<IntegerDeltaCommand>(value, 1, 2, "terrain-authoring"));
			history.Undo();
			history.DiscardHistoryDomain("terrain-authoring");
			context.Expect(history.GetUndoDepth() == 1U && history.GetRedoDepth() == 0U,
				"Replacing a terrain store must remove terrain commands from both stacks without clearing unrelated actions.");
			history.Clear();
		});

		registry.Add("Editor.TransactionalMutationUndo.PhysicsEditUndoRedoPreservesSelectionAndDirtyState", [](TestContext& context) {
			ScriptingSystem::Instance().SetRunning(false);
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			history.Clear();
			int colliderEnabled = 1;
			const TerraEngine::UUID selectedObject = GenerateUUID();
			const TerraEngine::UUID selectionAtSave = selectedObject;
			history.SetSavePoint();

			// A physics inspector edit changes authored state, but selection is editor
			// context rather than transaction payload and must remain stable through
			// the grouped undo/redo operations.
			colliderEnabled = 0;
			history.PushCommand(std::make_unique<IntegerDeltaCommand>(colliderEnabled, 1, 0));
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "A committed physics edit after a save point must mark the scene dirty.");
			context.Expect(selectionAtSave == selectedObject, "Committing a physics edit must preserve the active object selection identity.");

			history.Undo();
			context.ExpectEqual(1, colliderEnabled, "Undo must restore the authored collider value.");
			context.Expect(!SceneManager::Instance().IsActiveSceneDirty(), "Undoing to the saved authored state must clear dirty state.");
			context.Expect(selectionAtSave == selectedObject, "Undo must not replace the selected object identity.");

			history.Redo();
			context.ExpectEqual(0, colliderEnabled, "Redo must reapply the authored collider value.");
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "Redoing past the save point must restore dirty state.");
			context.Expect(selectionAtSave == selectedObject, "Redo must not replace the selected object identity.");
			history.Clear();
		});
	}
}
