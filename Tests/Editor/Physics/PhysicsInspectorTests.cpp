// Editor physics multi-edit, boundary, and undo regression tests.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
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
			context.ExpectEqual(1.0F, firstBody->mass, "Grouped undo must restore the first mixed value.");
			context.ExpectEqual(2.0F, secondBody->mass, "Grouped undo must restore the second mixed value.");
			UndoRedoSystem::Instance().Redo();
			queue.Apply(objects, "physics-multi-redo");
			context.ExpectEqual(7.0F, firstBody->mass, "Grouped redo must reapply the first object.");
			context.ExpectEqual(7.0F, secondBody->mass, "Grouped redo must reapply the second object.");
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
