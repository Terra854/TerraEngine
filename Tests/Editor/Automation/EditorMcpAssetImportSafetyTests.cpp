#include "../TestHarness.h"

#include "AssetDatabase.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <filesystem>
#include <memory>

namespace TerraEngine::EditorTests {
	namespace {
		struct TempMcpSafetyProject {
			TempMcpSafetyProject()
				: root(std::filesystem::temp_directory_path() / "TerraEngineMcpAssetImportSafety") {
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}

			~TempMcpSafetyProject() {
				std::error_code ignored;
				AssetDatabase::Instance().Shutdown();
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		EditorContext MakeContext(TempMcpSafetyProject& project) {
			EditorContext context;
			context.projectManager = std::make_shared<ProjectManager>();
			std::string error;
			context.projectManager->CreateProject(project.root, project.root.filename().string(), ProjectTemplate::Blank, &error);
			context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
			context.assetDatabase->Initialize(context.projectManager->GetProjectRoot());
			return context;
		}
	}

	void RegisterEditorMcpAssetImportSafetyTests(TestRegistry& registry) {
		registry.Add("Editor.MCP.AssetImport.RejectsOutsideProjectPaths", [](TestContext& context) {
			TempMcpSafetyProject project;
			EditorContext editorContext = MakeContext(project);
			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);

			const nlohmann::json traversal = service.HandleTool("asset.import", {{"path", "../outside.gltf"}}, TerraEngine::Editor::Automation::WriteMode::WritesEnabled, "mcp-import-safe-1");
			context.Expect(!traversal.value("ok", true), "Traversal imports should be rejected.");
			context.ExpectEqual(std::string("validation"), traversal["error"].value("category", std::string()), "Traversal rejection should be a validation error.");

			const nlohmann::json absolute = service.HandleTool("asset.import", {{"path", (project.root / "outside.gltf").string()}}, TerraEngine::Editor::Automation::WriteMode::WritesEnabled, "mcp-import-safe-2");
			context.Expect(!absolute.value("ok", true), "Absolute imports should be rejected.");
			context.ExpectEqual(std::string("validation"), absolute["error"].value("category", std::string()), "Absolute rejection should be a validation error.");
		});
	}
}
