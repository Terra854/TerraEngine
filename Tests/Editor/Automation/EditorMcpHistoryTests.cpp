#include "../TestHarness.h"

#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "ProjectManager.h"
#include "UndoRedoSystem.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		// History capabilities are scene scoped, so the fixture opens the same
		// minimal authored document that an MCP client would operate on.
		class HistoryProjectFixture
		{
		public:
			explicit HistoryProjectFixture(const std::string& suffix)
				: root(std::filesystem::temp_directory_path() / "TerraEngineAutomationHistory" / suffix)
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
				context.projectManager = std::make_shared<ProjectManager>();
				std::string error;
				context.projectManager->CreateProject(root, suffix, ProjectTemplate::Blank, &error);

				const std::filesystem::path scenePath = context.projectManager->GetProjectRoot() / "Assets/Scenes/History.tescene";
				std::filesystem::create_directories(scenePath.parent_path());
				std::ofstream scene(scenePath, std::ios::binary);
				scene << nlohmann::json{
					{"name", "History"},
					{"sceneSettings", nlohmann::json::object()},
					{"objects", nlohmann::json::array()}
				}.dump(2);
				scene.close();

				SceneManager::Instance().UnloadAll();
				SceneManager::Instance().SetSceneRoot(context.projectManager->GetProjectRoot());
				const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(scenePath);
				SceneManager::Instance().SetActiveScene(sceneId);
				service.SetEditorContext(&context);
			}

			~HistoryProjectFixture()
			{
				UndoRedoSystem::Instance().Clear();
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
			EditorContext context;
			EditorAutomationService service;
		};

		nlohmann::json ExecuteMetadataGroup(EditorAutomationService& service, const std::string& target, const std::string& name, const std::string& tag, const std::string& requestId)
		{
			return service.HandleTool("transaction.execute", {
				{"operations", nlohmann::json::array({
					{{"kind", "object.set_metadata"}, {"target", target}, {"name", name}},
					{{"kind", "object.set_metadata"}, {"target", target}, {"tag", tag}}
				})},
				{"label", "Update authored metadata"},
				{"correlationId", requestId + "-correlation"},
				{"idempotencyKey", requestId + "-key"}
			}, WriteMode::WritesEnabled, requestId);
		}

		void ResetHistoryFixture()
		{
			ScriptingSystem::Instance().SetRunning(false);
			UndoRedoSystem::Instance().Clear();
			ObjectManager::instance().clear();
		}
	}

	void RegisterEditorMcpHistoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.History.CompositeTransactionCreatesOneUndoEntry", [](TestContext& context) {
			ResetHistoryFixture();
			HistoryProjectFixture fixture("CompositeEntry");
			Object object = ObjectManager::instance().createObject("Before");
			ObjectManager::instance().setObjectTag(object, "BeforeTag");
			const std::string target = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));

			const nlohmann::json transaction = ExecuteMetadataGroup(fixture.service, target, "After", "AfterTag", "history-group");
			context.Expect(transaction.value("ok", false), "The metadata transaction should commit before history inspection.");
			const nlohmann::json state = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-state");
			context.Expect(state.value("ok", false), "History state should be readable.");
			context.Expect(state["data"].value("canUndo", false), "The committed group should be undoable.");
			context.ExpectEqual(std::string("Update authored metadata"), state["data"].value("undoLabel", std::string()), "Both writes should share one user-visible label.");
			context.ExpectEqual(static_cast<std::size_t>(1), state["data"].value("undoDepth", static_cast<std::size_t>(0)), "One accepted atomic group must create exactly one history entry.");
			ResetHistoryFixture();
		});

		registry.Add("Editor.MCP.History.UndoRedoRestoreExactBeforeAndAfterState", [](TestContext& context) {
			ResetHistoryFixture();
			HistoryProjectFixture fixture("UndoRedoRoundTrip");
			Object object = ObjectManager::instance().createObject("Before");
			ObjectManager::instance().setObjectTag(object, "BeforeTag");
			const std::string target = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));
			const nlohmann::json transaction = ExecuteMetadataGroup(fixture.service, target, "After", "AfterTag", "history-roundtrip");
			const nlohmann::json afterCommit = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-after-commit");
			const std::uint64_t commitSequence = afterCommit["data"].value("sequence", 0ull);

			const nlohmann::json undo = fixture.service.HandleTool("history.undo", {{"expectedSequence", commitSequence}}, WriteMode::WritesEnabled, "history-undo");
			context.Expect(undo.value("ok", false), "Undo with the current sequence should succeed.");
			context.ExpectEqual(transaction["data"].value("historyId", std::string()), undo["data"].value("historyId", std::string()), "Undo should identify the affected composite entry.");
			context.ExpectEqual(std::string("Before"), ObjectManager::instance().getObjectName(object), "Undo should restore the exact prior name.");
			context.ExpectEqual(std::string("BeforeTag"), ObjectManager::instance().getObjectTag(object), "Undo should restore the exact prior tag.");

			const std::uint64_t undoSequence = undo["data"].value("sequence", 0ull);
			const nlohmann::json redo = fixture.service.HandleTool("history.redo", {{"expectedSequence", undoSequence}}, WriteMode::WritesEnabled, "history-redo");
			context.Expect(redo.value("ok", false), "Redo with the post-undo sequence should succeed.");
			context.ExpectEqual(std::string("After"), ObjectManager::instance().getObjectName(object), "Redo should restore the exact committed name.");
			context.ExpectEqual(std::string("AfterTag"), ObjectManager::instance().getObjectTag(object), "Redo should restore the exact committed tag.");
			ResetHistoryFixture();
		});

		registry.Add("Editor.MCP.History.CreatedHierarchyRedoDoesNotDuplicateChildren", [](TestContext& context) {
			ResetHistoryFixture();
			HistoryProjectFixture fixture("CreatedHierarchyRoundTrip");
			const nlohmann::json transaction = fixture.service.HandleTool("transaction.execute", {
				{"operations", nlohmann::json::array({
					{{"kind", "object.create"}, {"localId", "parent"}, {"name", "Parent"}},
					{{"kind", "object.create"}, {"localId", "child"}, {"name", "Child"}},
					{{"kind", "object.set_parent"}, {"target", {{"localId", "child"}}}, {"parent", {{"localId", "parent"}}}}
				})},
				{"label", "Create hierarchy"},
				{"correlationId", "history-created-hierarchy"},
				{"idempotencyKey", "history-created-hierarchy-key"}
			}, WriteMode::WritesEnabled, "history-created-hierarchy");
			context.Expect(transaction.value("ok", false), "The created hierarchy should commit before history replay.");

			const nlohmann::json committedState = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-created-state");
			const nlohmann::json undo = fixture.service.HandleTool(
				"history.undo",
				{{"expectedSequence", committedState["data"].value("sequence", 0ull)}},
				WriteMode::WritesEnabled,
				"history-created-undo");
			context.Expect(undo.value("ok", false), "Undo should remove the complete created hierarchy.");
			context.Expect(ObjectManager::instance().getRootObjects().empty(), "Undo should leave no created hierarchy roots.");

			const nlohmann::json redo = fixture.service.HandleTool(
				"history.redo",
				{{"expectedSequence", undo["data"].value("sequence", 0ull)}},
				WriteMode::WritesEnabled,
				"history-created-redo");
			context.Expect(redo.value("ok", false), "Redo should restore the complete created hierarchy.");
			const std::vector<Object> roots = ObjectManager::instance().getRootObjects();
			context.ExpectEqual(std::size_t(1), roots.size(), "Redo must not recreate the serialized child as a duplicate root.");
			if (!roots.empty())
			{
				const std::vector<Object> children = ObjectManager::instance().getChildren(roots.front());
				context.ExpectEqual(std::size_t(1), children.size(), "Redo should restore exactly one child under the created parent.");
				if (!children.empty())
					context.ExpectEqual(std::string("Child"), ObjectManager::instance().getObjectName(children.front()), "The restored child should retain its authored identity.");
			}
			ResetHistoryFixture();
		});

		registry.Add("Editor.MCP.History.NewCommitInvalidatesRedoBranch", [](TestContext& context) {
			ResetHistoryFixture();
			HistoryProjectFixture fixture("BranchInvalidation");
			Object object = ObjectManager::instance().createObject("Initial");
			const std::string target = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));
			ExecuteMetadataGroup(fixture.service, target, "First", "FirstTag", "history-first");
			const nlohmann::json firstState = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-first-state");
			fixture.service.HandleTool("history.undo", {{"expectedSequence", firstState["data"].value("sequence", 0ull)}}, WriteMode::WritesEnabled, "history-branch-undo");

			const nlohmann::json branch = ExecuteMetadataGroup(fixture.service, target, "Second", "SecondTag", "history-second");
			context.Expect(branch.value("ok", false), "A new transaction should be accepted after undo.");
			const nlohmann::json branchState = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-branch-state");
			context.Expect(!branchState["data"].value("canRedo", true), "A new commit after undo must invalidate the abandoned redo branch.");
			context.ExpectEqual(static_cast<std::size_t>(1), branchState["data"].value("undoDepth", static_cast<std::size_t>(0)), "The new branch should expose only its current composite entry.");
			ResetHistoryFixture();
		});

		registry.Add("Editor.MCP.History.RejectsStaleSequenceWithoutChangingState", [](TestContext& context) {
			ResetHistoryFixture();
			HistoryProjectFixture fixture("StaleSequence");
			Object object = ObjectManager::instance().createObject("Initial");
			const std::string target = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));
			ExecuteMetadataGroup(fixture.service, target, "Current", "CurrentTag", "history-stale-source");
			const nlohmann::json current = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-current-state");
			const std::uint64_t currentSequence = current["data"].value("sequence", 0ull);

			const nlohmann::json stale = fixture.service.HandleTool("history.undo", {{"expectedSequence", currentSequence + 1ull}}, WriteMode::WritesEnabled, "history-stale-undo");
			context.Expect(!stale.value("ok", true), "Undo prepared against a stale history sequence must be rejected.");
			context.ExpectEqual(std::string("stale_target"), stale["error"].value("category", std::string()), "Stale history should have a specific machine-readable category.");
			context.ExpectEqual(std::string("Current"), ObjectManager::instance().getObjectName(object), "Rejected stale undo must not change the object name.");
			context.ExpectEqual(std::string("CurrentTag"), ObjectManager::instance().getObjectTag(object), "Rejected stale undo must not partially restore metadata.");
			const nlohmann::json unchanged = fixture.service.HandleTool("history.state", nlohmann::json::object(), WriteMode::ReadOnly, "history-unchanged-state");
			context.ExpectEqual(currentSequence, unchanged["data"].value("sequence", 0ull), "Rejected stale history actions must not advance the sequence.");
			ResetHistoryFixture();
		});
	}
}
