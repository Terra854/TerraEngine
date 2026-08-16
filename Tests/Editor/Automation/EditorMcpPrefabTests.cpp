#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "AssetDatabase.h"
#include "EditorContext.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/AnimatorComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"
#include "UndoRedoSystem.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class PrefabAutomationFixture
		{
		public:
			explicit PrefabAutomationFixture(std::string name)
				: workspace(std::filesystem::temp_directory_path() / "TerraEngineMcpPrefabs" / std::move(name)),
				projectRoot(workspace / "Project")
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
				std::filesystem::create_directories(workspace);
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				// The prefab fixture serializes the real authored animation
				// component so the atomic policy override is verified after the
				// protected ObjectManager publication.
				AnimatorComponent::registerType();
				context.projectManager = std::make_shared<ProjectManager>();
				service.SetEditorContext(&context);
				// Project selection stays editor-owned; automation starts with the
				// native workspace already active.
				const bool project = context.projectManager->CreateProject(projectRoot, "PrefabProject", ProjectTemplate::Blank);
				const nlohmann::json scene = service.HandleTool(
					"scene.create",
					{{"path", "Assets/Scenes/PrefabTests.tescene"}, {"name", "PrefabTests"}, {"open", true}},
					WriteMode::WritesEnabled,
					"prefab-scene-create");
				ready = project && scene.value("ok", false);
			}

			~PrefabAutomationFixture()
			{
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				if (assetDatabaseEnabled)
				{
					AssetDatabase::Instance().Shutdown();
				}
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
			}

			void EnableAssetDatabase()
			{
				// Existing prefabs are discovered through the same project index used by
				// the installed editor. This deliberately creates no prefab through MCP,
				// reproducing a generated or previously-authored asset after startup.
				context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
				context.assetDatabase->Initialize(projectRoot);
				assetDatabaseEnabled = true;
			}

			std::string WriteExistingPrefab(const std::filesystem::path& projectRelativePath, std::string_view name)
			{
				const std::filesystem::path absolutePath = projectRoot / projectRelativePath;
				std::filesystem::create_directories(absolutePath.parent_path());
				const TerraEngine::UUID rootId = TerraEngine::GenerateUUID();
				const TerraEngine::UUID childId = TerraEngine::GenerateUUID();
				std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
				output << nlohmann::json{
					{"format", "TerraPrefabUnboundedV1"},
					{"sourceId", TerraEngine::ToHexString(rootId)},
					{"name", std::string(name)},
					{"objects", nlohmann::json::array({{
						{"name", std::string(name)},
						{"uuid", TerraEngine::ToHexString(rootId)},
						{"components", {
							{"AnimatorComponent", {
								{"active", true},
								{"requestedMode", 0}
							}},
							{"Transform", {
							{"WorldPose", {{"position", {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {3.0f, 4.0f, 5.0f}}}}, {"rotation", {1.0f, 0.0f, 0.0f, 0.0f}}, {"scale", {1.0f, 2.0f, 1.0f}}}},
							{"ParentRelativePose", {{"translationMetres", {3.0f, 4.0f, 5.0f}}, {"rotation", {1.0f, 0.0f, 0.0f, 0.0f}}, {"scale", {1.0f, 2.0f, 1.0f}}, {"transformSpaceId", 0u}}}
						}}}},
						{"children", nlohmann::json::array({{
							{"name", "Imported Hero Mesh"},
							{"uuid", TerraEngine::ToHexString(childId)},
							{"components", {{"Transform", {
								{"WorldPose", {{"position", {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {0.0f, 2.0f, 0.0f}}}}, {"rotation", {1.0f, 0.0f, 0.0f, 0.0f}}, {"scale", {0.5f, 0.5f, 0.5f}}}},
								{"ParentRelativePose", {{"translationMetres", {0.0f, 2.0f, 0.0f}}, {"rotation", {1.0f, 0.0f, 0.0f, 0.0f}}, {"scale", {0.5f, 0.5f, 0.5f}}, {"transformSpaceId", 0u}}}
							}}}}
						}})}
					}})}
				}.dump(2);
				output.close();
				context.assetDatabase->ScanProject();
				for (const auto& [registeredPath, metadata] : context.assetDatabase->GetAssets())
				{
					if (registeredPath.lexically_normal() == std::filesystem::absolute(absolutePath).lexically_normal())
					{
						return TerraEngine::ToHexString(metadata.guid);
					}
				}
				return {};
			}

			nlohmann::json CreateObject(std::string_view name, std::string_view requestId)
			{
				return service.HandleTool("object.create", {{"name", std::string(name)}}, WriteMode::WritesEnabled, std::string(requestId));
			}

			std::string ObjectId(const nlohmann::json& created) const
			{
				return created.value("data", nlohmann::json::object()).value("object", nlohmann::json::object()).value("uuid", std::string());
			}

			std::filesystem::path workspace;
			std::filesystem::path projectRoot;
			EditorContext context;
			EditorAutomationService service;
			bool ready = false;
			bool assetDatabaseEnabled = false;
		};
	}

	void RegisterEditorMcpPrefabTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Prefab.InstantiatesRegisteredProjectAssetAfterServiceStartup", [](TestContext& context) {
			PrefabAutomationFixture fixture("ExistingProjectAsset");
			context.Expect(fixture.ready, "Existing-prefab fixture should create and open a project scene.");
			fixture.EnableAssetDatabase();
			const std::string registeredGuid = fixture.WriteExistingPrefab(
				"Assets/Models/Hero_Generated/Hero.teprefab", "Imported Hero");
			context.Expect(!registeredGuid.empty(), "The existing prefab should be registered in the project asset index.");

			const nlohmann::json instantiated = fixture.service.HandleTool(
				"prefab.instantiate",
				{{"prefab", "Assets/Models/Hero_Generated/Hero.teprefab"}, {"name", "Hero Instance"}},
				WriteMode::WritesEnabled,
				"prefab-existing-instantiate");
			context.Expect(instantiated.value("ok", false),
				"A registered project prefab should instantiate without being created during the current automation session. Response: " + instantiated.dump());
			context.ExpectEqual(registeredGuid,
				instantiated.value("data", nlohmann::json::object()).value("prefabId", std::string()),
				"Existing prefab resolution should expose the stable AssetDatabase GUID as its source identity.");
			context.ExpectEqual(static_cast<std::uint64_t>(1u),
				instantiated.value("data", nlohmann::json::object()).value("prefabGeneration", 0ull),
				"A loose registered prefab should begin at the service's first stable generation.");
			context.Expect(!instantiated.value("data", nlohmann::json::object()).value("instanceId", std::string()).empty(),
				"Existing prefab instantiation should publish a stable scene-object identity.");

			const std::optional<TerraEngine::UUID> instanceId = TerraEngine::ParseUUID(
				instantiated.value("data", nlohmann::json::object()).value("instanceId", std::string()));
			const std::optional<Object> root = instanceId ? ObjectManager::instance().getObject(*instanceId) : std::nullopt;
			context.Expect(root.has_value(), "The returned instance ID should identify the deserialized prefab root.");
			if (!root) return;
			context.ExpectEqual(std::string("Hero Instance"), ObjectManager::instance().getObjectName(*root),
				"The requested name should be applied to the canonical prefab root.");
			const Transform* rootTransform = ObjectManager::instance().GetComponent<Transform>(*root);
			context.Expect(rootTransform != nullptr && rootTransform->worldPose.position.offsetMetres == glm::dvec3(3.0, 4.0, 5.0)
				&& rootTransform->worldPose.scale == glm::vec3(1.0f, 2.0f, 1.0f),
				"Registered prefab instantiation should restore authored root component values.");
			const std::vector<Object> children = ObjectManager::instance().getChildren(*root);
			context.ExpectEqual(static_cast<std::size_t>(1U), children.size(),
				"Registered prefab instantiation should restore the authored child hierarchy.");
			if (!children.empty())
			{
				context.ExpectEqual(std::string("Imported Hero Mesh"), ObjectManager::instance().getObjectName(children.front()),
					"Child names should come from the prefab document rather than a generated empty root.");
				const Transform* childTransform = ObjectManager::instance().GetComponent<Transform>(children.front());
				context.Expect(childTransform != nullptr && childTransform->worldPose.position.offsetMetres == glm::dvec3(0.0, 2.0, 0.0),
					"Child components should be deserialized with their authored values.");
			}
			const PrefabLinkComponent* link = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*root);
			context.Expect(link != nullptr && link->prefabSourceId != TerraEngine::EMPTY_UUID
				&& link->sourceObjectId != TerraEngine::EMPTY_UUID,
				"The complete graph root should retain prefab source and source-object link identities.");

			UndoRedoSystem::Instance().Undo();
			context.Expect(!ObjectManager::instance().getObject(*instanceId).has_value(),
				"One undo should remove the complete prefab graph as a single history action.");
			UndoRedoSystem::Instance().Redo();
			const std::optional<Object> restoredRoot = ObjectManager::instance().getObject(*instanceId);
			context.Expect(restoredRoot.has_value(), "Redo should restore the prefab's stable canonical root identity.");
			if (restoredRoot)
			{
				context.ExpectEqual(static_cast<std::size_t>(1U), ObjectManager::instance().getChildren(*restoredRoot).size(),
					"Redo should restore the complete child graph rather than only the root.");
			}
		});

		registry.Add("Editor.MCP.Prefab.InstantiateManyPublishesOneDeterministicGeneration", [](TestContext& context) {
			PrefabAutomationFixture fixture("InstantiateManyGrid");
			context.Expect(fixture.ready, "Bulk-prefab fixture should create and open a project scene.");
			fixture.EnableAssetDatabase();
			const std::string registeredGuid = fixture.WriteExistingPrefab(
				"Assets/Models/Crowd_Generated/Crowd.teprefab", "Crowd Character");
			context.Expect(!registeredGuid.empty(), "The crowd prefab should be registered before bulk publication.");

			ObjectManager& objects = ObjectManager::instance();
			const std::uint64_t hierarchyBefore = objects.GetHierarchyRevision();
			const std::array<TerraEngine::UUID, 2> clipIds = {
				TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID()
			};
			const nlohmann::json result = fixture.service.HandleTool(
				"prefab.instantiate_many",
				{{"prefab", registeredGuid}, {"count", 4u}, {"namePrefix", "Gate Character"},
					{"spacing", 2.0f}, {"requestedAnimationMode", 3},
					{"defaultClipAssetIds", {
						TerraEngine::ToHexString(clipIds[0]), TerraEngine::ToHexString(clipIds[1])}},
					{"defaultClipStartFrameStep", 7u}},
				WriteMode::WritesEnabled,
				"prefab-instantiate-many-grid");
			context.Expect(result.value("ok", false), "A registered prefab grid should publish atomically. Response: " + result.dump());
			const nlohmann::json data = result.value("data", nlohmann::json::object());
			const nlohmann::json rows = data.value("instances", nlohmann::json::array());
			context.ExpectEqual(static_cast<std::size_t>(4u), rows.size(),
				"Bulk instantiation should return one typed identity row per requested instance.");
			context.ExpectEqual(static_cast<std::uint64_t>(hierarchyBefore + 1u), objects.GetHierarchyRevision(),
				"Every object in the grid should become visible through one bulk-publication generation.");
			context.ExpectEqual(std::string("prefab_bulk_publication"),
				data.value("visibilityBoundary", std::string()),
				"The result should name the protected visibility boundary.");
			context.ExpectEqual(static_cast<std::size_t>(4u),
				data.value("animationModeOverrideCount", static_cast<std::size_t>(0u)),
				"Every cloned AnimatorComponent should receive the policy before publication.");
			context.ExpectEqual(static_cast<std::size_t>(4u),
				data.value("animationDefaultsOverrideCount", static_cast<std::size_t>(0u)),
				"Every cloned AnimatorComponent should receive clip and phase defaults before publication: "
					+ data.dump());

			const std::array<glm::vec3, 4> expectedPositions = {
				glm::vec3(-1.0f, 0.0f, -1.0f),
				glm::vec3(1.0f, 0.0f, -1.0f),
				glm::vec3(-1.0f, 0.0f, 1.0f),
				glm::vec3(1.0f, 0.0f, 1.0f)
			};
			std::unordered_set<std::string> returnedIds;
			for (std::size_t index = 0u; index < rows.size(); ++index)
			{
				const std::string instanceId = rows[index].value("instanceId", std::string());
				context.Expect(!instanceId.empty() && returnedIds.insert(instanceId).second,
					"Every bulk-created canonical root should have a distinct stable identity.");
				const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(instanceId);
				const std::optional<Object> root = parsed ? objects.getObject(*parsed) : std::nullopt;
				context.Expect(root.has_value(), "Returned bulk identities should resolve immediately after the boundary.");
				if (!root) continue;
				const Transform* transform = objects.GetComponent<Transform>(*root);
				context.Expect(transform != nullptr && transform->worldPose.position.offsetMetres == glm::dvec3(expectedPositions[index]),
					"Bulk grid placement should be deterministic and centered.");
				const AnimatorComponent* animator = objects.GetComponent<AnimatorComponent>(*root);
				context.Expect(
					animator != nullptr
						&& animator->requestedMode
							== TerraEngine::Animation::AnimationExecutionPolicy::
								ForceCpuDeterministicFallback,
					"The requested animation policy should be visible in the same bulk generation.");
				context.Expect(animator != nullptr
					&& animator->defaultClipAssetId == clipIds[index % clipIds.size()]
					&& animator->defaultClipStartFrame == index * 7u,
					"Round-robin clips and deterministic phase offsets should share the bulk visibility boundary.");
				context.ExpectEqual("Gate Character " + std::to_string(index + 1u),
					objects.getObjectName(*root),
					"Bulk root names should be deterministic and one-based.");
			}

			UndoRedoSystem::Instance().Undo();
			for (const std::string& instanceId : returnedIds)
			{
				const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(instanceId);
				context.Expect(parsed && !objects.getObject(*parsed).has_value(),
					"One history action should remove the complete bulk prefab publication.");
			}
		});

		registry.Add("Editor.MCP.Prefab.InstantiateManyRejectsAtomicallyAndDeclaresBoundary", [](TestContext& context) {
			PrefabAutomationFixture fixture("InstantiateManyRollback");
			context.Expect(fixture.ready, "Bulk rollback fixture should create and open a project scene.");
			fixture.EnableAssetDatabase();
			const std::string registeredGuid = fixture.WriteExistingPrefab(
				"Assets/Models/Rollback_Generated/Rollback.teprefab", "Rollback Character");
			context.Expect(!registeredGuid.empty(), "The rollback prefab should be registered.");

			const auto capability = fixture.service.GetRegistry().Find(
				"prefab.instantiate_many",
				1u,
				fixture.service.BuildAutomationStateSnapshot());
			context.Expect(capability.has_value(), "Bulk prefab automation should be discoverable in an active scene.");
			if (capability)
			{
				context.Expect(capability->descriptor.execution.workerPreparation,
					"Immutable bulk request parsing should be declared worker-preparable.");
				context.Expect(capability->descriptor.execution.protectedCommit,
					"Live prefab publication should remain a protected commit.");
				context.ExpectEqual(std::string("prefab_bulk_publication"),
					capability->descriptor.execution.visibilityBoundary,
					"The capability should declare its exact visibility boundary.");
				context.Expect(std::find(
					capability->descriptor.execution.commitWrites.begin(),
					capability->descriptor.execution.commitWrites.end(),
					"ObjectStorage") != capability->descriptor.execution.commitWrites.end(),
					"The protected commit should declare live object-storage writes.");
			}

			ObjectManager& objects = ObjectManager::instance();
			const std::size_t rootsBefore = objects.getRootObjects().size();
			const std::uint64_t hierarchyBefore = objects.GetHierarchyRevision();
			objects.BeginBulkPublication({0u, 0u});
			const nlohmann::json rejected = fixture.service.HandleTool(
				"prefab.instantiate_many",
				{{"prefab", registeredGuid}, {"count", 8u}, {"namePrefix", "Rejected"},
					{"spacing", 1.0f}},
				WriteMode::WritesEnabled,
				"prefab-instantiate-many-conflict");
			objects.FinishBulkPublication(false);

			context.Expect(!rejected.value("ok", true),
				"A concurrent protected publication should reject the entire prefab batch.");
			context.ExpectEqual(std::string("conflict"),
				rejected.value("error", nlohmann::json::object()).value("category", std::string()),
				"Concurrent publication rejection should be classified as a conflict.");
			context.Expect(rejected.value("data", nlohmann::json::object()).value("rolledBack", false),
				"Rejected bulk operations should explicitly report atomic rollback.");
			context.ExpectEqual(rootsBefore, objects.getRootObjects().size(),
				"Rejected bulk publication must not leak any roots.");
			context.ExpectEqual(hierarchyBefore, objects.GetHierarchyRevision(),
				"A rejected publication must not advance the visible hierarchy generation.");
		});

		registry.Add("Editor.MCP.Prefab.CreateAndInstantiatePreserveStableIdentity", [](TestContext& context) {
			PrefabAutomationFixture fixture("CreateInstantiate");
			context.Expect(fixture.ready, "Prefab fixture should create and open a project scene.");
			const nlohmann::json source = fixture.CreateObject("Reusable Crate", "prefab-source-create");
			context.Expect(source.value("ok", false), "Prefab source object should exist.");
			const std::string sourceId = fixture.ObjectId(source);

			const nlohmann::json created = fixture.service.HandleTool(
				"prefab.create",
				{{"object", sourceId}, {"path", "Assets/Prefabs/ReusableCrate.teprefab"}},
				WriteMode::WritesEnabled,
				"prefab-create");
			context.Expect(created.value("ok", false), "Creating a prefab from a scene object should succeed atomically.");
			const std::string prefabId = created["data"].value("prefabId", std::string());
			context.Expect(!prefabId.empty(), "Prefab creation should return stable source identity.");
			context.ExpectEqual(sourceId, created["data"].value("sourceObjectId", std::string()), "Prefab result should identify the authored source object.");

			const nlohmann::json first = fixture.service.HandleTool(
				"prefab.instantiate",
				{{"prefab", prefabId}, {"name", "Crate A"}},
				WriteMode::WritesEnabled,
				"prefab-instantiate-first");
			const nlohmann::json second = fixture.service.HandleTool(
				"prefab.instantiate",
				{{"prefab", "Assets/Prefabs/ReusableCrate.teprefab"}, {"name", "Crate B"}},
				WriteMode::WritesEnabled,
				"prefab-instantiate-second");
			context.Expect(first.value("ok", false) && second.value("ok", false), "A prefab should instantiate by stable ID or project-relative path.");
			context.ExpectEqual(prefabId, first["data"].value("prefabId", std::string()), "Instance should retain source prefab identity.");
			context.ExpectEqual(prefabId, second["data"].value("prefabId", std::string()), "Path lookup should resolve the same prefab identity.");
			context.Expect(first["data"].value("instanceId", std::string()) != second["data"].value("instanceId", std::string()), "Each instance requires its own stable identity.");
		});

		registry.Add("Editor.MCP.Prefab.RejectsCyclicInstantiationWithoutPartialState", [](TestContext& context) {
			PrefabAutomationFixture fixture("CycleRejection");
			context.Expect(fixture.ready, "Prefab cycle fixture should create and open a project scene.");
			const nlohmann::json source = fixture.CreateObject("Cycle Root", "prefab-cycle-source");
			const nlohmann::json created = fixture.service.HandleTool(
				"prefab.create",
				{{"object", fixture.ObjectId(source)}, {"path", "Assets/Prefabs/Cycle.teprefab"}},
				WriteMode::WritesEnabled,
				"prefab-cycle-create");
			context.Expect(created.value("ok", false), "Cycle fixture should create its prefab.");
			const std::string prefabId = created["data"].value("prefabId", std::string());

			const nlohmann::json opened = fixture.service.HandleTool(
				"prefab.edit.open",
				{{"prefab", prefabId}, {"dirtyPolicy", "reject"}},
				WriteMode::WritesEnabled,
				"prefab-cycle-open");
			context.Expect(opened.value("ok", false), "Prefab editing context should open by stable identity.");
			const nlohmann::json cycle = fixture.service.HandleTool(
				"prefab.instantiate",
				{{"prefab", prefabId}},
				WriteMode::WritesEnabled,
				"prefab-cycle-instantiate");
			context.Expect(!cycle.value("ok", true), "A prefab cannot instantiate itself into its own editing context.");
			context.ExpectEqual(std::string("conflict"), cycle["error"].value("category", std::string()), "Cyclic prefab graphs should return a conflict.");
			context.Expect(cycle.value("data", nlohmann::json::object()).value("rolledBack", false), "Cycle rejection should explicitly confirm zero partial publication.");
		});

		registry.Add("Editor.MCP.Prefab.OverrideInspectApplyAndRevertRoundTrip", [](TestContext& context) {
			PrefabAutomationFixture fixture("Overrides");
			context.Expect(fixture.ready, "Prefab override fixture should create and open a project scene.");
			const nlohmann::json source = fixture.CreateObject("Override Source", "prefab-override-source");
			const nlohmann::json created = fixture.service.HandleTool(
				"prefab.create",
				{{"object", fixture.ObjectId(source)}, {"path", "Assets/Prefabs/OverrideSource.teprefab"}},
				WriteMode::WritesEnabled,
				"prefab-override-create");
			context.Expect(created.value("ok", false), "Override fixture should create its prefab.");
			const nlohmann::json instantiated = fixture.service.HandleTool(
				"prefab.instantiate",
				{{"prefab", created["data"].value("prefabId", std::string())}, {"name", "Override Instance"}},
				WriteMode::WritesEnabled,
				"prefab-override-instantiate");
			context.Expect(instantiated.value("ok", false), "Override fixture should create an instance.");
			const std::string instanceId = instantiated["data"].value("instanceId", std::string());

			const nlohmann::json renamed = fixture.service.HandleTool(
				"object.set_metadata",
				{{"object", instanceId}, {"name", "Locally Overridden"}},
				WriteMode::WritesEnabled,
				"prefab-override-metadata");
			context.Expect(renamed.value("ok", false), "Fixture should author an instance override through the normal object tool.");
			const nlohmann::json inspected = fixture.service.HandleTool(
				"prefab.overrides.inspect",
				{{"instance", instanceId}},
				WriteMode::ReadOnly,
				"prefab-overrides-inspect");
			context.Expect(inspected.value("ok", false), "Prefab override inspection should succeed.");
			context.Expect(!inspected["data"].value("overrides", nlohmann::json::array()).empty(), "Authored instance differences should appear as structured overrides.");

			const nlohmann::json applied = fixture.service.HandleTool(
				"prefab.overrides.apply",
				{{"instance", instanceId}, {"expectedPrefabGeneration", inspected["data"].value("prefabGeneration", 0u)}},
				WriteMode::WritesEnabled,
				"prefab-overrides-apply");
			context.Expect(applied.value("ok", false), "Applying overrides should update the source as one history action.");
			context.Expect(applied["data"].value("historyId", std::string()).size() > 0, "Override application should return its composite history identity.");

			const nlohmann::json reverted = fixture.service.HandleTool(
				"prefab.overrides.revert",
				{{"instance", instanceId}},
				WriteMode::WritesEnabled,
				"prefab-overrides-revert");
			context.Expect(reverted.value("ok", false), "Reverting overrides should restore the current prefab source state.");
			context.ExpectEqual(static_cast<std::size_t>(0), reverted["data"].value("remainingOverrideCount", static_cast<std::size_t>(1)), "A complete revert should leave no overrides.");
		});

		registry.Add("Editor.MCP.Prefab.EditContextHasExplicitDirtyAndIdentityPreconditions", [](TestContext& context) {
			PrefabAutomationFixture fixture("EditContext");
			context.Expect(fixture.ready, "Prefab edit-context fixture should create and open a project scene.");
			const nlohmann::json firstSource = fixture.CreateObject("First Prefab", "prefab-context-first-source");
			const nlohmann::json secondSource = fixture.CreateObject("Second Prefab", "prefab-context-second-source");
			const nlohmann::json first = fixture.service.HandleTool("prefab.create", {{"object", fixture.ObjectId(firstSource)}, {"path", "Assets/Prefabs/First.teprefab"}}, WriteMode::WritesEnabled, "prefab-context-first-create");
			const nlohmann::json second = fixture.service.HandleTool("prefab.create", {{"object", fixture.ObjectId(secondSource)}, {"path", "Assets/Prefabs/Second.teprefab"}}, WriteMode::WritesEnabled, "prefab-context-second-create");
			context.Expect(first.value("ok", false) && second.value("ok", false), "Edit-context fixture should create both prefabs.");

			const nlohmann::json opened = fixture.service.HandleTool("prefab.edit.open", {{"prefab", first["data"].value("prefabId", std::string())}, {"dirtyPolicy", "reject"}}, WriteMode::WritesEnabled, "prefab-context-open");
			context.Expect(opened.value("ok", false), "Opening a prefab context should return its context identity.");
			const std::string contextId = opened["data"].value("contextId", std::string());
			context.Expect(!contextId.empty(), "Prefab edit contexts require stable identities for stale-close protection.");

			const nlohmann::json collision = fixture.service.HandleTool("prefab.edit.open", {{"prefab", second["data"].value("prefabId", std::string())}, {"dirtyPolicy", "reject"}}, WriteMode::WritesEnabled, "prefab-context-collision");
			context.Expect(!collision.value("ok", true), "Opening another prefab must not silently discard or replace an active context.");
			context.ExpectEqual(std::string("conflict"), collision["error"].value("category", std::string()), "Active edit-context collision should be a conflict.");

			const nlohmann::json staleClose = fixture.service.HandleTool("prefab.edit.close", {{"contextId", "stale-context"}, {"dirtyPolicy", "reject"}}, WriteMode::WritesEnabled, "prefab-context-stale-close");
			context.Expect(!staleClose.value("ok", true), "A stale context ID must not close the current prefab.");
			const nlohmann::json closed = fixture.service.HandleTool("prefab.edit.close", {{"contextId", contextId}, {"dirtyPolicy", "discard"}}, WriteMode::WritesEnabled, "prefab-context-close");
			context.Expect(closed.value("ok", false), "An explicit dirty policy and matching context ID should close prefab editing.");
		});
	}
}
