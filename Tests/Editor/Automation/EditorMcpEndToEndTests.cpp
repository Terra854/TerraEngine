#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "BuildProfile.h"
#include "EditorContext.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"

#include <filesystem>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	void RegisterEditorMcpEndToEndTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.EndToEnd.OwnerOpenedProjectToExportAndReload", [](TestContext& context) {
			const std::filesystem::path workspace = std::filesystem::temp_directory_path() / "TerraEngineMcpEndToEnd";
			const std::filesystem::path project = workspace / "AutomatedGame";
			std::error_code ignored;
			std::filesystem::remove_all(workspace, ignored);
			std::filesystem::create_directories(workspace);
			ObjectManager::instance().clear();
			SceneManager::Instance().UnloadAll();

			EditorContext editorContext;
			editorContext.projectManager = std::make_shared<ProjectManager>();
			EditorAutomationService service;
			service.SetEditorContext(&editorContext);

			// Workspace selection is intentionally native-editor-owned. Once the
			// owner opens it, the end-to-end scenario uses semantic MCP operations
			// for authoring and export.
			context.Expect(!editorContext.projectManager->HasActiveProject(), "End-to-end scenario must begin at the no-project hub.");
			context.Expect(editorContext.projectManager->CreateProject(project, "AutomatedGame", ProjectTemplate::Blank),
				"The native editor precondition should create and open the disposable project.");

			const nlohmann::json createdScene = service.HandleTool(
				"scene.create",
				{{"path", "Assets/Scenes/Playable.tescene"}, {"name", "Playable"}, {"open", true}},
				WriteMode::WritesEnabled,
				"e2e-scene-create");
			context.Expect(createdScene.value("ok", false), "MCP should create and open a playable scene.");
			const nlohmann::json camera = service.HandleTool("object.create", {{"name", "Gameplay Camera"}}, WriteMode::WritesEnabled, "e2e-camera");
			const nlohmann::json litMesh = service.HandleTool("object.create", {{"name", "Lit Mesh"}}, WriteMode::WritesEnabled, "e2e-lit-mesh");
			context.Expect(camera.value("ok", false) && litMesh.value("ok", false), "MCP should add the authored camera and lit-mesh objects.");
			const nlohmann::json saved = service.HandleTool("scene.save", nlohmann::json::object(), WriteMode::WritesEnabled, "e2e-scene-save");
			context.Expect(saved.value("ok", false), "MCP should save the authored scene before export.");

			BuildProfile profile;
			profile.name = "Automation";
			profile.id = "automation";
			profile.outputDirectory = project / "Exports" / "Automation";
			profile.startupScene = "Scenes/Playable.tescene";
			profile.enableManagedCompilation = false;
			std::filesystem::create_directories(project / "BuildProfiles");
			context.Expect(SaveBuildProfile(profile, project / "BuildProfiles" / "Automation.json"), "End-to-end fixture should publish the profile consumed by existing export automation.");
			const nlohmann::json validation = service.HandleTool("export.validate", {{"profile", "Automation"}}, WriteMode::ReadOnly, "e2e-export-validate");
			context.Expect(validation.value("ok", false), "MCP should return structured export validation.");
			const nlohmann::json exportStart = service.HandleTool("export.start", {{"profile", "Automation"}, {"correlationId", "e2e-export"}}, WriteMode::WritesEnabled, "e2e-export-start");
			context.Expect(exportStart.value("ok", false), "MCP should accept the export as a durable job.");
			const std::string jobId = exportStart["data"].value("jobId", std::string());
			context.Expect(!jobId.empty(), "Accepted export should return a job ID.");
			context.Expect(service.GetJobService().CompleteSucceeded(jobId, {{"output", profile.outputDirectory.generic_string()}}), "Fixture should emulate the owning export worker's successful terminal publication.");
			const nlohmann::json completed = service.HandleTool("jobs.get", {{"jobId", jobId}}, WriteMode::ReadOnly, "e2e-export-poll");
			context.ExpectEqual(std::string("succeeded"), completed["data"]["job"].value("state", std::string()), "MCP should observe export completion through jobs.get.");

			// Project lifecycle ownership stays with the native editor. The MCP
			// scenario stops at saved project state, then the owner closes/reopens it.
			editorContext.projectManager->CloseProject();
			context.Expect(editorContext.projectManager->LoadProject(project),
				"The native editor should reopen the completed project from the hub.");
			context.Expect(std::filesystem::exists(project / "Assets/Scenes/Playable.tescene"), "Reopened project should retain the MCP-authored scene.");

			ObjectManager::instance().clear();
			SceneManager::Instance().UnloadAll();
			std::filesystem::remove_all(workspace, ignored);
		});
	}
}
