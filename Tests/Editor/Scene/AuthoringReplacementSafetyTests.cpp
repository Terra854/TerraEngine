// AuthoringReplacementSafetyTests.cpp -- Actual reset entry point and import-close policy.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.h"
#include "Editor/TerraEngine-Editor/Subsystems/EditorClosePolicy.h"
#include "Engine/Components/CharacterLocomotionComponent.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct ReplacementFixture
		{
			std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraPrefabResetReview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::path previousRoot = SceneManager::Instance().GetSceneRoot();
			MainEditorSubsystem editor;
			ObjectManager& objects = ObjectManager::instance();
			Object parent, root, child;
			std::filesystem::path prefab;
			TerraEngine::UUID sourceId = TerraEngine::GenerateUUID();

			// Initialization is separate so cleanup also runs if a prerequisite fails.
			void Initialize(TestContext& context)
			{
				ComponentRegistry::RegisterAllComponents();
				SceneManager::Instance().BeginLoadSession();
				objects.clear();
				std::filesystem::create_directories(workspace);
				SceneManager::Instance().SetSceneRoot(workspace);
				const auto scene = workspace / "Reset.tescene";
				{ std::ofstream output(scene); output << nlohmann::json{{"format", "TerraSceneUnboundedV1"}, {"name", "Reset"}, {"objects", nlohmann::json::array()}}; }
				const auto sceneId = SceneManager::Instance().LoadScene(scene);
				context.Expect(sceneId != TerraEngine::EMPTY_UUID && SceneManager::Instance().SetActiveScene(sceneId),
					"The reset fixture needs a real active scene for dirty-state observation.");
				parent = objects.createObject("Preserved parent");
				root = objects.createObject("Authored instance");
				child = objects.createObject("Authored child");
				context.Expect(objects.setParent(root, parent) && objects.setParent(child, root),
					"The production reset must exercise a real nested instance with an external parent.");
				prefab = workspace / "Source.teprefab";
				auto* link = objects.AddComponent<PrefabLinkComponent>(root);
				context.Expect(link != nullptr, "The instance needs its production prefab metadata component.");
				link->prefabSourceId = sourceId;
				link->sourceObjectId = TerraEngine::GenerateUUID();
				link->prefabName = "Original source";
				link->prefabPath = prefab.generic_string();
				link->overriddenProperties = "name";
				editor.SetSelectedEntity(root.EntityId());
				SceneManager::Instance().ClearActiveSceneDirty();
			}

			void WriteSource(const nlohmann::json& object)
			{
				std::ofstream output(prefab);
				output << nlohmann::json{{"format", "TerraPrefabUnboundedV1"}, {"objects", nlohmann::json::array({object})}};
				if(!output) throw TestFailure("The reset fixture could not persist its source document.");
			}

			~ReplacementFixture()
			{
				editor.SetSelectedEntity(std::nullopt);
				SceneManager::Instance().UnloadAll();
				objects.clear();
				SceneManager::Instance().SetSceneRoot(previousRoot);
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
			}
		};
	}

	void RegisterAuthoringReplacementSafetyTests(TestRegistry& registry)
	{
		registry.Add("Editor.Authoring.PendingPrefabResetRejectsEditsAfterAdmission", [](TestContext& context) {
			// Worker preparation cannot replace the original observation with a
			// fresh one at completion, or it would overwrite edits made meanwhile.
			auto& pool = ThreadPool::instance();
			const bool ownsPool = pool.getThreadCount() == 0;
			if(ownsPool) pool.initialize(2);
			struct PoolScope { bool owns; ~PoolScope() { if(owns) ThreadPool::instance().shutdown(); } } poolScope{ownsPool};
			ReplacementFixture fixture; fixture.Initialize(context);
			fixture.WriteSource(Serializer::SerializeObject(fixture.root));
			context.Expect(fixture.editor.RequestPrefabInstanceReset(fixture.root.EntityId()), "Interactive reset must admit detached preparation without waiting.");
			fixture.objects.setObjectName(fixture.root, "User edited while pending");
			const auto original = Serializer::SerializeObject(fixture.parent);
			const auto dirtyBeforeCompletion = SceneManager::Instance().IsActiveSceneDirty();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while(fixture.editor.IsPrefabInstanceResetPending() && std::chrono::steady_clock::now() < deadline) {
				fixture.editor.PollPrefabInstanceReset();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			context.Expect(!fixture.editor.IsPrefabInstanceResetPending() && Serializer::SerializeObject(fixture.parent) == original
				&& SceneManager::Instance().IsActiveSceneDirty() == dirtyBeforeCompletion, "Stale worker output must reject atomically without replacing the edited branch or dirty state.");
			context.Expect(fixture.editor.GetLastPrefabInstanceResetResult() == false,
				"A normal transaction rejection must remain observable after the pending request clears.");
			const auto shallow = Serializer::SerializeObjectShallow(fixture.parent.EntityId());
			context.Expect(!shallow.contains("children") && shallow["uuid"] == TerraEngine::ToHexString(fixture.objects.getUUID(fixture.parent)),
				"Reference scans must snapshot one object without serializing descendants.");
		});
		registry.Add("Editor.Authoring.PrefabResetDecodeFailurePreservesOriginalGraph", [](TestContext& context) {
			ReplacementFixture fixture;
			fixture.Initialize(context);
			const auto rootId = fixture.objects.getUUID(fixture.root);
			const auto childId = fixture.objects.getUUID(fixture.child);
			const auto parentBefore = Serializer::SerializeObject(fixture.parent);
			const auto selectionBefore = fixture.editor.GetSelectedEntityUuids();
			const auto generationBefore = fixture.objects.GetSceneGeneration();
			const auto rootLifetime = fixture.objects.getObjectLifetimeGeneration(fixture.root);
			const auto childLifetime = fixture.objects.getObjectLifetimeGeneration(fixture.child);
			const auto dirtyBefore = SceneManager::Instance().IsActiveSceneDirty();
			auto malformed = Serializer::SerializeObject(fixture.root);
			// This passes the envelope/objects checks but fails concrete object decode
			// before a replacement can be created. Catch JSON failure as a safe result.
			malformed["name"] = 42;
			fixture.WriteSource(malformed);
			bool accepted = false;
			bool threwDecodeError = false;
			try { accepted = fixture.editor.ResetPrefabInstanceToDefaults(fixture.root.EntityId()); }
			catch(const nlohmann::json::exception&) { threwDecodeError = true; }
			const auto root = fixture.objects.getObject(rootId);
			const auto child = fixture.objects.getObject(childId);
			context.Expect(root.has_value() && child.has_value(),
				"Rejected replacement decode must preserve the original root and child identities.");
			context.Expect(!accepted && !threwDecodeError && Serializer::SerializeObject(fixture.parent) == parentBefore
				&& fixture.editor.GetSelectedEntityUuids() == selectionBefore
				&& fixture.objects.GetSceneGeneration() == generationBefore
				&& fixture.objects.getObjectLifetimeGeneration(*root) == rootLifetime
				&& fixture.objects.getObjectLifetimeGeneration(*child) == childLifetime
				&& SceneManager::Instance().IsActiveSceneDirty() == dirtyBefore,
				"Rejected reset must be normal and atomic across hierarchy, components, lifetimes, selection, and dirty state.");
		});

		registry.Add("Editor.Authoring.PrefabResetRemapsInternalIdsAndPreservesSourceProvenance", [](TestContext& context) {
			ReplacementFixture fixture;
			fixture.Initialize(context);
			const auto unrelatedRoot = fixture.objects.createObject("Unrelated source-id collision");
			const auto unrelatedChild = fixture.objects.createObject("Unrelated child-id collision");
			const auto sourceRootId = fixture.objects.getUUID(unrelatedRoot);
			const auto sourceChildId = fixture.objects.getUUID(unrelatedChild);
			const auto unrelatedRootBefore = Serializer::SerializeObject(unrelatedRoot.EntityId());
			const auto unrelatedChildBefore = Serializer::SerializeObject(unrelatedChild.EntityId());
			auto source = Serializer::SerializeObject(fixture.root);
			source["uuid"] = TerraEngine::ToHexString(sourceRootId);
			context.Expect(source["children"].is_array() && source["children"].size() == 1U,
				"The source fixture must contain one remappable child.");
			source["children"][0]["uuid"] = TerraEngine::ToHexString(sourceChildId);
			const auto nestedSource = TerraEngine::GenerateUUID(), nestedObject = TerraEngine::GenerateUUID();
			source["children"][0]["components"]["PrefabLinkComponent"] = {
				{"prefabSourceId", TerraEngine::ToHexString(nestedSource)}, {"sourceObjectId", TerraEngine::ToHexString(nestedObject)},
				{"prefabName", "Nested source"}, {"prefabPath", "Nested.teprefab"}, {"overriddenProperties", ""}};
			CharacterLocomotionComponent locomotion;
			locomotion.animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
			locomotion.animatorObjectId = sourceChildId;
			source["components"]["CharacterLocomotionComponent"] = CharacterLocomotionComponent::SerializeAuthored(locomotion);
			const auto oldRootId = fixture.objects.getUUID(fixture.root), oldChildId = fixture.objects.getUUID(fixture.child);
			// Install genuine source provenance for the surviving child.
			auto* childLink = fixture.objects.AddComponent<PrefabLinkComponent>(fixture.child);
			childLink->prefabSourceId = fixture.sourceId; childLink->sourceObjectId = sourceChildId;
			auto* external = fixture.objects.AddComponent<CharacterLocomotionComponent>(unrelatedRoot);
			external->animatorObjectId = oldChildId;
			const auto externalBefore = Serializer::SerializeObject(unrelatedRoot.EntityId());
			fixture.WriteSource(source);
			context.Expect(fixture.editor.ResetPrefabInstanceToDefaults(fixture.root.EntityId()),
				"A valid concrete replacement must publish through the production reset entry point.");
			const auto selected = fixture.editor.GetSelectedEntity();
			context.Expect(selected.has_value(), "Successful reset must select the complete replacement root.");
			const auto replacement = fixture.objects.getObject(*selected);
			context.Expect(replacement.has_value(), "The selected replacement must be a live object.");
			const auto children = fixture.objects.getChildren(*replacement);
			context.Expect(children.size() == 1U, "Successful reset must retain the source child hierarchy.");
			const auto parent = fixture.objects.getParent(*replacement);
			const auto preservedRoot = fixture.objects.getObject(sourceRootId);
			const auto preservedChild = fixture.objects.getObject(sourceChildId);
			const auto* actualLocomotion = fixture.objects.GetComponent<CharacterLocomotionComponent>(*replacement);
			const auto* link = fixture.objects.GetComponent<PrefabLinkComponent>(*replacement);
			const auto* nestedLink = fixture.objects.GetComponent<PrefabLinkComponent>(children.front());
			context.Expect(nestedLink && nestedLink->prefabSourceId == nestedSource && nestedLink->sourceObjectId == nestedObject,
				"Outer instance identity must not overwrite the nested source's authored link.");
			context.Expect(fixture.objects.getUUID(*replacement) != sourceRootId
				&& fixture.objects.getUUID(children.front()) != sourceChildId
				&& preservedRoot && preservedRoot->EntityId() == unrelatedRoot.EntityId()
				&& preservedChild && preservedChild->EntityId() == unrelatedChild.EntityId()
				&& Serializer::SerializeObject(unrelatedRoot.EntityId()) == externalBefore
				&& fixture.objects.getUUID(*replacement) == oldRootId && fixture.objects.getUUID(children.front()) == oldChildId
				&& Serializer::SerializeObject(unrelatedChild.EntityId()) == unrelatedChildBefore,
				"Replacement identities must be remapped without stealing unrelated live UUID lookups.");
			context.Expect(parent && parent->EntityId() == fixture.parent.EntityId()
				&& actualLocomotion && actualLocomotion->animatorObjectId == fixture.objects.getUUID(children.front())
				&& link && link->prefabSourceId == fixture.sourceId && link->sourceObjectId == sourceRootId
				&& link->overriddenProperties.empty() && SceneManager::Instance().IsActiveSceneDirty(),
				"Reset must remap internal references, preserve source provenance/parent, clear overrides, and mark the scene dirty.");
			// Ordinary children in the source have no link of their own. A second
			// reset must use the persisted root map rather than regenerate their UUIDs.
			context.Expect(fixture.editor.ResetPrefabInstanceToDefaults(replacement->EntityId()),
				"A second reset must retain provenance even with an external child reference.");
			context.Expect(fixture.objects.getObject(oldChildId).has_value()
				&& fixture.objects.GetComponent<CharacterLocomotionComponent>(unrelatedRoot)->animatorObjectId == oldChildId,
				"Repeated reset must preserve descendant UUIDs and external references.");
		});

		registry.Add("Editor.Authoring.PrefabResetGuardsTransformlessExternalReferences", [](TestContext& context) {
			ReplacementFixture fixture; fixture.Initialize(context);
			const auto external = fixture.objects.createObject("Reference without Transform");
			const auto childId = fixture.objects.getUUID(fixture.child);
			fixture.objects.AddComponent<CharacterLocomotionComponent>(external)->animatorObjectId = childId;
			fixture.objects.RemoveComponent<Transform>(external);
			fixture.objects.setObjectActive(external, false);
			auto source = Serializer::SerializeObject(fixture.root);
			source["children"] = nlohmann::json::array();
			fixture.WriteSource(source);
			const auto before = Serializer::SerializeObject(fixture.parent);
			context.Expect(!fixture.editor.ResetPrefabInstanceToDefaults(fixture.root.EntityId())
				&& fixture.objects.getObject(childId).has_value() && Serializer::SerializeObject(fixture.parent) == before,
				"An inactive Transform-less external reference must reject child removal without any live mutation.");
		});

		registry.Add("Editor.Authoring.ImportCloseTerrainOnlyDirtinessRequiresSaveConfirmation", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			// This is the exact policy used by Continue Closing to choose its existing
			// save-confirmation branch before ExecutePendingAction. UI delivery is separate.
			context.Expect(EvaluateImportClosePolicy(false, true, false, false) == EditorCloseDecision::SaveConfirmation,
				"Terrain-only authored changes during import must require saving before close or exit continuation.");
			context.Expect(EvaluateImportClosePolicy(false, false, false, false) == EditorCloseDecision::ExecutePendingAction
				&& EvaluateImportClosePolicy(true, false, false, false) == EditorCloseDecision::SaveConfirmation
				&& EvaluateImportClosePolicy(false, false, true, true) == EditorCloseDecision::SaveConfirmation
				&& EvaluateImportClosePolicy(false, false, false, true) == EditorCloseDecision::ExecutePendingAction,
				"Shared policy must preserve clean, scene-dirty, active-prefab, and inactive-prefab decisions.");
		});
	}
}
