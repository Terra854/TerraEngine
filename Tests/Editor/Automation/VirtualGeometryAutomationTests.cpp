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

		std::filesystem::path WriteAutomationTinyObj(EditorContext& context, std::string_view fileName)
		{
			const std::filesystem::path projectAsset = context.assetDatabase->GetAssetRoot() / "Meshes" / std::string(fileName);
			std::filesystem::create_directories(projectAsset.parent_path());
			std::ofstream output(projectAsset, std::ios::trunc);
			output << "v 0 0 0\n";
			output << "v 1 0 0\n";
			output << "v 0 1 0\n";
			output << "vt 0 0\n";
			output << "vt 1 0\n";
			output << "vt 0 1\n";
			output << "f 1/1 2/2 3/3\n";
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
			context.ExpectEqual(std::string("skipped"), mesh.value("virtualGeometryStatus", std::string()), "The triangle fixture must report the canonical direct-meshlet route.");
			context.ExpectEqual(0u, mesh.value("detailLevelCount", 0u), "Direct meshlets must not report virtual detail levels.");
			context.Expect(mesh.value("sourceTriangleCount", 0u) > 0u, "Generated mesh should report source triangle count.");
			context.Expect(mesh.contains("settings"), "Generated mesh import response should include resolved adaptive settings.");
			context.Expect(mesh.contains("cookDiagnostics"), "Generated mesh import response should include cook diagnostics.");
			context.ExpectEqual(0u, mesh["cookDiagnostics"].value("requestedTargetCount", 0u),
				"Direct-meshlet inputs must not request virtual-geometry detail targets.");
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
			context.Expect(packageRebuild["data"]["rebuiltAssets"].front().contains("requestedDetailCount"), "Package rebuild should report requested adaptive detail count.");
			context.Expect(packageRebuild["data"]["rebuiltAssets"].front().contains("clusterCount"), "Package rebuild should report cluster count.");
			context.Expect(packageRebuild["data"]["rebuiltAssets"].front().contains("pageCount"), "Package rebuild should report page count.");
			context.Expect(packageRebuild["data"]["rebuiltAssets"].front().contains("fallbackCoverageValid"), "Package rebuild should report fallback coverage validation.");
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
			// A package is usable when all generated mesh children are usable. The
			// aggregate therefore remains cooked even when its tiny child uses direct meshlets.
			context.ExpectEqual(std::string("cooked"), inspect["data"].value("virtualGeometryStatus", std::string()), "Inspect should report a usable imported package.");
			context.ExpectEqual(0u, inspect["data"].value("detailLevels", 0u), "Direct meshlets must not expose virtual detail levels.");
			context.Expect(inspect["data"].contains("runtimeDiagnostics"), "Inspect should include runtime diagnostics container.");
			context.Expect(inspect["data"].contains("residencyDiagnostics"), "Inspect should include residency diagnostics container.");
			context.Expect(inspect["data"].contains("settings"), "Inspect should include resolved adaptive settings.");
			context.Expect(inspect["data"].contains("cookDiagnostics"), "Inspect should include cook diagnostics.");
			context.Expect(inspect["data"]["cookDiagnostics"].contains("requestedTargets"), "Inspect cook diagnostics should include requested target rows.");
			context.Expect(inspect["data"].contains("clusterSummary"), "Inspect should include cluster hierarchy summary.");
			context.Expect(inspect["data"].contains("pageResidencySummary"), "Inspect should include page residency summary.");
			context.Expect(inspect["data"].contains("selectionSummary"), "Inspect should include runtime selection summary.");
			context.Expect(inspect["data"].contains("hiZValidation"), "Inspect should include Hi-Z validation summary.");
			context.Expect(inspect["data"].contains("staleReasons"), "Inspect should include stale settings reasons.");
			context.Expect(inspect["data"].contains("packageVirtualGeometry"), "Package inspect should include generated mesh virtual geometry diagnostics.");
			const nlohmann::json& packageMeshes = inspect["data"]["packageVirtualGeometry"].value("generatedMeshes", nlohmann::json::array());
			context.Expect(!packageMeshes.empty()
				&& packageMeshes.front().value("virtualGeometryStatus", std::string()) == "skipped"
				&& packageMeshes.front().value("detailLevelCount", 1u) == 0u,
				"The triangle package child must explicitly report the canonical direct-meshlet route.");
			context.Expect(inspect["data"].contains("assetRegistrationBoundary"), "Inspect should state the asset publication boundary.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.SettingsGetSetResetRoundTrip", [](TestContext& context) {
			TempVirtualGeometryAutomationProject project("SettingsRoundTrip");
			EditorContext editorContext = MakeVirtualGeometryAutomationContext(project);
			WriteAutomationTinyObj(editorContext, "automation-settings.obj");

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			service.HandleTool(
				"asset.import",
				{{"path", "Meshes/automation-settings.obj"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-settings-import");

			const nlohmann::json before = service.HandleTool(
				"asset.virtualGeometry.settings.get",
				{{"asset", "Meshes/automation-settings.obj"}, {"includeResolved", true}},
				TerraEngine::Editor::Automation::WriteMode::ReadOnly,
				"vg-settings-get-before");
			context.Expect(before.value("ok", false), "Settings get should resolve imported mesh defaults.");
			context.Expect(!before["data"].value("hasMeshOverride", true), "Freshly imported meshes should not have a per-mesh override.");
			context.Expect(before["data"].contains("projectDefaults"), "Settings get should include project defaults.");
			context.Expect(before["data"].contains("settingsSignature"), "Settings get should include a stable settings signature.");

			const nlohmann::json saved = service.HandleTool(
				"asset.virtualGeometry.settings.set",
				{
					{"asset", "Meshes/automation-settings.obj"},
					{"override", {
						{"targetRatios", nlohmann::json::array({ 1.0f, 0.5f })},
						{"minimumSourceTriangles", 16u},
						{"minimumGeneratedTriangles", 8u}
					}},
					{"rebuildAfterSave", false}
				},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-settings-set");
			context.Expect(saved.value("ok", false), "Valid automation override should save.");
			context.ExpectEqual(std::string("saved"), saved["data"].value("status", std::string()), "Settings set should report saved status without rebuild.");
			context.Expect(saved["data"].value("hasMeshOverride", false), "Settings set should report that an override now exists.");
			context.ExpectEqual(static_cast<std::size_t>(2u), saved["data"]["resolvedSettings"]["targetRatios"].size(), "Resolved settings should include saved override ratios.");

			const nlohmann::json reset = service.HandleTool(
				"asset.virtualGeometry.settings.reset",
				{{"asset", "Meshes/automation-settings.obj"}, {"rebuildAfterReset", false}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-settings-reset");
			context.Expect(reset.value("ok", false), "Settings reset should clear the per-mesh override.");
			context.ExpectEqual(std::string("reset"), reset["data"].value("status", std::string()), "Settings reset should report reset status without rebuild.");
			context.Expect(!reset["data"].value("hasMeshOverride", true), "Reset settings should no longer report an override.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.InvalidSettingsAreAtomic", [](TestContext& context) {
			TempVirtualGeometryAutomationProject project("SettingsAtomicity");
			EditorContext editorContext = MakeVirtualGeometryAutomationContext(project);
			WriteAutomationTinyObj(editorContext, "automation-atomic.obj");

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			service.HandleTool(
				"asset.import",
				{{"path", "Meshes/automation-atomic.obj"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-atomic-import");

			const nlohmann::json valid = service.HandleTool(
				"asset.virtualGeometry.settings.set",
				{
					{"asset", "Meshes/automation-atomic.obj"},
					{"override", {{"targetRatios", nlohmann::json::array({ 1.0f, 0.5f })}}},
					{"rebuildAfterSave", false}
				},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-atomic-valid");
			context.Expect(valid.value("ok", false), "Initial valid settings should save before atomicity check.");

			const nlohmann::json invalid = service.HandleTool(
				"asset.virtualGeometry.settings.set",
				{
					{"asset", "Meshes/automation-atomic.obj"},
					{"override", {{"targetRatios", nlohmann::json::array({ 1.0f, 0.75f, 0.8f })}}},
					{"rebuildAfterSave", false}
				},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"vg-atomic-invalid");
			context.Expect(!invalid.value("ok", true), "Invalid non-descending override ratios should be rejected.");

			const nlohmann::json after = service.HandleTool(
				"asset.virtualGeometry.settings.get",
				{{"asset", "Meshes/automation-atomic.obj"}, {"includeResolved", true}},
				TerraEngine::Editor::Automation::WriteMode::ReadOnly,
				"vg-atomic-after");
			context.Expect(after.value("ok", false), "Settings get should still succeed after a rejected override.");
			context.Expect(after["data"].value("hasMeshOverride", false), "Rejected settings should leave the previous override intact.");
			context.ExpectEqual(static_cast<std::size_t>(2u), after["data"]["resolvedSettings"]["targetRatios"].size(), "Rejected settings should not replace the previous valid ratios.");
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
