#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "BuildProfile.h"
#include "Components/AnimatorComponent.h"
#include "Components/SkyComponent.h"
#include "EditorContext.h"
#include "AssetDatabase.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/Transform.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
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
	using TerraEngine::Editor::Automation::AssistantCapability;
	using TerraEngine::Editor::Automation::ErrorCategory;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		nlohmann::json CanonicalPosition(double x, double y, double z)
		{
			return {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {x, y, z}}};
		}
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
				{"format", "TerraSceneUnboundedV1"},
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
			context.Expect(service.FindCapability("renderer.validation.sparseTransform.start") != nullptr,
				"Protected sparse transform validation start should be registered.");
			context.Expect(service.FindCapability("renderer.validation.sparseTransform.status") != nullptr,
				"Protected sparse transform validation status should be registered.");
			context.Expect(service.FindCapability("renderer.validation.sparseTransform.stop") != nullptr,
				"Protected sparse transform validation stop should be registered.");
			const AssistantCapability* virtualGeometryStats = service.FindCapability("renderer.virtualGeometry.stats");
			context.Expect(virtualGeometryStats != nullptr, "Virtual geometry renderer stats should be registered.");
			if (virtualGeometryStats != nullptr)
			{
				// Tool discovery must advertise the supported Shadow selector so
				// capture automation can request real shadow diagnostics directly.
				context.Expect(virtualGeometryStats->inputSummary.find("Shadow") != std::string::npos,
					"Virtual geometry stats discovery should advertise Shadow diagnostics.");
			}
		});

		registry.Add("Editor.Automation.Service.VirtualGeometryStatsEndpointValidatesContract", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json shadowWithoutRenderer = service.HandleTool(
				"renderer.virtualGeometry.stats",
				{{"viewType", "Shadow"}},
				WriteMode::ReadOnly,
				"vg-stats-shadow-missing-renderer");
			// Shadow is a supported diagnostic selector. Without a renderer it must
			// still fail as a missing target rather than being rejected as syntax.
			context.Expect(!shadowWithoutRenderer.value("ok", true), "Shadow virtual geometry stats should require a live renderer.");
			context.ExpectEqual(shadowWithoutRenderer["error"].value("category", std::string()), std::string("missing_target"), "A supported Shadow selector should fail as a missing target when no renderer exists.");

			const nlohmann::json missingRenderer = service.HandleTool(
				"renderer.virtualGeometry.stats",
				{{"viewType", "Game"}},
				WriteMode::ReadOnly,
				"vg-stats-missing-renderer");
			context.Expect(!missingRenderer.value("ok", true), "Virtual geometry stats should fail clearly without a renderer.");
			context.ExpectEqual(missingRenderer["error"].value("category", std::string()), std::string("missing_target"), "Missing renderer should use the missing-target error category.");
		});

		registry.Add("Editor.Automation.Service.SparseTransformValidationUsesProtectedTypedBatches", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			for(std::size_t index = 0U; index < 100U; ++index)
			{
				Object object = objects.createObject("Static Validation " + std::to_string(index));
				objects.AddComponent<Transform>(object);
				objects.AddComponent<MeshRenderComponent>(object);
				objects.setObjectStatic(object, true);
			}

			EditorAutomationService service;
			const nlohmann::json started = service.HandleTool(
				"renderer.validation.sparseTransform.start",
				{{"expectedObjectCount", 100}, {"percentPerFrame", 1.0}, {"displacement", 0.01}},
				WriteMode::WritesEnabled,
				"sparse-start");
			context.Expect(started.value("ok", false), "Sparse validation should accept the exact static population.");
			if(started.value("ok", false))
			{
				service.PumpRendererValidationMutationsAtProtectedFrame();
				const TerraEngine::Scene::SceneMutationBatchResult first =
					TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "SparseValidationTest.First");
				context.Expect(first.applied, "The first typed sparse transform batch should commit.");
				service.PumpRendererValidationMutationsAtProtectedFrame();
				const nlohmann::json stopped = service.HandleTool(
					"renderer.validation.sparseTransform.stop",
					{{"operationId", started["data"]["operationId"]}},
					WriteMode::WritesEnabled,
					"sparse-stop");
				context.Expect(stopped.value("ok", false), "Sparse validation should accept a stop request before the final flush.");
				if(stopped.value("ok", false))
				{
					context.ExpectEqual(stopped["data"].value("batchSize", 0U), static_cast<std::size_t>(1U),
						"One percent of one hundred objects should be one typed transform update.");
					context.ExpectEqual(stopped["data"].value("pendingMutationCount", 0U), static_cast<std::size_t>(1U),
						"The stop response should retain the unflushed final batch for protected verification.");
					const TerraEngine::Scene::SceneMutationBatchResult second =
						TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "SparseValidationTest.Second");
					context.Expect(second.applied, "The final typed sparse transform batch should commit after stop.");
					service.PumpRendererValidationMutationsAtProtectedFrame();
					const nlohmann::json drained = service.HandleTool(
						"renderer.validation.sparseTransform.status",
						{{"operationId", started["data"]["operationId"]}},
						WriteMode::ReadOnly,
						"sparse-status-drained");
					context.ExpectEqual(drained["data"].value("committedFrameCount", 0ULL), 2ULL,
						"Both protected frame batches should be proven visible.");
					context.ExpectEqual(drained["data"].value("committedMutationCount", 0ULL), 2ULL,
						"Committed evidence should conserve exact mutation count.");
					context.ExpectEqual(drained["data"].value("pendingMutationCount", 1U), static_cast<std::size_t>(0U),
						"The inactive validator should drain the final batch without enqueueing another one.");
				}
			}
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			objects.clear();
		});

		registry.Add("Editor.Automation.Service.ReadOnlySceneListingWorksWithoutProject", [](TestContext& context) {
			ObjectManager::instance().clear();
			Object parent = ObjectManager::instance().createObject("Automation Test Parent");
			Object child = ObjectManager::instance().createObject("Automation Test Child");
			context.Expect(ObjectManager::instance().setParent(child, parent), "The listing fixture hierarchy should be valid.");
			EditorAutomationService service;
			const nlohmann::json result = service.HandleTool("scene.list_objects", {{"includeComponents", false}}, WriteMode::ReadOnly, "req-3");
			context.Expect(result.value("ok", false), "Read-only scene listing should succeed without project state.");
			context.ExpectEqual(result["data"]["objects"].size(), std::size_t(2), "Scene listing should include root and nested objects.");
			const auto childResult = std::find_if(
				result["data"]["objects"].begin(),
				result["data"]["objects"].end(),
				[](const nlohmann::json& value) {
					return value.value("name", std::string{}) == "Automation Test Child";
				});
			context.Expect(childResult != result["data"]["objects"].end(), "Scene listing should include a nested child.");
			if (childResult != result["data"]["objects"].end())
			{
				context.ExpectEqual(
					childResult->value("parent", std::uint32_t{}),
					static_cast<std::uint32_t>(parent.EntityId()),
					"The compatibility parent field should retain the parent entity.");
				context.ExpectEqual(
					childResult->value("parentUuid", std::string{}),
					TerraEngine::ToHexString(ObjectManager::instance().getUUID(parent)),
					"The stable parent UUID should identify the parent across reloads.");
			}
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.Automation.Service.SceneListingPagesDeepHierarchiesBeforeComponentDiscovery", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			// A long parent chain reproduces the hierarchy shape that previously made
			// the recursive MCP listing consume one call frame per scene object.
			Object current = objects.createObject("Paged hierarchy 0");
			for (std::uint32_t index = 1u; index < 512u; ++index)
			{
				Object child = objects.createObject("Paged hierarchy " + std::to_string(index));
				context.Expect(objects.setParent(child, current), "The deep listing fixture hierarchy should remain valid.");
				current = child;
			}

			EditorAutomationService service;
			const nlohmann::json page = service.HandleTool("scene.list_objects",
				{{"includeComponents", true}, {"offset", 128u}, {"limit", 32u}},
				WriteMode::ReadOnly, "scene-page-deep");
			context.Expect(page.value("ok", false), "A deep scene listing should return a bounded page without recursive traversal.");
			context.ExpectEqual(page["data"]["objects"].size(), std::size_t(32u),
				"The requested scene page should contain only its bounded object count.");
			context.Expect(page["data"].value("hasMore", false)
				&& page["data"].value("nextOffset", 0ULL) == 160ULL,
				"A nonterminal page should advertise the next stable matched-object offset.");
			context.Expect(page["data"]["objects"].front().value("name", std::string{}) == "Paged hierarchy 128",
				"Pagination must preserve the existing stable depth-first hierarchy order.");

			const nlohmann::json invalidLimit = service.HandleTool("scene.list_objects",
				{{"limit", 257u}}, WriteMode::ReadOnly, "scene-page-invalid");
			context.Expect(!invalidLimit.value("ok", true)
				&& invalidLimit["error"].value("category", std::string{}) == "validation",
				"The service must reject an unbounded scene-list page before it constructs response JSON.");
			objects.clear();
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

		registry.Add("Editor.Automation.Service.TransformPropertyEditsInvalidateWorldTransformHistory", [](TestContext& context) {
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
			Transform::registerType();
			EditorAutomationService service;
			Object object = ObjectManager::instance().createObject("Transform Revision Source");
			const std::string objectId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));
			const std::uint64_t beforeEdit = ObjectManager::instance().GetObjectTransformRevision(object);

			const nlohmann::json set = service.HandleTool(
				"component.set_property",
				{{"object", objectId}, {"component", "Transform"}, {"property", "Scale"}, {"value", {0.25f, 0.5f, 0.75f}}},
				WriteMode::WritesEnabled,
				"transform-revision-1");
			context.Expect(!set.value("ok", true), "Generic reflected Transform property edits must be rejected by the canonical spatial authority boundary.");
			const std::uint64_t afterEdit = ObjectManager::instance().GetObjectTransformRevision(object);
			context.ExpectEqual(beforeEdit, afterEdit, "Rejected generic Transform edits must not advance world-transform history.");
			context.Expect(ObjectManager::instance().GetComponent<Transform>(object)->worldPose.scale == glm::vec3(1.0f), "Rejected Transform edits must leave canonical scale unchanged.");

			UndoRedoSystem::Instance().Undo();
			const std::uint64_t afterUndo = ObjectManager::instance().GetObjectTransformRevision(object);
			context.ExpectEqual(afterEdit, afterUndo, "No undo entry should be created for a rejected Transform edit.");
			context.Expect(ObjectManager::instance().GetComponent<Transform>(object)->worldPose.scale == glm::vec3(1.0f), "Rejected edits must retain the original Transform scale.");

			UndoRedoSystem::Instance().Redo();
			context.ExpectEqual(afterUndo, ObjectManager::instance().GetObjectTransformRevision(object), "Rejected edits must not create a redo entry.");
			context.Expect(ObjectManager::instance().GetComponent<Transform>(object)->worldPose.scale == glm::vec3(1.0f), "Rejected edits must retain the original Transform scale after redo attempt.");
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
		});

		registry.Add("Editor.Automation.Service.ComponentUnsignedAccessorDefaultsRemainMutable", [](TestContext& context) {
			ObjectManager::instance().clear();
			UndoRedoSystem::Instance().Clear();
			AnimatorComponent::registerType();
			EditorAutomationService service;
			Object object = ObjectManager::instance().createObject("Animator Policy");
			(void)ObjectManager::instance().AddComponent<AnimatorComponent>(object);
			const std::string objectName = ObjectManager::instance().getObjectName(object);

			const nlohmann::json before = service.HandleTool(
				"component.get",
				{{"object", objectName}, {"component", "AnimatorComponent"}},
				WriteMode::ReadOnly,
				"cmp-unsigned-1");
			context.Expect(before.value("ok", false)
				&& before["data"]["properties"].value("updatePolicy", std::numeric_limits<std::uint32_t>::max()) == 0U,
				"Component reads must retain reflected uint32 defaults instead of omitting them from automation metadata.");

			const nlohmann::json set = service.HandleTool(
				"component.set_property",
				{{"object", objectName}, {"component", "AnimatorComponent"}, {"property", "updatePolicy"}, {"value", 2U}},
				WriteMode::WritesEnabled,
				"cmp-unsigned-2");
			const nlohmann::json after = service.HandleTool(
				"component.get",
				{{"object", objectName}, {"component", "AnimatorComponent"}},
				WriteMode::ReadOnly,
				"cmp-unsigned-3");
			context.Expect(set.value("ok", false)
				&& after["data"]["properties"].value("updatePolicy", 0U) == 2U,
				"A reflected uint32 accessor must pass metadata preflight and publish the requested value.");

			UndoRedoSystem::Instance().Undo();
			const nlohmann::json restored = service.HandleTool(
				"component.get",
				{{"object", objectName}, {"component", "AnimatorComponent"}},
				WriteMode::ReadOnly,
				"cmp-unsigned-4");
			context.Expect(restored["data"]["properties"].value("updatePolicy", std::numeric_limits<std::uint32_t>::max()) == 0U,
				"Undo must restore the complete uint32-bearing component payload.");
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
				{{"camera", {{"position", CanonicalPosition(0.0, 1.0, 5.0)}, {"lookAt", CanonicalPosition(0.0, 0.0, 0.0)}}}, {"width", 320u}, {"height", 180u}},
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

		registry.Add("Editor.Automation.Service.CustomCameraParallelUpUsesFiniteFallback", [](TestContext& context) {
			EditorAutomationService service;
			for (const nlohmann::json& target : {
				CanonicalPosition(0.0, 2.0, 5.0),
				CanonicalPosition(0.0, -2.0, 5.0) })
			{
				const nlohmann::json result = service.HandleTool(
					"camera.capture_custom",
					{{"camera", {{"position", CanonicalPosition(0.0, 0.0, 5.0)},
					 {"lookAt", target}, {"up", nlohmann::json::array({0.0f, 1.0f, 0.0f})}}},
					 {"width", 64u}, {"height", 64u}},
					WriteMode::ReadOnly, "capture-parallel-up");
				context.Expect(result.value("ok", false),
					"Parallel and antiparallel custom cameras should still capture successfully.");
				if (!result.value("ok", false)) continue;
				const nlohmann::json& camera = result["data"]["camera"];
				const auto finite = [](const nlohmann::json& vector) {
					return vector.is_array() && vector.size() == 3u
						&& std::isfinite(vector[0].get<float>())
						&& std::isfinite(vector[1].get<float>())
						&& std::isfinite(vector[2].get<float>());
				};
				context.Expect(finite(camera["forward"]) && finite(camera["up"])
					&& camera.value("upFallbackApplied", false),
					"A collinear camera basis must report finite forward/up vectors and explicit fallback provenance.");
			}
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
