#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
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
		};
	}

	void RegisterEditorMcpPrefabTests(TestRegistry& registry)
	{
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
