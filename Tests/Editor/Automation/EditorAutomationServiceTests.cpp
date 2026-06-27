#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "BuildProfile.h"
#include "Components/SkyComponent.h"
#include "EditorContext.h"
#include "AssetDatabase.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "ProjectManager.h"
#include "UndoRedoSystem.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_set>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::ErrorCategory;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class TempAutomationProject
		{
		public:
			explicit TempAutomationProject(const std::string& name)
				: root(std::filesystem::temp_directory_path() / "TerraEngineEditorAutomation" / name)
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}

			~TempAutomationProject()
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		nlohmann::json MakeSceneJson(const std::string& name)
		{
			return {
				{"name", name},
				{"sceneSettings", nlohmann::json::object()},
				{"objects", nlohmann::json::array()}
			};
		}

		void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream stream(path, std::ios::binary);
			stream << contents;
		}

		EditorContext MakeProjectContext(TempAutomationProject& workspace)
		{
			EditorContext context;
			context.projectManager = std::make_shared<ProjectManager>();
			std::string error;
			context.projectManager->CreateProject(workspace.root, workspace.root.filename().string(), ProjectTemplate::Blank, &error);
			context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
			context.assetDatabase->Initialize(context.projectManager->GetProjectRoot());
			return context;
		}
	}

	void RegisterEditorAutomationServiceTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Service.EnvelopesUseContractFields", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json ok = service.BuildSuccess("req-1", "done", {{"value", 7}});
			context.Expect(ok.value("ok", false), "Success envelope should set ok true.");
			context.ExpectEqual(ok.value("requestId", std::string()), std::string("req-1"), "Success envelope should preserve request id.");
			context.Expect(ok.contains("data"), "Success envelope should include data.");
			context.Expect(ok.contains("error"), "Success envelope should include error field.");

			const nlohmann::json error = service.BuildError("req-2", ErrorCategory::Validation, "bad input", "target");
			context.Expect(!error.value("ok", true), "Error envelope should set ok false.");
			context.ExpectEqual(error["error"].value("category", std::string()), std::string("validation"), "Error category should be serialized.");
			context.ExpectEqual(error["error"].value("target", std::string()), std::string("target"), "Error target should be serialized when provided.");
		});

		registry.Add("Editor.Automation.Service.CapabilitiesAreUnique", [](TestContext& context) {
			EditorAutomationService service;
			std::unordered_set<std::string> names;
			for (const auto& capability : service.GetCapabilities())
			{
				context.Expect(names.insert(capability.name).second, "Capability names should be unique.");
				context.Expect(!capability.category.empty(), "Capability category should be present.");
				context.Expect(!capability.inputSummary.empty(), "Capability input summary should be present.");
			}
			context.Expect(service.FindCapability("scene.list_objects") != nullptr, "Scene listing should be registered.");
			context.Expect(service.FindCapability("diagnostics.get_summary") != nullptr, "Diagnostics should be registered.");
			context.Expect(service.FindCapability("export.start") != nullptr, "Export start should be registered.");
		});

		registry.Add("Editor.Automation.Service.ReadOnlySceneListingWorksWithoutProject", [](TestContext& context) {
			ObjectManager::instance().clear();
			Object object = ObjectManager::instance().createObject("Automation Test Object");
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool("scene.list_objects", {{"includeComponents", false}}, WriteMode::ReadOnly, "req-3");
			context.Expect(result.value("ok", false), "Read-only scene listing should succeed without project state.");
			context.ExpectEqual(result["data"]["objects"][0].value("name", std::string()), object.getName(), "Scene listing should include created object.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.Automation.Service.ObjectMetadataDeleteAndUndoWork", [](TestContext& context) {
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
			ScriptingSystem::Instance().SetRunning(false);
			EditorAutomationService service;
			const nlohmann::json create = service.HandleTool("object.create", {{"name", "Assistant Cube"}}, WriteMode::WritesEnabled, "obj-1");
			context.Expect(create.value("ok", false), "Object create should succeed.");
			const auto entity = static_cast<entt::entity>(create["data"]["object"]["entity"].get<std::uint32_t>());
			const nlohmann::json metadata = service.HandleTool("object.set_metadata", {{"object", create["data"]["object"]["uuid"]}, {"name", "Renamed Cube"}, {"tag", "Generated"}}, WriteMode::WritesEnabled, "obj-2");
			context.Expect(metadata.value("ok", false), "Metadata edit should succeed.");
			context.Expect(ObjectManager::instance().FindObjectByName("Renamed Cube").has_value(), "Renamed object should be findable.");
			UndoRedoSystem::Instance().Undo();
			context.Expect(ObjectManager::instance().FindObjectByName("Assistant Cube").has_value(), "Undo should restore object metadata.");
			const nlohmann::json deleted = service.HandleTool("object.delete", {{"object", static_cast<std::uint32_t>(entity)}}, WriteMode::WritesEnabled, "obj-3");
			context.Expect(deleted.value("ok", false), "Object delete should succeed.");
			context.Expect(!ObjectManager::instance().getObject(entity).has_value(), "Deleted object should leave object manager.");
			UndoRedoSystem::Instance().Undo();
			context.Expect(ObjectManager::instance().FindObjectByName("Assistant Cube").has_value(), "Undo should restore deleted object.");
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
			ScriptingSystem::Instance().SetRunning(false);
		});

		registry.Add("Editor.Automation.Service.ComponentAddGetSetRemoveAndUndoWork", [](TestContext& context) {
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
			SkyComponent::registerType();
			EditorAutomationService service;
			Object object = ObjectManager::instance().createObject("Sky Source");
			const nlohmann::json add = service.HandleTool("component.add", {{"object", object.getName()}, {"component", "SkyComponent"}}, WriteMode::WritesEnabled, "cmp-1");
			context.Expect(add.value("ok", false), "Component add should succeed.");
			const nlohmann::json set = service.HandleTool("component.set_property", {{"object", object.getName()}, {"component", "SkyComponent"}, {"property", "sourceAsset"}, {"value", "Textures/Sky/Automation.tetf"}}, WriteMode::WritesEnabled, "cmp-2");
			context.Expect(set.value("ok", false), "Component property edit should succeed.");
			const nlohmann::json get = service.HandleTool("component.get", {{"object", object.getName()}, {"component", "SkyComponent"}}, WriteMode::ReadOnly, "cmp-3");
			context.ExpectEqual(std::string("Textures/Sky/Automation.tetf"), get["data"]["properties"].value("sourceAsset", std::string()), "Component get should return reflected property data.");
			const nlohmann::json remove = service.HandleTool("component.remove", {{"object", object.getName()}, {"component", "SkyComponent"}}, WriteMode::WritesEnabled, "cmp-4");
			context.Expect(remove.value("ok", false), "Component remove should succeed.");
			UndoRedoSystem::Instance().Undo();
			const nlohmann::json restored = service.HandleTool("component.get", {{"object", object.getName()}, {"component", "SkyComponent"}}, WriteMode::ReadOnly, "cmp-5");
			context.Expect(restored.value("ok", false), "Undo should restore removed component.");
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
		});

		registry.Add("Editor.Automation.Service.SceneOpenSaveClearsDirtyState", [](TestContext& context) {
			TempAutomationProject workspace("SceneOpenSave");
			EditorContext editorContext = MakeProjectContext(workspace);
			const std::filesystem::path scenes = editorContext.projectManager->GetProjectRoot() / "Assets" / "Scenes";
			WriteTextFile(scenes / "Automation.tescene", MakeSceneJson("Automation").dump(4));
			SceneManager::Instance().UnloadAll();
			SceneManager::Instance().SetSceneRoot(editorContext.projectManager->GetProjectRoot());
			EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json open = service.HandleTool("scene.open", {{"path", "Assets/Scenes/Automation.tescene"}}, WriteMode::WritesEnabled, "scene-1");
			context.Expect(open.value("ok", false), "Scene open should load project-relative scene files.");
			ObjectManager::instance().createObject("Dirty Object");
			SceneManager::Instance().ForceMarkActiveSceneDirty();
			context.Expect(SceneManager::Instance().IsActiveSceneDirty(), "Fixture should mark the active scene dirty before save.");
			const nlohmann::json save = service.HandleTool("scene.save", nlohmann::json::object(), WriteMode::WritesEnabled, "scene-2");
			context.Expect(save.value("ok", false), "Scene save should succeed.");
			context.Expect(!save["data"].value("dirty", true), "Scene save should clear dirty state.");
			ObjectManager::instance().clear();
			SceneManager::Instance().UnloadAll();
		});

		registry.Add("Editor.Automation.Service.DiagnosticsEmptyAndDetailedStatesAreSafe", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json empty = service.HandleTool("diagnostics.get_summary", nlohmann::json::object(), WriteMode::ReadOnly, "diag-1");
			context.Expect(empty.value("ok", false), "Diagnostics should succeed without an open project.");
			context.Expect(empty["data"].value("emptyStateSafe", false), "Diagnostics should advertise empty-state safety.");
			context.Expect(empty["data"].contains("parallelFrame"), "Diagnostics should include the parallel-frame contract even without an engine.");
			context.Expect(!empty["data"]["parallelFrame"].value("available", true), "Missing engine state should be reported explicitly.");
			context.Expect(empty["data"].contains("hardBoundaries"), "Diagnostics should include hard-boundary classifications.");
			context.Expect(empty["data"].contains("mutationDiagnostics"), "Diagnostics should include mutation counters.");
			const nlohmann::json detailed = service.HandleTool("diagnostics.get_summary", {{"includeDetails", true}}, WriteMode::ReadOnly, "diag-2");
			context.Expect(!detailed["data"]["surfaces"].empty(), "Detailed diagnostics should include surfaces.");
			context.Expect(detailed["data"]["mutationDiagnostics"].contains("acceptedMutationCount"), "Mutation diagnostics should expose accepted mutation counts.");
			context.Expect(detailed["data"]["mutationDiagnostics"].contains("rollbackCount"), "Mutation diagnostics should expose rollback counts.");
			context.Expect(detailed["data"]["mutationDiagnostics"].value("rejectedBatchesPreventPartialState", false), "Mutation diagnostics should document rollback safety.");
		});

		registry.Add("Editor.Automation.Service.DiagnosticsExposeParallelFrameState", [](TestContext& context) {
			Engine engine;
			EditorContext editorContext;
			editorContext.engine = &engine;
			EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json detailed = service.HandleTool("diagnostics.get_summary", {{"includeDetails", true}}, WriteMode::ReadOnly, "diag-parallel");
			context.Expect(detailed.value("ok", false), "Engine-backed diagnostics should succeed.");
			context.Expect(detailed["data"]["parallelFrame"].value("available", false), "Parallel-frame diagnostics should be available when an engine is attached.");
			context.Expect(detailed["data"]["parallelFrame"].contains("migrationSummary"), "Parallel-frame diagnostics should expose migration counters.");
			context.Expect(detailed["data"]["parallelFrame"].contains("workerCoverage"), "Parallel-frame diagnostics should expose worker coverage.");
			context.Expect(!detailed["data"]["hardBoundaries"].empty(), "Hard-boundary classifications should be serialized.");
			context.Expect(detailed["data"]["surfaces"][4]["details"].contains("parallelFrame"), "Scheduler surface should mirror parallel-frame diagnostics.");
			context.Expect(detailed["data"]["surfaces"][4]["details"].contains("mutationDiagnostics"), "Scheduler surface should mirror mutation diagnostics.");
		});

		registry.Add("Editor.Automation.Service.AssetImportListAndExportJobsUseEditorSystems", [](TestContext& context) {
			TempAutomationProject workspace("AssetExport");
			EditorContext editorContext = MakeProjectContext(workspace);
			const std::filesystem::path projectRoot = editorContext.projectManager->GetProjectRoot();
			WriteTextFile(projectRoot / "Assets" / "Notes" / "automation.txt", "sample");
			BuildProfile profile;
			profile.name = "Automation";
			profile.id = "automation";
			profile.outputDirectory = "Builds/Automation";
			profile.startupScene = "Scenes/Automation.tescene";
			profile.enableManagedCompilation = false;
			context.Expect(SaveBuildProfile(profile, projectRoot / "BuildProfiles" / "automation.json"), "Fixture profile should save.");
			EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json import = service.HandleTool("asset.import", {{"path", "Notes/automation.txt"}, {"force", true}}, WriteMode::WritesEnabled, "asset-1");
			context.Expect(import.value("ok", false), "Asset import should call AssetDatabase for project assets.");
			const nlohmann::json list = service.HandleTool("asset.list", {{"search", "automation"}}, WriteMode::ReadOnly, "asset-2");
			context.Expect(!list["data"]["assets"].empty(), "Asset list should include imported database entries.");
			const nlohmann::json validate = service.HandleTool("export.validate", {{"profile", "Automation"}}, WriteMode::ReadOnly, "export-1");
			context.Expect(validate.value("ok", false), "Export validation should load BuildProfiles from the project.");
			const nlohmann::json start = service.HandleTool("export.start", {{"profile", "Automation"}}, WriteMode::WritesEnabled, "export-2");
			context.Expect(start.value("ok", false), "Export start should create a pollable job.");
			const nlohmann::json cancel = service.HandleTool("export.cancel", {{"jobId", start["data"].value("jobId", "")}}, WriteMode::WritesEnabled, "export-3");
			context.Expect(cancel.value("ok", false), "Export cancel should update an existing job.");
			context.ExpectEqual(std::string("canceled"), cancel["data"].value("state", std::string()), "Canceled job should report canceled state.");
			editorContext.assetDatabase->Shutdown();
		});

		registry.Add("Editor.Automation.Service.WriteModeBlocksMutatingCommands", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool("object.create", {{"name", "Blocked"}}, WriteMode::ReadOnly, "req-4");
			context.Expect(!result.value("ok", true), "Mutating commands should fail in read-only mode.");
			context.ExpectEqual(result["error"].value("category", std::string()), std::string("permission"), "Read-only block should be a permission error.");
		});
	}
}
