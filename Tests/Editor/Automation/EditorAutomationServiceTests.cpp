#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "BuildProfile.h"
#include "Components/SkyComponent.h"
#include "EditorContext.h"
#include "AssetDatabase.h"
#include "Components/CameraComponent.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "ProjectManager.h"
#include "UndoRedoSystem.h"

#include <filesystem>
#include <fstream>
#include <limits>
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
			context.Expect(service.FindCapability("game_view.capture") != nullptr, "Game View capture should be registered.");
			context.Expect(service.FindCapability("camera.capture_object") != nullptr, "Camera object capture should be registered.");
			context.Expect(service.FindCapability("camera.capture_custom") != nullptr, "Custom camera capture should be registered.");
			context.Expect(service.FindCapability("renderer.virtualGeometry.stats") != nullptr, "Virtual geometry renderer stats should be registered.");
		});

		registry.Add("Editor.Automation.Service.VirtualGeometryStatsEndpointValidatesContract", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json invalidView = service.HandleTool(
				"renderer.virtualGeometry.stats",
				{{"viewType", "Shadow"}},
				WriteMode::ReadOnly,
				"vg-stats-invalid-view");
			context.Expect(!invalidView.value("ok", true), "Virtual geometry stats should reject unsupported view types.");
			context.ExpectEqual(invalidView["error"].value("category", std::string()), std::string("validation"), "Unsupported view type should be a validation error.");

			const nlohmann::json missingRenderer = service.HandleTool(
				"renderer.virtualGeometry.stats",
				{{"viewType", "Game"}},
				WriteMode::ReadOnly,
				"vg-stats-missing-renderer");
			context.Expect(!missingRenderer.value("ok", true), "Virtual geometry stats should fail clearly without a renderer.");
			context.ExpectEqual(missingRenderer["error"].value("category", std::string()), std::string("missing_target"), "Missing renderer should use the missing-target error category.");
		});

		registry.Add("Editor.Automation.Service.ReadOnlySceneListingWorksWithoutProject", [](TestContext& context) {
			ObjectManager::instance().clear();
			Object object = ObjectManager::instance().createObject("Automation Test Object");
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool("scene.list_objects", {{"includeComponents", false}}, WriteMode::ReadOnly, "req-3");
			context.Expect(result.value("ok", false), "Read-only scene listing should succeed without project state.");
			context.ExpectEqual(result["data"]["objects"][0].value("name", std::string()), ObjectManager::instance().getObjectName(object), "Scene listing should include created object.");
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
			const Object objectHandle = ObjectManager::instance().makeObjectHandle(entity);
			context.Expect(TerraEngine::IsResolvedObjectIdentity(ObjectManager::instance().validateObjectHandle(objectHandle)), "Editor-created objects should have a validated compact identity before facade migration.");
			const nlohmann::json metadata = service.HandleTool("object.set_metadata", {{"object", create["data"]["object"]["uuid"]}, {"name", "Renamed Cube"}, {"tag", "Generated"}}, WriteMode::WritesEnabled, "obj-2");
			context.Expect(metadata.value("ok", false), "Metadata edit should succeed.");
			context.Expect(ObjectManager::instance().FindObjectByName("Renamed Cube").has_value(), "Renamed object should be findable.");
			UndoRedoSystem::Instance().Undo();
			context.Expect(ObjectManager::instance().FindObjectByName("Assistant Cube").has_value(), "Undo should restore object metadata.");
			const nlohmann::json deleted = service.HandleTool("object.delete", {{"object", static_cast<std::uint32_t>(entity)}}, WriteMode::WritesEnabled, "obj-3");
			context.Expect(deleted.value("ok", false), "Object delete should succeed.");
			context.Expect(!ObjectManager::instance().getObject(entity).has_value(), "Deleted object should leave object manager.");
			context.Expect(!TerraEngine::IsResolvedObjectIdentity(ObjectManager::instance().validateObjectHandle(objectHandle)), "Deleted editor objects should invalidate compact identity before undo restores authored state.");
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
			const std::string objectName = ObjectManager::instance().getObjectName(object);
			const nlohmann::json add = service.HandleTool("component.add", {{"object", objectName}, {"component", "SkyComponent"}}, WriteMode::WritesEnabled, "cmp-1");
			context.Expect(add.value("ok", false), "Component add should succeed.");
			const nlohmann::json set = service.HandleTool("component.set_property", {{"object", objectName}, {"component", "SkyComponent"}, {"property", "sourceAsset"}, {"value", "Textures/Sky/Automation.tetf"}}, WriteMode::WritesEnabled, "cmp-2");
			context.Expect(set.value("ok", false), "Component property edit should succeed.");
			const nlohmann::json get = service.HandleTool("component.get", {{"object", objectName}, {"component", "SkyComponent"}}, WriteMode::ReadOnly, "cmp-3");
			context.ExpectEqual(std::string("Textures/Sky/Automation.tetf"), get["data"]["properties"].value("sourceAsset", std::string()), "Component get should return reflected property data.");
			const nlohmann::json remove = service.HandleTool("component.remove", {{"object", objectName}, {"component", "SkyComponent"}}, WriteMode::WritesEnabled, "cmp-4");
			context.Expect(remove.value("ok", false), "Component remove should succeed.");
			UndoRedoSystem::Instance().Undo();
			const nlohmann::json restored = service.HandleTool("component.get", {{"object", objectName}, {"component", "SkyComponent"}}, WriteMode::ReadOnly, "cmp-5");
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
			// Match MainEditorSubsystem: live scene paths resolve from Assets even
			// though MCP clients use project-relative Assets/... spelling.
			SceneManager::Instance().SetSceneRoot(editorContext.projectManager->GetProjectRoot() / "Assets");
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
			const nlohmann::json compact = service.HandleTool("diagnostics.get_summary", nlohmann::json::object(), WriteMode::ReadOnly, "diag-parallel-compact");
			context.Expect(compact["data"]["parallelFrame"].contains("executionTelemetry"), "Compact diagnostics should preserve the additive scheduler telemetry envelope.");
			context.Expect(!compact["data"]["parallelFrame"]["executionTelemetry"].value("detailIncluded", true), "Compact diagnostics should avoid retaining workload and worker history arrays.");
			context.Expect(compact["data"]["parallelFrame"]["executionTelemetry"]["executionRecords"].empty(), "Compact diagnostics should omit workload decision records.");
			const nlohmann::json detailed = service.HandleTool("diagnostics.get_summary", {{"includeDetails", true}}, WriteMode::ReadOnly, "diag-parallel");
			context.Expect(detailed.value("ok", false), "Engine-backed diagnostics should succeed.");
			context.Expect(detailed["data"]["parallelFrame"].value("available", false), "Parallel-frame diagnostics should be available when an engine is attached.");
			context.Expect(detailed["data"]["parallelFrame"].contains("migrationSummary"), "Parallel-frame diagnostics should expose migration counters.");
			context.Expect(detailed["data"]["parallelFrame"].contains("workerCoverage"), "Parallel-frame diagnostics should expose worker coverage.");
			context.Expect(detailed["data"]["parallelFrame"]["executionTelemetry"].value("detailIncluded", false), "Detailed diagnostics should opt in to bounded workload and worker history arrays.");
			context.Expect(detailed["data"]["parallelFrame"]["executionTelemetry"].contains("dependencyEdges"), "Detailed diagnostics should expose explicit scheduler dependency edges.");
			context.Expect(detailed["data"]["parallelFrame"]["executionTelemetry"]["conditionalFlush"].contains("reason"), "Scheduler diagnostics should explain the conditional After Physics flush decision.");
			context.Expect(detailed["data"]["parallelFrame"]["runtimeLayout"].contains("metadataScanFallbackCount"), "Runtime layout diagnostics should expose metadata scan fallback count.");
			context.Expect(detailed["data"]["parallelFrame"]["runtimeLayout"].contains("staleObjectHandleCount"), "Runtime layout diagnostics should expose stale compact reference count.");
			context.Expect(!detailed["data"]["hardBoundaries"].empty(), "Hard-boundary classifications should be serialized.");
			bool sawSchedulerSurface = false;
			for (const auto& surface : detailed["data"]["surfaces"])
			{
				if (surface.value("surface", std::string()) == "scheduler")
				{
					sawSchedulerSurface = surface["details"].contains("parallelFrame") && surface["details"].contains("mutationDiagnostics");
				}
			}
			context.Expect(sawSchedulerSurface, "Scheduler surface should mirror parallel-frame and mutation diagnostics.");
		});

		registry.Add("Editor.Automation.Service.CustomCameraCaptureReturnsMcpImageContent", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 1.0f, 5.0f})}, {"target", nlohmann::json::array({0.0f, 0.0f, 0.0f})}, {"width", 320u}, {"height", 180u}},
				WriteMode::ReadOnly,
				"capture-custom");
			context.Expect(result.value("ok", false), "Custom camera capture should succeed without mutating scene state.");
			context.ExpectEqual(std::string("custom_camera"), result["data"].value("sourceKind", std::string()), "Custom camera capture should report its source kind.");
			context.Expect(result.contains("mcpContent"), "Capture result should include MCP content.");
			context.ExpectEqual(std::string("image"), result["mcpContent"][0].value("type", std::string()), "Capture content should include an image item.");
			context.ExpectEqual(std::string("image/png"), result["mcpContent"][0].value("mimeType", std::string()), "Capture image should be PNG.");
			context.Expect(!result["mcpContent"][0].value("data", std::string()).empty(), "Capture image should include base64 data.");
			const nlohmann::json diagnostics = service.HandleTool("diagnostics.get_summary", {{"includeDetails", true}}, WriteMode::ReadOnly, "capture-diag");
			bool sawCaptureScheduler = false;
			for (const auto& surface : diagnostics["data"]["surfaces"])
			{
				if (surface.value("surface", std::string()) == "capture")
				{
					sawCaptureScheduler = !surface["details"]["schedulerDeclarations"].empty();
				}
			}
			context.Expect(sawCaptureScheduler, "Capture diagnostics should expose scheduler declaration details after a capture.");
		});

		registry.Add("Editor.Automation.Service.CameraObjectCaptureIncludesDisabledAuthoredCameras", [](TestContext& context) {
			ObjectManager::instance().clear();
			EditorAutomationService service;
			Object cameraObject = ObjectManager::instance().createObject("Disabled Capture Camera");
			CameraComponent* camera = ObjectManager::instance().AddComponent<CameraComponent>(cameraObject);
			context.Expect(camera != nullptr, "Fixture should add a camera component.");
			camera->active = false;
			ObjectManager::instance().setObjectActive(cameraObject, false);

			const nlohmann::json result = service.HandleTool(
				"camera.capture_object",
				{{"object", "Disabled Capture Camera"}, {"includeDisabled", true}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"capture-object");
			context.Expect(result.value("ok", false), "Camera object capture should include disabled authored cameras by default.");
			context.ExpectEqual(std::string("camera_object"), result["data"].value("sourceKind", std::string()), "Camera object capture should report its source kind.");
			context.Expect(!result["data"]["object"].value("objectActive", true), "Capture metadata should report inactive authored object state.");
			context.Expect(!result["data"]["object"].value("cameraComponentActive", true), "Capture metadata should report disabled camera component state.");
			context.ExpectEqual(std::string("image"), result["mcpContent"][0].value("type", std::string()), "Disabled camera capture should still return an image item.");

			const nlohmann::json excluded = service.HandleTool(
				"camera.capture_object",
				{{"object", "Disabled Capture Camera"}, {"includeDisabled", false}, {"width", 64u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"capture-object-disabled");
			context.Expect(!excluded.value("ok", true), "includeDisabled false should reject disabled authored cameras.");
			context.ExpectEqual(std::string("invalid_camera_object"), excluded["error"].value("category", std::string()), "Disabled camera rejection should use the invalid-camera category.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.Automation.Service.CaptureValidationRejectsInvalidDimensions", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 1.0f})}, {"width", 0u}, {"height", 64u}},
				WriteMode::ReadOnly,
				"capture-invalid");
			context.Expect(!result.value("ok", true), "Capture requests should reject zero-sized targets.");
			context.ExpectEqual(std::string("validation"), result["error"].value("category", std::string()), "Invalid capture dimensions should use validation errors.");

			const nlohmann::json overflow = service.HandleTool(
				"camera.capture_custom",
				{{"position", nlohmann::json::array({0.0f, 0.0f, 1.0f})}, {"width", std::numeric_limits<std::uint64_t>::max()}, {"height", 64u}},
				WriteMode::ReadOnly,
				"capture-overflow");
			context.Expect(!overflow.value("ok", true), "Capture requests should reject dimensions that cannot fit the renderer contract.");
			context.ExpectEqual(std::string("validation"), overflow["error"].value("category", std::string()), "Overflow dimensions should use validation errors.");
		});

		registry.Add("Editor.Automation.Service.ActiveToggleUndoRestoresAuthoredState", [](TestContext& context) {
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
			EditorAutomationService service;
			Object object = ObjectManager::instance().createObject("Toggle");
			const std::string uuid = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));

			const nlohmann::json inactive = service.HandleTool("object.set_metadata", {{"object", uuid}, {"active", false}}, WriteMode::WritesEnabled, "active-1");
			context.Expect(inactive.value("ok", false), "Automation active toggle should succeed.");
			context.Expect(!ObjectManager::instance().isObjectActive(object), "Inactive authored state should be applied.");

			UndoRedoSystem::Instance().Undo();

			context.Expect(ObjectManager::instance().isObjectActive(object), "Undo should restore authored active state.");
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
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
