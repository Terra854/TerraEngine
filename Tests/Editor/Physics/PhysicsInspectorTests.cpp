// Editor physics multi-edit, boundary, and undo regression tests.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/SceneManager.h"
#include "Scene/TransactionalMutation.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "PhysicsEditorAuthoringState.h"
#include "UndoRedoSystem.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	void RegisterPhysicsInspectorTests(TestRegistry& registry)
	{
		registry.Add("Editor.PhysicsInspector.StableColliderRecordOperations", [](TestContext& context) {
			ColliderSetComponent colliders;
			auto& first = colliders.Add();
			const UUID firstId = first.recordId;
			auto duplicate = first;
			duplicate.recordId = GenerateUUID();
			colliders.Add(duplicate);
			std::swap(colliders.colliders[0], colliders.colliders[1]);
			context.Expect(colliders.Remove(firstId), "Remove must address a collider by stable record ID after reordering.");
			context.ExpectEqual(static_cast<std::size_t>(1), colliders.colliders.size(), "Stable removal must remove exactly one record.");
			context.Expect(colliders.colliders.front().recordId == duplicate.recordId, "Duplicate records require a fresh stable record identity.");
		});

		registry.Add("Editor.PhysicsInspector.MultiEditIsAtomicAndUndoable", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			UndoRedoSystem::Instance().Clear();
			Object first = objects.createObject("First");
			Object second = objects.createObject("Second");
			RigidBodyComponent* firstBody = objects.AddComponent<RigidBodyComponent>(first);
			RigidBodyComponent* secondBody = objects.AddComponent<RigidBodyComponent>(second);
			firstBody = objects.GetComponent<RigidBodyComponent>(first);
			secondBody = objects.GetComponent<RigidBodyComponent>(second);
			firstBody->mass = 1.0F;
			secondBody->mass = 2.0F;

			Scene::TransactionalMutationBuilder invalidBuilder("physics-multi-invalid", Scene::TransactionOrigin::Editor, "InspectorPhysicsMultiEdit");
			invalidBuilder.ObserveScene(objects).ObserveObject(objects, objects.getUUID(first)).ObserveObject(objects, objects.getUUID(second))
				.SetUndoGroup("Edit Rigid Body Mass")
				.UpdateComponentProperty(objects.getUUID(first), "RigidBodyComponent", "mass", TerraEngine::Reflection::Variant{5.0F})
				.UpdateComponentProperty(objects.getUUID(second), "RigidBodyComponent", "mass", TerraEngine::Reflection::Variant{std::string("invalid")});
			const Scene::TransactionResult rejected = Scene::CommitTransaction(objects, invalidBuilder.Build());
			context.Expect(rejected.status != Scene::TransactionStatus::Committed, "One invalid selected payload must reject the complete edit.");
			context.ExpectEqual(1.0F, firstBody->mass, "A rejected batch must not partially change the first object.");
			context.ExpectEqual(2.0F, secondBody->mass, "A rejected batch must not partially change the second object.");

			Scene::TransactionalMutationBuilder validBuilder("physics-multi-valid", Scene::TransactionOrigin::Editor, "InspectorPhysicsMultiEdit");
			validBuilder.ObserveScene(objects).ObserveObject(objects, objects.getUUID(first)).ObserveObject(objects, objects.getUUID(second))
				.SetUndoGroup("Edit Rigid Body Mass")
				.UpdateComponentProperty(objects.getUUID(first), "RigidBodyComponent", "mass", TerraEngine::Reflection::Variant{7.0F})
				.UpdateComponentProperty(objects.getUUID(second), "RigidBodyComponent", "mass", TerraEngine::Reflection::Variant{7.0F});
			const Scene::Transaction transaction = validBuilder.Build();
			const Scene::TransactionResult committed = Scene::CommitTransaction(objects, transaction);
			context.Expect(committed.status == Scene::TransactionStatus::Committed, "Every valid selected payload must commit together.");
			context.ExpectEqual(static_cast<std::size_t>(2), committed.inverseDeltas.size(), "The receipt must retain one inverse delta per selected object.");
			context.Expect(UndoRedoSystem::Instance().RecordCommittedTransaction(transaction, committed), "A committed physics edit must create one grouped history item.");
			context.ExpectEqual(static_cast<std::size_t>(1), UndoRedoSystem::Instance().GetUndoDepth(), "Multi-edit must consume one undo slot.");
			UndoRedoSystem::Instance().Undo();
			queue.Apply(objects, "physics-multi-undo");
			std::vector<Scene::TransactionResult> undoReceipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(undoReceipts.size() == 1U
				&& UndoRedoSystem::Instance().CompleteDeferredReplay(undoReceipts.front()),
				"Grouped undo must finalize after its protected transaction commits.");
			context.ExpectEqual(1.0F, firstBody->mass, "Grouped undo must restore the first mixed value.");
			context.ExpectEqual(2.0F, secondBody->mass, "Grouped undo must restore the second mixed value.");
			UndoRedoSystem::Instance().Redo();
			queue.Apply(objects, "physics-multi-redo");
			std::vector<Scene::TransactionResult> redoReceipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(redoReceipts.size() == 1U
				&& UndoRedoSystem::Instance().CompleteDeferredReplay(redoReceipts.front()),
				"Grouped redo must finalize after its protected transaction commits.");
			context.ExpectEqual(7.0F, firstBody->mass, "Grouped redo must reapply the first object.");
			context.ExpectEqual(7.0F, secondBody->mass, "Grouped redo must reapply the second object.");
			UndoRedoSystem::Instance().Clear();
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.PhysicsInspector.ColliderShapeTransactionRestoresExactRecordOnUndoRedo", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			UndoRedoSystem::Instance().Clear();

			Object object = objects.createObject("Collider undo fixture");
			ColliderSetComponent* colliderSet = objects.AddComponent<ColliderSetComponent>(object);
			TerraEngine::Physics::ColliderRecord& authored = colliderSet->Add();
			const UUID recordId = authored.recordId;
			authored.localCenter = {0.0F, 1.0F, 2.0F};
			std::get<TerraEngine::Physics::BoxColliderShape>(authored.shape).halfExtents = {0.5F, 0.75F, 1.0F};

			std::vector<TerraEngine::Physics::ColliderRecord> edited = colliderSet->colliders;
			edited.front().localCenter = {3.0F, 4.0F, 5.0F};
			std::get<TerraEngine::Physics::BoxColliderShape>(edited.front().shape).halfExtents = {2.0F, 2.5F, 3.0F};
			Scene::TransactionalMutationBuilder builder(
				"collider-shape-undo", Scene::TransactionOrigin::Editor, "SceneViewPhysicsColliderHandles");
			builder.ObserveScene(objects)
				.ObserveObject(objects, objects.getUUID(object))
				.SetUndoGroup("Edit Physics Collider Shape")
				.UpdateComponentProperty(
					objects.getUUID(object), "ColliderSetComponent", "colliders",
					TerraEngine::Reflection::Variant{edited});
			const Scene::Transaction transaction = builder.Build();
			queue.EnqueueTransaction(transaction);
			const Scene::SceneMutationBatchResult applied = queue.Apply(objects, "collider-shape-apply");
			context.Expect(applied.applied && applied.transactionResults.size() == 1U,
				"The protected queue flush must commit the collider shape transaction.");
			const std::vector<Scene::TransactionResult> completed = queue.DrainCompletedEditorTransactionResults();
			context.Expect(completed.size() == 1U && !completed.front().inverseDeltas.empty(),
				"The editor receipt channel must retain the complete inverse payload removed from diagnostics.");
			const Scene::TransactionDiagnostics diagnostics = queue.GetTransactionDiagnostics();
			context.Expect(!diagnostics.recentResults.empty() && diagnostics.recentResults.back().inverseDeltas.empty(),
				"Diagnostics must remain lightweight while the separate editor receipt carries undo data.");
			context.Expect(!completed.empty()
				&& UndoRedoSystem::Instance().RecordCommittedTransaction(transaction, completed.front()),
				"A ColliderSet transaction completed by the live queue path must create one undo history item.");
			context.ExpectEqual(static_cast<std::size_t>(1), UndoRedoSystem::Instance().GetUndoDepth(),
				"One applied collider edit must register exactly one history entry.");

			auto expectCollider = [&](const TerraEngine::Physics::PhysicsVector3& center,
				const TerraEngine::Physics::PhysicsVector3& halfExtents, const char* message) {
				ColliderSetComponent* current = objects.GetComponent<ColliderSetComponent>(object);
				const bool validRecord = current != nullptr && current->colliders.size() == 1U
					&& current->colliders.front().recordId == recordId
					&& current->colliders.front().localCenter.x == center.x
					&& current->colliders.front().localCenter.y == center.y
					&& current->colliders.front().localCenter.z == center.z;
				bool validShape = false;
				if (validRecord)
				{
					const auto* box = std::get_if<TerraEngine::Physics::BoxColliderShape>(&current->colliders.front().shape);
					validShape = box != nullptr && box->halfExtents.x == halfExtents.x
						&& box->halfExtents.y == halfExtents.y && box->halfExtents.z == halfExtents.z;
				}
				context.Expect(validRecord && validShape, message);
			};

			expectCollider({3.0F, 4.0F, 5.0F}, {2.0F, 2.5F, 3.0F},
				"Apply must publish the complete edited collider record.");
			UndoRedoSystem::Instance().Undo();
			queue.Apply(objects, "collider-shape-undo");
			std::vector<Scene::TransactionResult> undoReceipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(undoReceipts.size() == 1U
				&& UndoRedoSystem::Instance().CompleteDeferredReplay(undoReceipts.front()),
				"Undo must finalize after its protected collider transaction commits.");
			expectCollider({0.0F, 1.0F, 2.0F}, {0.5F, 0.75F, 1.0F},
				"Undo must restore the exact authored collider identity, center, and dimensions.");
			UndoRedoSystem::Instance().Redo();
			queue.Apply(objects, "collider-shape-redo");
			std::vector<Scene::TransactionResult> redoReceipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(redoReceipts.size() == 1U
				&& UndoRedoSystem::Instance().CompleteDeferredReplay(redoReceipts.front()),
				"Redo must finalize after its protected collider transaction commits.");
			expectCollider({3.0F, 4.0F, 5.0F}, {2.0F, 2.5F, 3.0F},
				"Redo must reapply the exact collider preview values.");

			UndoRedoSystem::Instance().Clear();
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.PhysicsInspector.ColliderUndoRedoRestoresSavedRevisionDirtyState", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ScriptingSystem::Instance().SetRunning(false);
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = Scene::SceneMutationQueue::Instance();
			SceneManager& scenes = SceneManager::Instance();
			objects.clear();
			queue.Clear();
			UndoRedoSystem::Instance().Clear();

			Object object = objects.createObject("Collider saved revision fixture");
			ColliderSetComponent* colliderSet = objects.AddComponent<ColliderSetComponent>(object);
			colliderSet->Add().localCenter = {0.0F, 0.0F, 0.0F};
			std::vector<TerraEngine::Physics::ColliderRecord> edited = colliderSet->colliders;
			edited.front().localCenter = {4.0F, 5.0F, 6.0F};

			Scene::TransactionalMutationBuilder builder(
				"collider-saved-revision", Scene::TransactionOrigin::Editor, "InspectorPhysicsMultiEdit");
			builder.ObserveScene(objects)
				.ObserveObject(objects, objects.getUUID(object))
				.SetUndoGroup("Edit Collider Center")
				.UpdateComponentProperty(objects.getUUID(object), "ColliderSetComponent", "colliders",
					TerraEngine::Reflection::Variant{edited});
			const Scene::Transaction transaction = builder.Build();
			queue.EnqueueTransaction(transaction);
			(void)scenes.ApplySceneMutations("collider-saved-revision-edit");
			std::vector<Scene::TransactionResult> receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1U
				&& UndoRedoSystem::Instance().RecordCommittedTransaction(transaction, receipts.front()),
				"The committed ColliderSet edit must enter history before saved-state checks.");
			context.Expect(scenes.IsActiveSceneDirty(), "A collider edit after the initial save point must be dirty.");

			auto completeReplay = [&](const char* flushName) {
				(void)scenes.ApplySceneMutations(flushName);
				std::vector<Scene::TransactionResult> replayReceipts = queue.DrainCompletedEditorTransactionResults();
				context.Expect(replayReceipts.size() == 1U
					&& UndoRedoSystem::Instance().CompleteDeferredReplay(replayReceipts.front()),
					"The protected collider replay receipt must finalize the history cursor.");
			};

			UndoRedoSystem::Instance().Undo();
			completeReplay("collider-saved-revision-undo-initial");
			context.Expect(!scenes.IsActiveSceneDirty(),
				"Undoing a ColliderSet edit back to the initial saved revision must clear dirty state.");
			UndoRedoSystem::Instance().Redo();
			completeReplay("collider-saved-revision-redo-edited");
			context.Expect(scenes.IsActiveSceneDirty(),
				"Redoing away from the initial saved revision must restore dirty state.");

			UndoRedoSystem::Instance().SetSavePoint();
			context.Expect(!scenes.IsActiveSceneDirty(), "Saving the edited collider revision must clear dirty state.");
			UndoRedoSystem::Instance().Undo();
			completeReplay("collider-saved-revision-undo-before-save");
			context.Expect(scenes.IsActiveSceneDirty(),
				"Undoing away from the saved collider revision must mark the scene dirty.");
			UndoRedoSystem::Instance().Redo();
			completeReplay("collider-saved-revision-redo-to-save");
			context.Expect(!scenes.IsActiveSceneDirty(),
				"Redoing to the exact saved ColliderSet revision must clear dirty state.");

			UndoRedoSystem::Instance().Clear();
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.PhysicsInspector.UnitsRangesBoundaryAndStableSelection", [](TestContext& context) {
			RigidBodyComponent body;
			body.mass = 0.0F;
			context.Expect(!body.Validate().accepted, "Mass controls labelled in kilograms must reject zero mass.");
			CharacterControllerComponent character;
			character.maximumSlopeAngleDegrees = 90.0F;
			context.Expect(!character.Validate().accepted, "Slope controls labelled in degrees must reject the singular 90-degree boundary.");

			TerraEngine::Editor::PhysicsRebuildIntentQueue rebuildIntents;
			const UUID firstId = GenerateUUID();
			const UUID secondId = GenerateUUID();
			rebuildIntents.Queue({firstId, secondId, firstId}, "test edit");
			const auto intents = rebuildIntents.Drain();
			context.ExpectEqual(static_cast<std::size_t>(2), intents.size(), "The protected boundary must receive one generation-safe intent per changed UUID.");
			context.Expect(rebuildIntents.Drain().empty(), "Draining the boundary must consume the complete pending intent batch.");

			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object first = objects.createObject("First selected");
			Object second = objects.createObject("Second selected");
			const TerraEngine::Editor::StableEditorSelection selection = TerraEngine::Editor::BuildStableEditorSelection({first.EntityId(), second.EntityId(), first.EntityId()});
			context.ExpectEqual(static_cast<std::size_t>(2), selection.objectIds.size(), "Multi-selection must retain both stable object UUIDs without duplicate transient handles.");
			context.Expect(selection.primaryEntity == second.EntityId(), "The last distinct selected entity must remain the Inspector focus.");
			objects.clear();
		});
	}
}
