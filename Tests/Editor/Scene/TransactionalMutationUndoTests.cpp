#include "TestHarness.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/Serializer.h"

#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Components/MeshRenderComponent.h"
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
		registry.Add("Editor.TransactionalMutation.LegacyRemovalCapturesAuthoredPayloadUnderRegistrationLock", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance(); objects.clear(); MeshRenderComponent::registerType();
			Object object = objects.createObject("Locked Removal Target");
			auto* component = objects.AddComponent<MeshRenderComponent>(object);
			const auto meshId = TerraEngine::GenerateUUID(); component->meshId = meshId;
			std::unique_ptr<RemoveComponentCommand> command;
			{
				// Match automation's protected capture: the component registry remains
				// locked while the command snapshots the attached authored component.
				std::lock_guard lock(ComponentAccessRegistryMutex());
				command = std::make_unique<RemoveComponentCommand>(object.EntityId(), "MeshRenderComponent");
			}
			command->Redo();
			context.Expect(objects.GetComponent<MeshRenderComponent>(object) == nullptr, "Legacy removal must retire the component.");
			command->Undo();
			const auto* restored = objects.GetComponent<MeshRenderComponent>(object);
			context.Expect(restored && restored->meshId == meshId, "Prepared undo must restore the captured authored binding.");
			objects.clear();
		});

		registry.Add("Editor.TransactionalMutation.PreparedObjectRestoreUndoRedoUsesProtectedBoundary", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			auto& objects = ObjectManager::instance();
			auto& queue = SceneMutationQueue::Instance();
			auto& history = UndoRedoSystem::Instance();
			objects.clear(); queue.Clear(); history.Clear();
			const Object authored = objects.createObject("History Restore");
			const Object child = objects.createObject("History Child");
			objects.setParent(child, authored);
			const auto id = objects.getUUID(authored);
			const auto childId = objects.getUUID(child);
			const auto snapshot = Serializer::SerializeObject(authored.EntityId());
			objects.removeObject(authored);
			Transaction transaction;
			transaction.id = "snapshot-history"; transaction.origin = TransactionOrigin::Editor;
			transaction.undoGroupName = "Restore authored graph";
			TransactionWriteOperation restore;
			restore.kind = TransactionWriteKind::RestoreObjectSnapshot;
			restore.target = ExistingObject(id); restore.serializedObject = snapshot;
			transaction.writeSet.operations.push_back(restore);
			const auto committed = CommitTransaction(objects, transaction);
			context.Expect(committed.status == TransactionStatus::Committed && history.RecordCommittedTransaction(transaction, committed),
				"A prepared restore must commit and enter history as one operation.");
			history.Undo();
			context.Expect(objects.getObject(id).has_value(), "Undo submission must leave restoration visible until the protected boundary.");
			queue.Apply(objects, "restore-history-undo");
			auto receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1 && receipts.front().status == TransactionStatus::Committed && history.CompleteDeferredReplay(receipts.front()),
				"Undo must produce one committed protected receipt.");
			context.Expect(!objects.getObject(id) && !objects.getObject(childId), "Undo must remove the complete restored branch.");
			history.Redo();
			context.Expect(!objects.getObject(id), "Redo submission must wait for the protected boundary.");
			queue.Apply(objects, "restore-history-redo");
			receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1 && receipts.front().status == TransactionStatus::Committed && history.CompleteDeferredReplay(receipts.front()),
				"Redo must produce one committed prepared restoration receipt.");
			const auto restored = objects.getObject(id);
			context.Expect(restored && objects.getObject(childId)
				&& Serializer::SerializeObject(restored->EntityId()) == snapshot, "Redo must preserve exact authored values and stable hierarchy identities.");
			history.Clear(); queue.Clear(); objects.clear();
		});
		registry.Add("Editor.TransactionalMutation.RejectedDeferredReplayRestoresCursorAndSaveState", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			auto& objects = ObjectManager::instance();
			auto& queue = SceneMutationQueue::Instance();
			auto& history = UndoRedoSystem::Instance();
			objects.clear(); queue.Clear(); history.Clear();
			const Object object = objects.createObject("Before history edit");
			const auto id = objects.getUUID(object);
			TransactionalMutationBuilder edit("rejected-replay-history", TransactionOrigin::Editor);
			edit.SetUndoGroup("Rename replay fixture").SetName(id, "After history edit");
			const auto transaction = edit.Build();
			const auto committed = CommitTransaction(objects, transaction);
			context.Expect(committed.status == TransactionStatus::Committed && history.RecordCommittedTransaction(transaction, committed),
				"The real authored edit must enter history before replay is tested.");
			history.SetSavePoint();
			const auto commandId = history.GetUndoHistoryId();
			const auto historyBytes = history.GetHistoryBytes();

			// Exercise both stack directions. An intervening pair of real commits
			// restores the authored name but advances its observed metadata revision,
			// so the pending replay must conflict only at the protected queue flush.
			for (const bool undo : { true, false }) {
				const std::string currentName = undo ? "After history edit" : "Before history edit";
				const std::string replayedName = undo ? "Before history edit" : "After history edit";
				if (undo) history.Undo(); else history.Redo();
				context.Expect(objects.getObjectName(object) == currentName, "Replay admission must not expose its write before the protected flush.");
				TransactionalMutationBuilder conflict(undo ? "undo-replay-conflict" : "redo-replay-conflict", TransactionOrigin::Test);
				conflict.SetName(id, "Intervening revision");
				context.Expect(CommitTransaction(objects, conflict.Build()).status == TransactionStatus::Committed,
					"The intervening transaction must advance the captured read-set revision.");
				TransactionalMutationBuilder restore(undo ? "undo-conflict-restore" : "redo-conflict-restore", TransactionOrigin::Test);
				restore.SetName(id, currentName);
				context.Expect(CommitTransaction(objects, restore.Build()).status == TransactionStatus::Committed,
					"Restoring the fixture value must retain the newer revision.");
				queue.Apply(objects, undo ? "rejected-history-undo" : "rejected-history-redo");
				auto receipts = queue.DrainCompletedEditorTransactionResults();
				bool metadataConflict = false;
				if (receipts.size() == 1U) {
					for (const auto& conflictRecord : receipts.front().conflicts)
						metadataConflict = metadataConflict || conflictRecord.reason == TransactionConflictReason::MetadataVersionMismatch;
				}
				context.Expect(receipts.size() == 1U && receipts.front().status == TransactionStatus::Conflicted && metadataConflict
					&& receipts.front().appliedWriteCount == 0U, "The actual queued replay must reject its stale metadata read set without applying writes.");
				context.Expect(receipts.size() == 1U && history.CompleteDeferredReplay(receipts.front()),
					"The protected conflict receipt must repair the deferred history cursor.");
				context.Expect(objects.getObjectName(object) == currentName, "Rejected replay must preserve the current authored value.");
				context.Expect(history.GetUndoDepth() == (undo ? 1U : 0U) && history.GetRedoDepth() == (undo ? 0U : 1U)
					&& (undo ? history.GetUndoHistoryId() : history.GetRedoHistoryId()) == commandId
					&& (undo ? history.GetUndoLabel() : history.GetRedoLabel()) == "Rename replay fixture"
					&& history.GetHistoryBytes() == historyBytes, "Rejection must restore the original command ownership, stack position and accounting.");
				context.Expect(history.IsAtSavePoint() == undo && SceneManager::Instance().IsActiveSceneDirty() != undo,
					"Rejected replay must restore the exact prior saved-state cursor and dirty flag.");

				// Completing rejection must release the pending marker. The same command
				// now observes the current revisions and can commit on its next attempt.
				context.Expect(undo ? history.CanUndo() : history.CanRedo(), "The rejected command must be available for retry.");
				if (undo) history.Undo(); else history.Redo();
				queue.Apply(objects, undo ? "retried-history-undo" : "retried-history-redo");
				receipts = queue.DrainCompletedEditorTransactionResults();
				context.Expect(receipts.size() == 1U && receipts.front().status == TransactionStatus::Committed
					&& history.CompleteDeferredReplay(receipts.front()), "Retry must settle one actual committed queue receipt.");
				context.Expect(objects.getObjectName(object) == replayedName && history.IsAtSavePoint() != undo
					&& SceneManager::Instance().IsActiveSceneDirty() == undo, "Successful retry must publish the command and move to the intended saved-state cursor.");
			}
			history.Clear(); queue.Clear(); objects.clear();
		});
		registry.Add("Editor.TransactionalMutation.ChildWorldScaleUndoRedoUsesProtectedBoundary", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = SceneMutationQueue::Instance();
			auto& history = UndoRedoSystem::Instance();
			objects.clear();
			queue.Clear();
			history.Clear();
			const Object root = objects.createObject("Capsule root");
			const Object child = objects.createObject("Visual");
			context.Expect(objects.setParent(child, root, ReparentMode::KeepLocal), "Visual fixture must attach.");
			const auto childId = objects.getUUID(child);
			auto pose = *objects.GetCanonicalWorldPose(child);
			pose.scale = glm::vec3(0.321496f);
			TransactionalMutationBuilder builder("visual-scale-history", TransactionOrigin::Editor);
			builder.SetUndoGroup("Align character visual").SetWorldTransform(objects, childId, pose);
			const auto transaction = builder.Build();
			const auto result = CommitTransaction(objects, transaction);
			context.Expect(result.status == TransactionStatus::Committed && history.RecordCommittedTransaction(transaction, result), "Committed visual edit must enter editor history.");
			history.Undo();
			context.Expect(objects.GetComponent<Transform>(child)->parentRelativePose.scale == pose.scale, "Requesting undo must not expose local changes ahead of the protected flush.");
			queue.Apply(objects, "visual-scale-undo");
			auto receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1 && receipts.front().status == TransactionStatus::Committed && history.CompleteDeferredReplay(receipts.front()), "Undo must commit at its protected boundary.");
			context.Expect(BuildSceneGraphFromObjectManager(objects).objects.at(childId).worldPose.scale == glm::vec3(1), "Undo must restore rendered size, not just the Inspector cache.");
			history.Redo();
			queue.Apply(objects, "visual-scale-redo");
			receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1 && receipts.front().status == TransactionStatus::Committed && history.CompleteDeferredReplay(receipts.front()), "Redo must commit at its protected boundary.");
			context.Expect(BuildSceneGraphFromObjectManager(objects).objects.at(childId).worldPose.scale == pose.scale, "Redo must restore the intended character size.");
			history.Clear();
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.TransactionalMutation.MeshInspectorLifecyclePublishesRenderChanges", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Live mesh authoring");
			(void)objects.PublishSceneChangePacket();
			auto& access = ComponentAccessRegistry().at("MeshRenderComponent");
			std::uint64_t revision = objects.GetComponentMutationRevision();
			const auto expectPublication = [&] {
				// This is the immutable journal consumed by renderer membership
				// capture, not merely the scene's unsaved-file indicator.
				context.Expect(objects.GetComponentMutationRevision() > revision,
					"An Inspector/history edit must invalidate retained render capture.");
				revision = objects.GetComponentMutationRevision();
				const auto packet = objects.PublishSceneChangePacket();
				context.Expect(packet.Validate(objects.GetSceneGeneration()).valid
					&& packet.ComponentChanged().size() == 1U
					&& packet.ComponentChanged().front().objectId == objects.getUUID(object),
					"The next protected publication must identify the edited owner.");
			};
			AddComponentCommand add(object.EntityId(), "MeshRenderComponent");
			add.Redo();
			expectPublication();
			const auto meshId = TerraEngine::GenerateUUID();
			const auto materialId = TerraEngine::GenerateUUID();
			context.Expect(access.entitySetter(objects.getRegistry(), object.EntityId(), "meshId", meshId),
				"Inspector mesh assignment must succeed.");
			expectPublication();
			context.Expect(access.entitySetter(objects.getRegistry(), object.EntityId(), "materialId", materialId),
				"Inspector material assignment must succeed.");
			expectPublication();
			RemoveComponentCommand remove(object.EntityId(), "MeshRenderComponent");
			remove.Redo();
			expectPublication();
			context.Expect(objects.GetComponent<MeshRenderComponent>(object) == nullptr,
				"Removal must retire the live mesh binding.");
			remove.Undo();
			expectPublication();
			const auto* restored = objects.GetComponent<MeshRenderComponent>(object);
			context.Expect(restored && restored->meshId == meshId && restored->materialId == materialId,
				"Undo removal must restore both resource bindings before publication.");
			remove.Redo();
			expectPublication();
			add.Redo();
			expectPublication();
			add.Undo();
			expectPublication();
			objects.clear();
		});

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
