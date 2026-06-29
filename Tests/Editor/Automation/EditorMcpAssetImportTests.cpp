#include "../TestHarness.h"

#include "AssetDatabase.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <memory>

namespace TerraEngine::EditorTests {
	namespace {
		struct TempMcpImportProject {
			explicit TempMcpImportProject(std::string name)
				: root(std::filesystem::temp_directory_path() / "TerraEngineMcpAssetImport" / std::move(name)) {
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}

			~TempMcpImportProject() {
				std::error_code ignored;
				AssetDatabase::Instance().Shutdown();
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		EditorContext MakeContext(TempMcpImportProject& project) {
			EditorContext context;
			context.projectManager = std::make_shared<ProjectManager>();
			std::string error;
			context.projectManager->CreateProject(project.root, project.root.filename().string(), ProjectTemplate::Blank, &error);
			context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
			context.assetDatabase->Initialize(context.projectManager->GetProjectRoot());
			return context;
		}
	}

	void RegisterEditorMcpAssetImportTests(TestRegistry& registry) {
		registry.Add("Editor.MCP.AssetImport.ReturnsGltfPackageResult", [](TestContext& context) {
			TempMcpImportProject project("Response");
			EditorContext editorContext = MakeContext(project);
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path sourceFixture = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path projectAsset = editorContext.assetDatabase->GetAssetRoot() / "Models" / "one-material.gltf";
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(sourceFixture, projectAsset, std::filesystem::copy_options::overwrite_existing);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json result = service.HandleTool("asset.import", {{"path", "Models/one-material.gltf"}, {"force", true}}, TerraEngine::Editor::Automation::WriteMode::WritesEnabled, "mcp-import-1");
			context.Expect(result.value("ok", false), "MCP asset import should succeed for a project glTF.");
			context.Expect(result["data"].contains("importResult"), "MCP response should include package import details.");
			context.Expect(!result["data"]["importResult"]["generatedMeshes"].empty(), "MCP response should include generated mesh ids.");
			context.Expect(!result["data"]["importResult"]["generatedMaterials"].empty(), "MCP response should include generated material ids.");
		});

		registry.Add("Editor.MCP.AssetImport.ReturnsFbxPackageResult", [](TestContext& context) {
			TempMcpImportProject project("FbxResponse");
			EditorContext editorContext = MakeContext(project);
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path sourceFixture = sourceRoot / "External/assimp/test/models/FBX/box.fbx";
			const std::filesystem::path projectAsset = editorContext.assetDatabase->GetAssetRoot() / "Models" / "box.fbx";
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(sourceFixture, projectAsset, std::filesystem::copy_options::overwrite_existing);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json result = service.HandleTool("asset.import", {{"path", "Models/box.fbx"}, {"force", true}}, TerraEngine::Editor::Automation::WriteMode::WritesEnabled, "mcp-import-fbx-1");
			context.Expect(result.value("ok", false), "MCP asset import should succeed for a project FBX.");
			context.Expect(result["data"].contains("importResult"), "MCP FBX response should include package import details.");
			context.ExpectEqual(std::string("Fbx"), result["data"]["importResult"]["package"].value("packageKind", std::string()), "MCP FBX response should report package kind.");
			context.Expect(!result["data"]["importResult"]["generatedMeshes"].empty(), "MCP FBX response should include generated mesh ids.");
		});
	}
}
