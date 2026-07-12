#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"

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
		class SceneLifecycleFixture
		{
		public:
			explicit SceneLifecycleFixture(std::string name)
				: workspace(std::filesystem::temp_directory_path() / "TerraEngineMcpSceneLifecycle" / std::move(name))
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
				std::filesystem::create_directories(workspace);
				context.projectManager = std::make_shared<ProjectManager>();
				service.SetEditorContext(&context);
				// Scene automation is available only after the editor owner chooses a
				// project, so establish that native precondition directly.
				projectReady = context.projectManager->CreateProject(projectRoot(), "SceneLifecycleProject", ProjectTemplate::Blank);
			}

			~SceneLifecycleFixture()
			{
				SceneManager::Instance().UnloadAll();
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
			}

			std::filesystem::path projectRoot() const
			{
				return workspace / "SceneLifecycleProject";
			}

			std::filesystem::path workspace;
			EditorContext context;
			EditorAutomationService service;
			bool projectReady = false;
		};

		bool SceneListContains(const nlohmann::json& response, std::string_view path)
		{
			if (!response.value("ok", false) || !response["data"].contains("scenes"))
			{
				return false;
			}
			for (const nlohmann::json& scene : response["data"]["scenes"])
			{
				if (scene.value("path", std::string()) == path)
				{
					return true;
				}
			}
			return false;
		}
	}

	void RegisterEditorMcpSceneLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.SceneLifecycle.ListCreateOpenAndSaveAs", [](TestContext& context) {
			SceneLifecycleFixture fixture("CreateSaveAs");
			context.Expect(fixture.projectReady, "Scene lifecycle fixture should create an approved project.");
			const nlohmann::json initial = fixture.service.HandleTool("scene.list", nlohmann::json::object(), WriteMode::ReadOnly, "scene-list-initial");
			context.Expect(SceneListContains(initial, "Assets/Scenes/Scene.tescene"), "Scene inventory should include the blank template scene.");

			const nlohmann::json created = fixture.service.HandleTool(
				"scene.create",
				{{"path", "Assets/Scenes/Automation.tescene"}, {"name", "Automation"}, {"open", true}},
				WriteMode::WritesEnabled,
				"scene-create");
			context.Expect(created.value("ok", false), "Scene creation should publish and optionally open a project-relative scene.");
			context.ExpectEqual(std::string("Assets/Scenes/Automation.tescene"), created["data"].value("path", std::string()), "Created scene should report its project-relative path.");

			const nlohmann::json savedAs = fixture.service.HandleTool(
				"scene.save_as",
				{{"path", "Assets/Scenes/AutomationCopy.tescene"}},
				WriteMode::WritesEnabled,
				"scene-save-as");
			context.Expect(savedAs.value("ok", false), "save_as should save the active authored state to a new scene identity.");
			context.Expect(std::filesystem::exists(fixture.projectRoot() / "Assets/Scenes/AutomationCopy.tescene"), "save_as should publish the requested scene file.");
		});

		registry.Add("Editor.MCP.SceneLifecycle.RenameAndDuplicatePreserveSourceContent", [](TestContext& context) {
			SceneLifecycleFixture fixture("RenameDuplicate");
			context.Expect(fixture.projectReady, "Scene lifecycle fixture should create an approved project.");
			const nlohmann::json duplicate = fixture.service.HandleTool(
				"scene.duplicate",
				{{"source", "Assets/Scenes/Scene.tescene"}, {"destination", "Assets/Scenes/Duplicate.tescene"}},
				WriteMode::WritesEnabled,
				"scene-duplicate");
			context.Expect(duplicate.value("ok", false), "Scene duplication should create a distinct scene identity.");
			context.Expect(std::filesystem::exists(fixture.projectRoot() / "Assets/Scenes/Scene.tescene"), "Duplication must preserve the source file.");
			context.Expect(std::filesystem::exists(fixture.projectRoot() / "Assets/Scenes/Duplicate.tescene"), "Duplication must publish the destination file.");

			const nlohmann::json renamed = fixture.service.HandleTool(
				"scene.rename",
				{{"source", "Assets/Scenes/Duplicate.tescene"}, {"destination", "Assets/Scenes/Renamed.tescene"}},
				WriteMode::WritesEnabled,
				"scene-rename");
			context.Expect(renamed.value("ok", false), "Scene rename should succeed after impact preflight.");
			context.Expect(!std::filesystem::exists(fixture.projectRoot() / "Assets/Scenes/Duplicate.tescene"), "Rename should remove only the old scene path.");
			context.Expect(std::filesystem::exists(fixture.projectRoot() / "Assets/Scenes/Renamed.tescene"), "Rename should preserve scene content at the new path.");
		});

		registry.Add("Editor.MCP.SceneLifecycle.DeletePreflightReportsReferencesWithoutMutation", [](TestContext& context) {
			SceneLifecycleFixture fixture("DeleteImpact");
			context.Expect(fixture.projectReady, "Scene lifecycle fixture should create an approved project.");
			const std::filesystem::path profilePath = fixture.projectRoot() / "BuildProfiles" / "Reference.json";
			std::filesystem::create_directories(profilePath.parent_path());
			std::ofstream profile(profilePath, std::ios::binary);
			profile << nlohmann::json{{"name", "Reference"}, {"startupScene", "Assets/Scenes/Scene.tescene"}, {"includedScenes", nlohmann::json::array({"Assets/Scenes/Scene.tescene"})}}.dump(2);
			profile.close();

			const nlohmann::json impact = fixture.service.HandleTool(
				"scene.prepare_delete",
				{{"path", "Assets/Scenes/Scene.tescene"}},
				WriteMode::WritesEnabled,
				"scene-delete-impact");
			context.Expect(impact.value("ok", false), "Scene deletion preflight should return an impact report.");
			context.Expect(impact["data"].value("requiresApproval", false), "Scene deletion should require editor-owned destructive approval.");
			context.Expect(!impact["data"].value("dependents", nlohmann::json::array()).empty(), "Impact report should identify build-profile references.");
			context.Expect(std::filesystem::exists(fixture.projectRoot() / "Assets/Scenes/Scene.tescene"), "Preflight must not delete or rewrite the scene.");

			std::ifstream unchanged(profilePath, std::ios::binary);
			const std::string preserved((std::istreambuf_iterator<char>(unchanged)), std::istreambuf_iterator<char>());
			context.Expect(preserved.find("Assets/Scenes/Scene.tescene") != std::string::npos, "Preflight must preserve references until an approved commit.");
		});
	}
}
