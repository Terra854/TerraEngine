#include "../TestHarness.h"

#include "AssetDatabase.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct TempVirtualGeometryAutomationProject
		{
			explicit TempVirtualGeometryAutomationProject(std::string name)
				: root(std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryAutomation" / std::move(name))
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}

			~TempVirtualGeometryAutomationProject()
			{
				std::error_code ignored;
				AssetDatabase::Instance().Shutdown();
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		EditorContext MakeVirtualGeometryAutomationContext(TempVirtualGeometryAutomationProject& project)
		{
			EditorContext context;
			context.projectManager = std::make_shared<ProjectManager>();
			std::string error;
			context.projectManager->CreateProject(project.root, project.root.filename().string(), ProjectTemplate::Blank, &error);
			context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
			context.assetDatabase->Initialize(context.projectManager->GetProjectRoot());
			return context;
		}

		std::filesystem::path CopyGltfFixtureToProject(EditorContext& context, std::string_view fileName)
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path sourceFixture = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path projectAsset = context.assetDatabase->GetAssetRoot() / "Models" / std::string(fileName);
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(sourceFixture, projectAsset, std::filesystem::copy_options::overwrite_existing);
			return projectAsset;
		}
	}

	void RegisterVirtualGeometryAutomationTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Automation.ImportReturnsVirtualGeometryFields", [](TestContext& context) {
			TempVirtualGeometryAutomationProject project("ImportResponse");
			EditorContext editorContext = MakeVirtualGeometryAutomationContext(project);
			CopyGltfFixtureToProject(editorContext, "automation-import.gltf");

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json result = service.HandleTool(
				"asset.import",
				{{"path", "Models/automation-import.gltf"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-import");

			context.Expect(result.value("ok", false), "Virtual geometry package import should succeed through automation.");
			context.Expect(result["data"].contains("virtualGeometry"), "Import response should include a virtual geometry automation summary.");
			context.Expect(!result["data"]["generatedMeshes"].empty(), "Import response should include normalized generated mesh records.");
			const nlohmann::json& mesh = result["data"]["generatedMeshes"].front();
			context.Expect(mesh.value("usable", false), "Cooked generated meshes should be marked usable.");
			context.ExpectEqual(std::string("cooked"), mesh.value("virtualGeometryStatus", std::string()), "Generated mesh should report normalized cooked status.");
			context.Expect(mesh.value("detailLevelCount", 0u) > 0u, "Generated mesh should report detail level count.");
			context.Expect(mesh.value("sourceTriangleCount", 0u) > 0u, "Generated mesh should report source triangle count.");
			context.Expect(mesh.contains("warnings"), "Generated mesh should include warnings array.");
			context.Expect(mesh.contains("errors"), "Generated mesh should include errors array.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.RebuildHandlesPackageAndAllTargets", [](TestContext& context) {
			TempVirtualGeometryAutomationProject project("Rebuild");
			EditorContext editorContext = MakeVirtualGeometryAutomationContext(project);
			CopyGltfFixtureToProject(editorContext, "automation-rebuild.gltf");

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			service.HandleTool(
				"asset.import",
				{{"path", "Models/automation-rebuild.gltf"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-rebuild-import");

			const nlohmann::json packageRebuild = service.HandleTool(
				"asset.virtualGeometry.rebuild",
				{{"target", "Models/automation-rebuild.gltf"}, {"force", true}, {"includePackageOutputs", true}, {"diagnosticsLevel", "debug"}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-rebuild-package");
			context.Expect(packageRebuild.value("ok", false), "Package virtual geometry rebuild should succeed.");
			context.ExpectEqual(std::string("completed"), packageRebuild["data"].value("status", std::string()), "Package rebuild should complete.");
			context.Expect(!packageRebuild["data"]["rebuiltAssets"].empty(), "Package rebuild should report rebuilt assets.");
			context.Expect(packageRebuild["data"]["rebuiltAssets"].front().contains("generatedMeshes"), "Package rebuild should include generated output diagnostics when requested.");
			context.ExpectEqual(std::string("debug"), packageRebuild["data"].value("diagnosticsLevel", std::string()), "Rebuild response should echo diagnostics level.");

			const nlohmann::json allRebuild = service.HandleTool(
				"asset.virtualGeometry.rebuild",
				{{"target", "all"}, {"force", false}, {"diagnosticsLevel", "summary"}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-rebuild-all");
			context.Expect(allRebuild.value("ok", false), "All-target virtual geometry rebuild should succeed.");
			context.Expect(allRebuild["data"].contains("rebuiltAssets"), "All-target rebuild should report rebuilt assets array.");
			context.Expect(allRebuild["data"].contains("failedAssets"), "All-target rebuild should report failed assets array.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.InspectReturnsCookAndRuntimeDiagnostics", [](TestContext& context) {
			TempVirtualGeometryAutomationProject project("Inspect");
			EditorContext editorContext = MakeVirtualGeometryAutomationContext(project);
			CopyGltfFixtureToProject(editorContext, "automation-inspect.gltf");

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			service.HandleTool(
				"asset.import",
				{{"path", "Models/automation-inspect.gltf"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-inspect-import");

			const nlohmann::json inspect = service.HandleTool(
				"asset.virtualGeometry.inspect",
				{{"asset", "Models/automation-inspect.gltf"}, {"includeRuntimeState", true}, {"viewType", "Game"}},
				TerraEngine::Editor::Automation::WriteMode::ReadOnly,
				"vg-auto-inspect");

			context.Expect(inspect.value("ok", false), "Virtual geometry inspect should succeed for an imported package mesh.");
			context.ExpectEqual(std::string("cooked"), inspect["data"].value("virtualGeometryStatus", std::string()), "Inspect should report cooked virtual geometry.");
			context.Expect(inspect["data"].value("detailLevels", 0u) > 0u, "Inspect should expose cooked detail count.");
			context.Expect(inspect["data"].contains("runtimeDiagnostics"), "Inspect should include runtime diagnostics container.");
			context.Expect(inspect["data"].contains("residencyDiagnostics"), "Inspect should include residency diagnostics container.");
			context.Expect(inspect["data"].contains("assetRegistrationBoundary"), "Inspect should state the asset publication boundary.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.InvalidRequestsDoNotPublishPartialSuccess", [](TestContext& context) {
			TempVirtualGeometryAutomationProject project("FailureAtomicity");
			EditorContext editorContext = MakeVirtualGeometryAutomationContext(project);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json missingImport = service.HandleTool(
				"asset.import",
				{{"path", "Models/missing.gltf"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-missing-import");
			context.Expect(!missingImport.value("ok", true), "Missing import should fail before publishing generated outputs.");

			const nlohmann::json missingRebuild = service.HandleTool(
				"asset.virtualGeometry.rebuild",
				{{"target", "Models/missing.gltf"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-auto-missing-rebuild");
			context.Expect(!missingRebuild.value("ok", true), "Missing rebuild target should fail.");
			context.Expect(missingRebuild["data"].empty(), "Failed rebuild validation should not report rebuilt generated outputs.");
		});
	}
}
