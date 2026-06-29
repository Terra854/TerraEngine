// EditorAutomationService.cpp - Contract implementation for editor automation.
//
// This file keeps assistant-facing tool behavior in one place. Transport code
// supplies JSON, bridge code supplies policy, and this service performs the
// editor validation/action work against TerraEngine's existing systems.

#include "EditorAutomationService.h"

#include "../Assets/AssetImportResult.h"
#include "../Assets/AssetDatabase.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "../../Export/BuildProfile.h"
#include "../../Export/ExportValidator.h"
#include "../../AssetCompilers/GltfPackageImporter.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/RuntimeLayout.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Core/Scheduling/FrameWorkload.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		std::string EntityToString(entt::entity entity)
		{
			return std::to_string(static_cast<std::uint32_t>(entity));
		}

		bool IsWriteMutation(MutationMode mode)
		{
			return mode != MutationMode::ReadOnly;
		}

		nlohmann::json StatusEnvelope(bool ok, std::string_view requestId, RequestStatus status, std::string_view message)
		{
			nlohmann::json result;
			result["ok"] = ok;
			result["requestId"] = requestId;
			result["status"] = ToString(status);
			result["message"] = message;
			return result;
		}

		bool ContainsCaseInsensitive(std::string value, std::string query)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value.find(query) != std::string::npos;
		}

		nlohmann::json SerializeExportDiagnostic(const ExportDiagnostic& diagnostic)
		{
			const char* severity = "info";
			if (diagnostic.severity == ExportDiagnosticSeverity::Warning) severity = "warning";
			if (diagnostic.severity == ExportDiagnosticSeverity::Error) severity = "error";
			return {
				{"severity", severity},
				{"category", diagnostic.category},
				{"message", diagnostic.message},
				{"filePath", diagnostic.filePath},
				{"line", diagnostic.line}
			};
		}

		nlohmann::json SerializeExportValidation(const ExportValidationResult& result)
		{
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const ExportDiagnostic& diagnostic : result.diagnostics)
			{
				diagnostics.push_back(SerializeExportDiagnostic(diagnostic));
			}
			return {
				{"canExport", result.passed},
				{"errorCount", result.errorCount},
				{"warningCount", result.warningCount},
				{"issues", diagnostics},
				{"materialFallbacks", result.materialFallbackDiagnostics}
			};
		}

		std::optional<BuildProfile> FindBuildProfile(const std::filesystem::path& projectRoot, const std::string& selector)
		{
			const std::filesystem::path profileDirectory = projectRoot / "BuildProfiles";
			for (BuildProfile profile : LoadAllBuildProfiles(profileDirectory))
			{
				if (profile.name == selector || profile.id == selector || profile.filePath.filename().string() == selector)
				{
					return profile;
				}
			}
			const std::filesystem::path directPath = profileDirectory / selector;
			if (std::filesystem::exists(directPath))
			{
				BuildProfile profile = LoadBuildProfile(directPath);
				if (!profile.name.empty())
				{
					return profile;
				}
			}
			return std::nullopt;
		}

		nlohmann::json SerializeHardBoundary(const TerraEngine::Scheduling::HardBoundary& boundary)
		{
			return {
				{"name", boundary.boundaryName},
				{"classification", TerraEngine::Scheduling::ToString(boundary.currentClassification)},
				{"riskFactors", boundary.riskFactors},
				{"safeSubwork", boundary.safeSubwork},
				{"protectedCommit", boundary.protectedCommit},
				{"requiredEvidence", boundary.requiredEvidence}
			};
		}

		nlohmann::json SerializeArchitectureMigrationSummary(const TerraEngine::Scheduling::ArchitectureMigrationSummary& summary)
		{
			return {
				{"workerSafeCount", summary.workerSafeCount},
				{"protectedCount", summary.protectedCount},
				{"inlineCount", summary.inlineCount},
				{"rejectedCount", summary.rejectedCount},
				{"fallbackCount", summary.fallbackCount},
				{"mutationFlushCount", summary.mutationFlushCount},
				{"workerPhaseCount", summary.workerPhaseCount},
				{"protectedCommitCount", summary.protectedCommitCount},
				{"barrierCount", summary.barrierCount},
				{"hardBoundaryCount", summary.hardBoundaryCount},
				{"partiallyWorkerSafeBoundaryCount", summary.partiallyWorkerSafeBoundaryCount},
				{"protectedBaselineTimeNs", summary.protectedBaselineTimeNs},
				{"workerPreparationTimeNs", summary.workerPreparationTimeNs},
				{"protectedCommitTimeNs", summary.protectedCommitTimeNs},
				{"coordinatorWaitTimeNs", summary.coordinatorWaitTimeNs},
				{"workerUtilization", summary.workerUtilization},
				{"activeMembershipInvalidationCount", summary.activeMembershipInvalidationCount},
				{"staleObjectHandleCount", summary.staleObjectHandleCount},
				{"hierarchySnapshotRebuildCount", summary.hierarchySnapshotRebuildCount},
				{"componentAccessGrantDeniedCount", summary.componentAccessGrantDeniedCount},
				{"fallbackReasons", summary.fallbackReasons},
				{"workerCategories", summary.workerCategories},
				{"protectedCategories", summary.protectedCategories},
				{"rejectedCategories", summary.rejectedCategories},
				{"hardBoundaryFindings", summary.hardBoundaryFindings},
				{"formatted", summary.formatted}
			};
		}

		nlohmann::json SerializeWorkerCoverage(const TerraEngine::Scheduling::WorkerCoverageReport& report)
		{
			return {
				{"frameId", report.frameId},
				{"workerCategories", report.workerCategories},
				{"protectedCategories", report.protectedCategories},
				{"rejectedCategories", report.rejectedCategories},
				{"fallbackCategories", report.fallbackCategories},
				{"hardBoundaryFindings", report.hardBoundaryFindings},
				{"barrierCount", report.barrierCount},
				{"protectedTimeNs", report.protectedTimeNs},
				{"workerTimeNs", report.workerTimeNs},
				{"formattedSummary", report.formattedSummary}
			};
		}

		nlohmann::json SerializeParallelFrameDiagnostics(const ::Engine* engine)
		{
			if (engine == nullptr)
			{
				return {{"available", false}};
			}

			const TerraEngine::Scheduling::FrameExecutionPlan& plan = engine->GetParallelFrameExecutionPlan();
			const TerraEngine::Scene::LayoutDiagnosticsSummary& layout = engine->GetRuntimeLayoutDiagnostics();
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const TerraEngine::Scheduling::ArchitectureDiagnostic& diagnostic : plan.diagnostics)
			{
				diagnostics.push_back({
					{"severity", TerraEngine::Scheduling::ToString(diagnostic.severity)},
					{"code", TerraEngine::Scheduling::ToString(diagnostic.code)},
					{"workloadName", diagnostic.workloadName},
					{"resourceName", diagnostic.resourceName},
					{"message", diagnostic.message},
					{"recommendedAction", diagnostic.recommendedAction}
				});
			}

			return {
				{"available", true},
				{"frameId", plan.frameId},
				{"executionMode", TerraEngine::Scheduling::ToString(plan.executionMode)},
				{"parallelExecutionUsed", plan.parallelExecutionUsed},
				{"valid", plan.valid},
				{"completed", plan.completed},
				{"workloadCount", plan.workloads.size()},
				{"resultCount", plan.results.size()},
				{"diagnostics", diagnostics},
				{"migrationSummary", SerializeArchitectureMigrationSummary(engine->GetArchitectureMigrationSummary())},
				{"workerCoverage", SerializeWorkerCoverage(engine->GetWorkerCoverageReport())},
				{"runtimeLayout", nlohmann::json{
					{"frameId", layout.frameId},
					{"layoutReused", layout.layoutReused},
					{"layoutRebuildCount", layout.layoutRebuildCount},
					{"rebuildReasons", layout.rebuildReasons},
					{"activeObjectCount", layout.activeObjectCount},
					{"totalObjectCount", layout.totalObjectCount},
					{"transformRecordCount", layout.transformRecordCount},
					{"renderRecordCount", layout.renderRecordCount},
					{"fallbackCount", layout.fallbackCount},
					{"metadataScanFallbackCount", layout.metadataScanFallbackCount},
					{"fullObjectFacadeFallbackCount", layout.fullObjectFacadeFallbackCount},
					{"objectHandleFallbackCount", layout.objectHandleFallbackCount},
					{"staleObjectHandleCount", layout.staleObjectHandleCount},
					{"validationMismatchCount", layout.validationMismatchCount},
					{"smallSceneGuardTriggered", layout.smallSceneGuardTriggered}
				}}
			};
		}

		nlohmann::json SerializeMutationDiagnostics()
		{
			const ScriptingSystem::ScriptingFrameResult& scripting = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
			return {
				{"pendingMutationCount", TerraEngine::Scene::SceneMutationQueue::Instance().Size()},
				{"acceptedMutationCount", scripting.acceptedMutations},
				{"rejectedMutationCount", scripting.rejectedMutations},
				{"rollbackCount", 0},
				{"rejectedBatchesPreventPartialState", true},
				{"typedMutationCount", scripting.typedMutations},
				{"jsonMutationFallbackCount", scripting.jsonMutationFallbacks},
				{"coalescedMutationCount", scripting.coalescedMutations},
				{"deferredMutationCount", scripting.accessReport.deferredMutations},
				{"accessRejectedMutationCount", scripting.accessReport.rejectedMutations}
			};
		}
	}

	const char* ToString(BridgeState value)
	{
		switch (value)
		{
			case BridgeState::Disabled: return "disabled";
			case BridgeState::Listening: return "listening";
			case BridgeState::Connected: return "connected";
			case BridgeState::Stopping: return "stopping";
			case BridgeState::Stopped: return "stopped";
		}
		return "unknown";
	}

	const char* ToString(WriteMode value)
	{
		return value == WriteMode::WritesEnabled ? "writes_enabled" : "read_only";
	}

	const char* ToString(RequestStatus value)
	{
		switch (value)
		{
			case RequestStatus::Queued: return "queued";
			case RequestStatus::Running: return "running";
			case RequestStatus::WaitingApproval: return "waiting_approval";
			case RequestStatus::Completed: return "completed";
			case RequestStatus::Failed: return "failed";
			case RequestStatus::Rejected: return "rejected";
			case RequestStatus::Canceled: return "canceled";
		}
		return "unknown";
	}

	const char* ToString(MutationMode value)
	{
		switch (value)
		{
			case MutationMode::ReadOnly: return "read_only";
			case MutationMode::EditorSelection: return "editor_selection";
			case MutationMode::SceneMutation: return "scene_mutation";
			case MutationMode::AssetMutation: return "asset_mutation";
			case MutationMode::ExportMutation: return "export_mutation";
			case MutationMode::Destructive: return "destructive";
		}
		return "unknown";
	}

	const char* ToString(ErrorCategory value)
	{
		switch (value)
		{
			case ErrorCategory::Validation: return "validation";
			case ErrorCategory::Permission: return "permission";
			case ErrorCategory::MissingTarget: return "missing_target";
			case ErrorCategory::UnsupportedCapability: return "unsupported_capability";
			case ErrorCategory::Conflict: return "conflict";
			case ErrorCategory::Transport: return "transport";
			case ErrorCategory::Internal: return "internal";
			case ErrorCategory::Canceled: return "canceled";
		}
		return "internal";
	}

	const char* ToString(ActivityEventType value)
	{
		switch (value)
		{
			case ActivityEventType::Connection: return "connection";
			case ActivityEventType::Rejection: return "rejection";
			case ActivityEventType::Configuration: return "configuration";
			case ActivityEventType::Request: return "request";
			case ActivityEventType::Approval: return "approval";
			case ActivityEventType::Mutation: return "mutation";
			case ActivityEventType::Diagnostic: return "diagnostic";
			case ActivityEventType::Export: return "export";
		}
		return "request";
	}

	std::optional<WriteMode> ParseWriteMode(std::string_view value)
	{
		if (value == "read_only")
		{
			return WriteMode::ReadOnly;
		}
		if (value == "writes_enabled")
		{
			return WriteMode::WritesEnabled;
		}
		return std::nullopt;
	}

	EditorAutomationService::EditorAutomationService()
	{
		RegisterDefaultCapabilities();
	}

	void EditorAutomationService::SetEditorContext(EditorContext* context)
	{
		editorContext = context;
	}

	void EditorAutomationService::SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter)
	{
		getSelectedEntity = std::move(getter);
		setSelectedEntity = std::move(setter);
	}

	void EditorAutomationService::SetBridgeToolCallback(BridgeToolCallback callback)
	{
		bridgeToolCallback = std::move(callback);
	}

	void EditorAutomationService::SetActivityCallback(ActivityCallback callback)
	{
		activityCallback = std::move(callback);
	}

	bool EditorAutomationService::RegisterCapability(const AssistantCapability& capability)
	{
		if (capability.name.empty() || capabilityIndexByName.find(capability.name) != capabilityIndexByName.end())
		{
			return false;
		}
		capabilityIndexByName.emplace(capability.name, capabilities.size());
		capabilities.push_back(capability);
		return true;
	}

	void EditorAutomationService::RegisterDefaultCapabilities()
	{
		if (!capabilities.empty())
		{
			return;
		}

		const auto add = [this](std::string name, std::string category, MutationMode mode, bool requiresProject, bool requiresScene, bool requiresApproval, std::string inputSummary) {
			AssistantCapability capability;
			capability.name = std::move(name);
			capability.category = std::move(category);
			capability.mutationMode = mode;
			capability.mutatesState = IsWriteMutation(mode);
			capability.requiresProject = requiresProject;
			capability.requiresScene = requiresScene;
			capability.requiresApproval = requiresApproval;
			capability.inputSummary = std::move(inputSummary);
			RegisterCapability(capability);
		};

		// Bridge enablement and write scope are editor-owned controls. MCP
		// clients may inspect status and activity, but they cannot grant or
		// revoke their own write permissions.
		add("activity.list", "activity", MutationMode::ReadOnly, false, false, false, "limit?: integer, filter?: string");
		add("scene.open", "scene", MutationMode::SceneMutation, true, false, true, "path: project-relative scene path");
		add("scene.save", "scene", MutationMode::SceneMutation, true, true, false, "path?: project-relative scene path");
		add("scene.list_objects", "scene", MutationMode::ReadOnly, false, false, false, "includeComponents?: bool, search?: string");
		add("object.create", "object", MutationMode::SceneMutation, false, false, false, "name: string, parent?: target, select?: bool");
		add("object.delete", "object", MutationMode::Destructive, false, true, true, "object: uuid|entity|name");
		add("object.select", "object", MutationMode::EditorSelection, false, true, false, "object: uuid|entity|name");
		add("object.set_metadata", "object", MutationMode::SceneMutation, false, true, false, "object plus name/tag/layer/active/static fields");
		add("object.set_parent", "object", MutationMode::SceneMutation, false, true, false, "object: target, parent?: target|null");
		add("component.add", "component", MutationMode::SceneMutation, false, true, false, "object: target, component: string");
		add("component.remove", "component", MutationMode::SceneMutation, false, true, false, "object: target, component: string");
		add("component.get", "component", MutationMode::ReadOnly, false, true, false, "object: target, component: string");
		add("component.set_property", "component", MutationMode::SceneMutation, false, true, false, "object/component/property/value");
		add("asset.list", "asset", MutationMode::ReadOnly, true, false, false, "type?: string, search?: string");
		add("asset.import", "asset", MutationMode::AssetMutation, true, false, false, "path: project-relative asset path, force?: bool; glTF returns generated asset ids and diagnostics");
		add("asset.prepare_delete", "asset", MutationMode::Destructive, true, false, true, "path: project-relative asset path");
		add("export.validate", "export", MutationMode::ReadOnly, true, false, false, "profile: string");
		add("export.start", "export", MutationMode::ExportMutation, true, false, true, "profile: string, confirmOutput?: string");
		add("export.cancel", "export", MutationMode::ExportMutation, true, false, false, "jobId: string");
		add("diagnostics.get_summary", "diagnostics", MutationMode::ReadOnly, false, false, false, "includeDetails?: bool, surface?: string");
	}

	const std::vector<AssistantCapability>& EditorAutomationService::GetCapabilities() const
	{
		return capabilities;
	}

	const AssistantCapability* EditorAutomationService::FindCapability(std::string_view name) const
	{
		const auto it = capabilityIndexByName.find(std::string(name));
		return it == capabilityIndexByName.end() ? nullptr : &capabilities[it->second];
	}

	nlohmann::json EditorAutomationService::BuildSuccess(std::string_view requestId, std::string_view message, nlohmann::json data) const
	{
		nlohmann::json result = StatusEnvelope(true, requestId, RequestStatus::Completed, message);
		result["data"] = std::move(data);
		result["error"] = nullptr;
		return result;
	}

	nlohmann::json EditorAutomationService::BuildError(std::string_view requestId, ErrorCategory category, std::string_view message, std::string_view target) const
	{
		nlohmann::json result = StatusEnvelope(false, requestId, category == ErrorCategory::Permission ? RequestStatus::Rejected : RequestStatus::Failed, message);
		result["data"] = nlohmann::json::object();
		result["error"] = {
			{"category", ToString(category)},
			{"details", message}
		};
		if (!target.empty())
		{
			result["error"]["target"] = target;
		}
		return result;
	}

	nlohmann::json EditorAutomationService::HandleTool(std::string_view name, const nlohmann::json& arguments, WriteMode writeMode, const std::string& requestId)
	{
		const AssistantCapability* capability = FindCapability(name);
		if (capability == nullptr)
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Unknown editor automation capability.");
		}
		const std::string toolName(name);
		if (capability->mutatesState && writeMode != WriteMode::WritesEnabled)
		{
			return BuildError(requestId, ErrorCategory::Permission, "The bridge is in read-only mode.");
		}
		if (!arguments.is_object())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Tool arguments must be a JSON object.");
		}
		if (capability->requiresProject && (editorContext == nullptr || editorContext->projectManager == nullptr || !editorContext->projectManager->HasActiveProject()))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Open a project before running this capability.");
		}

		if (toolName == "activity.list" && bridgeToolCallback)
		{
			return bridgeToolCallback(nlohmann::json{{"name", toolName}, {"arguments", arguments}, {"requestId", requestId}});
		}
		if (toolName == "scene.list_objects") return HandleSceneListObjects(arguments, requestId);
		if (toolName == "scene.open")
		{
			nlohmann::json validation = ValidateProjectRelativePath(arguments, "path", requestId);
			if (!validation.value("ok", false)) return validation;
			const std::filesystem::path relativeScenePath = validation["path"].get<std::string>();
			const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(relativeScenePath);
			if (sceneId == TerraEngine::EMPTY_UUID || !SceneManager::Instance().SetActiveScene(sceneId))
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Scene could not be loaded.", relativeScenePath.generic_string());
			}
			UndoRedoSystem::Instance().Clear();
			return BuildSuccess(requestId, "Opened scene.", {{"path", relativeScenePath.generic_string()}});
		}
		if (toolName == "scene.save") return HandleSceneSave(arguments, requestId);
		if (toolName == "object.create") return HandleObjectCreate(arguments, requestId);
		if (toolName == "object.delete") return HandleObjectDelete(arguments, requestId);
		if (toolName == "object.select") return HandleObjectSelect(arguments, requestId);
		if (toolName == "object.set_metadata") return HandleObjectSetMetadata(arguments, requestId);
		if (toolName == "object.set_parent") return HandleObjectSetParent(arguments, requestId);
		if (toolName == "component.add") return HandleComponentAdd(arguments, requestId);
		if (toolName == "component.remove") return HandleComponentRemove(arguments, requestId);
		if (toolName == "component.get") return HandleComponentGet(arguments, requestId);
		if (toolName == "component.set_property") return HandleComponentSetProperty(arguments, requestId);
		if (toolName == "asset.list") return HandleAssetList(arguments, requestId);
		if (toolName == "asset.import") return HandleAssetImport(arguments, requestId);
		if (toolName == "asset.prepare_delete") return HandleAssetPrepareDelete(arguments, requestId);
		if (toolName == "export.validate") return HandleExportValidate(arguments, requestId);
		if (toolName == "export.start") return HandleExportStart(arguments, requestId);
		if (toolName == "export.cancel") return HandleExportCancel(arguments, requestId);
		if (toolName == "diagnostics.get_summary") return HandleDiagnosticsGetSummary(arguments, requestId);
		return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Capability metadata exists but no handler is registered.");
	}

	nlohmann::json EditorAutomationService::BuildCapabilityList() const
	{
		nlohmann::json items = nlohmann::json::array();
		for (const AssistantCapability& capability : capabilities)
		{
			items.push_back({
				{"name", capability.name},
				{"kind", capability.kind},
				{"category", capability.category},
				{"mutationMode", ToString(capability.mutationMode)},
				{"mutatesState", capability.mutatesState},
				{"requiresProject", capability.requiresProject},
				{"requiresScene", capability.requiresScene},
				{"requiresApproval", capability.requiresApproval},
				{"inputSummary", capability.inputSummary}
			});
		}
		return {{"capabilities", items}, {"count", items.size()}};
	}

	nlohmann::json EditorAutomationService::BuildProjectState() const
	{
		nlohmann::json data;
		const bool hasProject = editorContext != nullptr && editorContext->projectManager != nullptr && editorContext->projectManager->HasActiveProject();
		data["hasProject"] = hasProject;
		data["projectRoot"] = hasProject ? editorContext->projectManager->GetProjectRoot().string() : "";
		const auto* scene = SceneManager::Instance().GetActiveScene();
		data["activeScene"] = scene != nullptr ? scene->name : "";
		data["sceneDirty"] = SceneManager::Instance().IsActiveSceneDirty();
		data["playMode"] = ScriptingSystem::Instance().IsRunning();
		data["selectedObject"] = nullptr;
		if (getSelectedEntity)
		{
			std::optional<entt::entity> selectedEntity = getSelectedEntity();
			if (selectedEntity && ObjectManager::instance().getObject(*selectedEntity))
			{
				data["selectedObject"] = SerializeObjectSummary(*selectedEntity, false);
			}
		}
		return data;
	}

	nlohmann::json EditorAutomationService::BuildSceneSummary() const
	{
		nlohmann::json data;
		const auto* scene = SceneManager::Instance().GetActiveScene();
		data["activeScene"] = scene != nullptr ? scene->name : "";
		data["dirty"] = SceneManager::Instance().IsActiveSceneDirty();
		data["roots"] = nlohmann::json::array();
		std::vector<Object> roots = ObjectManager::instance().getRootObjects();
		ObjectManager::instance().SortObjectsByStableId(roots);
		std::size_t objectCount = 0;
		for (Object object : roots)
		{
			data["roots"].push_back(SerializeObjectSummary(object.EntityId(), true));
			++objectCount;
		}
		data["objectCount"] = objectCount;
		return data;
	}

	nlohmann::json EditorAutomationService::BuildDiagnosticsSummary(bool includeDetails) const
	{
		nlohmann::json exportJobJson = nlohmann::json::array();
		for (const ExportJob& job : exportJobs)
		{
			exportJobJson.push_back({
				{"jobId", job.jobId},
				{"profile", job.profile},
				{"state", job.state},
				{"progress", job.progress},
				{"cancelRequested", job.cancelRequested}
			});
		}
		const std::size_t assetJobCount = editorContext != nullptr && editorContext->assetDatabase != nullptr
			? editorContext->assetDatabase->GetCompileJobsCount()
			: 0u;
		nlohmann::json data;
		nlohmann::json boundaryFindings = nlohmann::json::array();
		for (const TerraEngine::Scheduling::HardBoundary& boundary : TerraEngine::Scheduling::InitialWorkerHardBoundaries())
		{
			boundaryFindings.push_back(SerializeHardBoundary(boundary));
		}
		const ::Engine* engine = editorContext != nullptr ? editorContext->engine : nullptr;
		const nlohmann::json parallelFrame = SerializeParallelFrameDiagnostics(engine);
		const nlohmann::json mutationDiagnostics = SerializeMutationDiagnostics();
		const auto& runtimeMembership = ObjectManager::instance().GetRuntimeActiveMembershipSnapshot();
		std::size_t activeRuntimeObjects = 0u;
		for (const TerraEngine::Scene::RuntimeActiveMembership& membership : runtimeMembership)
		{
			if (membership.objectActive)
			{
				++activeRuntimeObjects;
			}
		}
		data["surfaces"] = nlohmann::json::array({
			{{"surface", "render"}, {"state", "available"}, {"details", includeDetails ? nlohmann::json{{"note", "Render diagnostics are available through the profiler/resource windows."}} : nlohmann::json::object()}},
			{{"surface", "runtimeLayout"}, {"state", "available"}, {"details", nlohmann::json{{"authoredObjectCount", runtimeMembership.size()}, {"activeRuntimeObjectCount", activeRuntimeObjects}, {"membershipRevision", ObjectManager::instance().GetRuntimeActiveMembershipRevision()}}}},
			{{"surface", "resources"}, {"state", assetJobCount > 0 ? "compiling" : "available"}, {"details", nlohmann::json{{"assetCompileJobs", assetJobCount}}}},
			{{"surface", "streaming"}, {"state", "available"}, {"details", nlohmann::json::object()}},
			{{"surface", "scheduler"}, {"state", "available"}, {"details", nlohmann::json{{"workerBoundaries", boundaryFindings}, {"parallelFrame", parallelFrame}, {"mutationDiagnostics", mutationDiagnostics}}}},
			{{"surface", "scripting"}, {"state", ScriptingSystem::Instance().IsRunning() ? "running" : "idle"}, {"details", nlohmann::json{{"coordinatorState", editorContext ? editorContext->scripting.coordinatorState : "Idle"}}}},
			{{"surface", "export"}, {"state", exportJobs.empty() ? "idle" : exportJobs.front().state}, {"details", nlohmann::json{{"jobs", exportJobJson}}}}
		});
		data["parallelFrame"] = parallelFrame;
		data["hardBoundaries"] = boundaryFindings;
		data["mutationDiagnostics"] = mutationDiagnostics;
		data["emptyStateSafe"] = true;
		return data;
	}

	nlohmann::json EditorAutomationService::BuildActivityList(const std::vector<ActivityRecord>& records) const
	{
		nlohmann::json items = nlohmann::json::array();
		for (const ActivityRecord& record : records)
		{
			items.push_back({
				{"sequence", record.sequence},
				{"requestId", record.requestId},
				{"capability", record.capability},
				{"message", record.message},
				{"outcome", record.outcome},
				{"type", ToString(record.type)}
			});
		}
		return {{"activity", items}, {"count", items.size()}};
	}

	std::vector<ExportJob> EditorAutomationService::GetExportJobs() const
	{
		return exportJobs;
	}

	EditorAutomationService::ReflectionMutationPreflight EditorAutomationService::PreflightReflectionMutation(const nlohmann::json& arguments) const
	{
		ReflectionMutationPreflight result;
		result.classification = "protected";
		result.safeSubwork = "Target, component, property, and payload validation can run before the protected editor mutation.";
		result.protectedCommit = "Reflected setters, undo stack updates, object structure notifications, scene dirty markers, and selection/runtime inspector side effects stay protected.";
		if (!arguments.is_object())
		{
			result.message = "Tool arguments must be a JSON object.";
			return result;
		}

		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		const std::string property = arguments.value("property", "");
		if (!target.resolved || component.empty() || property.empty() || !arguments.contains("value"))
		{
			result.message = "component.set_property requires object, component, property, and value.";
			return result;
		}

		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto componentIt = ComponentAccessRegistry().find(component);
		if (componentIt == ComponentAccessRegistry().end() || !componentIt->second.entityHasComponent(ObjectManager::instance().getRegistry(), *target.entity))
		{
			result.message = "Component is not present on the object.";
			return result;
		}

		const nlohmann::json existingPayload = componentIt->second.entitySerialize(ObjectManager::instance().getRegistry(), *target.entity);
		if (!existingPayload.contains(property))
		{
			result.message = "Property is not present in component reflection metadata.";
			return result;
		}

		result.valid = true;
		result.message = "Reflection mutation preflight passed; apply the edit through the protected editor mutation handler.";
		return result;
	}

	EditorTarget EditorAutomationService::ResolveTarget(const nlohmann::json& value) const
	{
		EditorTarget target;
		if (value.is_number_unsigned() || value.is_number_integer())
		{
			const auto entity = static_cast<entt::entity>(value.get<std::uint32_t>());
			target.input = EntityToString(entity);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(entity))
			{
				target.entity = entity;
				target.displayName = objects.getObjectName(*object);
				target.uuid = TerraEngine::ToHexString(objects.getUUID(*object));
				target.resolved = true;
			}
			return target;
		}
		if (!value.is_string())
		{
			return target;
		}

		target.input = value.get<std::string>();
		if (const auto parsed = TerraEngine::ParseUUID(target.input))
		{
			ObjectManager& objects = ObjectManager::instance();
			if (auto object = objects.getObject(*parsed))
			{
				target.entity = object->EntityId();
				target.displayName = objects.getObjectName(*object);
				target.uuid = TerraEngine::ToHexString(objects.getUUID(*object));
				target.resolved = true;
				return target;
			}
		}
		if (auto object = ObjectManager::instance().FindObjectByName(target.input))
		{
			target.entity = object->EntityId();
			target.displayName = ObjectManager::instance().getObjectName(*object);
			target.uuid = TerraEngine::ToHexString(ObjectManager::instance().getUUID(*object));
			target.resolved = true;
		}
		return target;
	}

	nlohmann::json EditorAutomationService::SerializeObjectSummary(entt::entity object, bool includeComponents) const
	{
		ObjectManager& objectManager = ObjectManager::instance();
		std::optional<Object> objectHandle = objectManager.getObject(object);
		nlohmann::json data;
		data["entity"] = static_cast<std::uint32_t>(object);
		data["uuid"] = objectHandle ? TerraEngine::ToHexString(objectManager.getUUID(*objectHandle)) : std::string{};
		data["name"] = objectHandle ? objectManager.getObjectName(*objectHandle) : "Unnamed";
		data["tag"] = objectHandle ? objectManager.getObjectTag(*objectHandle) : "Untagged";
		data["layer"] = objectHandle ? objectManager.getObjectLayer(*objectHandle) : "Default";
		data["active"] = objectHandle ? objectManager.isObjectActive(*objectHandle) : false;
		data["static"] = objectHandle ? objectManager.isObjectStatic(*objectHandle) : false;
		std::optional<Object> parent = objectHandle ? objectManager.getParent(*objectHandle) : std::nullopt;
		data["parent"] = parent ? nlohmann::json(static_cast<std::uint32_t>(parent->EntityId())) : nullptr;
		if (includeComponents)
		{
			nlohmann::json components = nlohmann::json::array();
			std::lock_guard lock(ComponentAccessRegistryMutex());
			for (const auto& [name, access] : ComponentAccessRegistry())
			{
				if (access.entityHasComponent && access.entityHasComponent(objectManager.getRegistry(), object))
				{
					components.push_back(name);
				}
			}
			data["components"] = components;
		}
		return data;
	}

	nlohmann::json EditorAutomationService::HandleSceneListObjects(const nlohmann::json& arguments, const std::string& requestId)
	{
		const bool includeComponents = arguments.value("includeComponents", false);
		const std::string search = arguments.value("search", "");
		nlohmann::json objects = nlohmann::json::array();
		std::vector<Object> roots = ObjectManager::instance().getRootObjects();
		ObjectManager::instance().SortObjectsByStableId(roots);
		for (Object object : roots)
		{
			const bool matches = search.empty()
				|| ContainsCaseInsensitive(ObjectManager::instance().getObjectName(object), search)
				|| ContainsCaseInsensitive(ObjectManager::instance().getObjectTag(object), search)
				|| ContainsCaseInsensitive(ObjectManager::instance().getObjectLayer(object), search);
			if (matches)
			{
				objects.push_back(SerializeObjectSummary(object.EntityId(), includeComponents));
			}
		}
		return BuildSuccess(requestId, "Listed scene objects.", {{"objects", objects}});
	}

	nlohmann::json EditorAutomationService::HandleSceneSave(const nlohmann::json& arguments, const std::string& requestId)
	{
		const bool saved = arguments.contains("path") && arguments["path"].is_string()
			? SceneManager::Instance().SaveScene(arguments["path"].get<std::string>())
			: SceneManager::Instance().SaveScene();
		if (!saved)
		{
			return BuildError(requestId, ErrorCategory::Internal, "Scene save failed.");
		}
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "scene.save", "Saved active scene.");
		}
		return BuildSuccess(requestId, "Saved active scene.", {{"dirty", SceneManager::Instance().IsActiveSceneDirty()}});
	}

	nlohmann::json EditorAutomationService::HandleObjectCreate(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (!arguments.contains("name") || !arguments["name"].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation, "object.create requires a string name.");
		}
		Object object = ObjectManager::instance().createObject(arguments["name"].get<std::string>());
		const entt::entity entity = object.EntityId();
		if (arguments.contains("parent") && !arguments["parent"].is_null())
		{
			EditorTarget parent = ResolveTarget(arguments["parent"]);
			if (!parent.resolved)
			{
				ObjectManager::instance().removeObject(object);
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parent.input);
			}
			std::optional<Object> parentObject = ObjectManager::instance().getObject(*parent.entity);
			if (!parentObject || !ObjectManager::instance().setParent(object, parentObject))
			{
				ObjectManager::instance().removeObject(object);
				return BuildError(requestId, ErrorCategory::Conflict, "Parent change would create an invalid hierarchy.");
			}
		}
		if (arguments.value("select", false) && setSelectedEntity)
		{
			setSelectedEntity(entity);
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(entity));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.create", "Created object " + ObjectManager::instance().getObjectName(object) + ".");
		return BuildSuccess(requestId, "Created object.", {{"object", SerializeObjectSummary(entity, true)}});
	}

	nlohmann::json EditorAutomationService::HandleObjectDelete(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!target.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}
		// DeleteObjectCommand snapshots the object before it disappears, so the
		// command is created first and pushed only after the mutation succeeds.
		auto command = std::make_unique<DeleteObjectCommand>(*target.entity);
		if (std::optional<Object> object = ObjectManager::instance().getObject(*target.entity))
		{
			ObjectManager::instance().removeObject(*object);
		}
		UndoRedoSystem::Instance().PushCommand(std::move(command));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.delete", "Deleted object " + target.displayName + ".");
		return BuildSuccess(requestId, "Deleted object.", {{"object", target.displayName}});
	}

	nlohmann::json EditorAutomationService::HandleObjectSelect(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!target.resolved || !setSelectedEntity)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Selectable object was not found.", target.input);
		}
		setSelectedEntity(*target.entity);
		return BuildSuccess(requestId, "Selected object.", {{"object", target.displayName}});
	}

	nlohmann::json EditorAutomationService::HandleObjectSetMetadata(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!target.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::optional<Object> objectHandle = objectManager.getObject(object);
		if (!objectHandle)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}
		const nlohmann::json oldMetadata = {
			{"name", objectManager.getObjectName(*objectHandle)},
			{"tag", objectManager.getObjectTag(*objectHandle)},
			{"layer", objectManager.getObjectLayer(*objectHandle)},
			{"active", objectManager.isObjectActive(*objectHandle)},
			{"static", objectManager.isObjectStatic(*objectHandle)}
		};
		if (arguments.contains("name") && arguments["name"].is_string()) objectManager.setObjectName(*objectHandle, arguments["name"].get<std::string>());
		if (arguments.contains("tag") && arguments["tag"].is_string()) objectManager.setObjectTag(*objectHandle, arguments["tag"].get<std::string>());
		if (arguments.contains("layer") && arguments["layer"].is_string()) objectManager.setObjectLayer(*objectHandle, arguments["layer"].get<std::string>());
		if (arguments.contains("active") && arguments["active"].is_boolean()) objectManager.setObjectActive(*objectHandle, arguments["active"].get<bool>());
		if (arguments.contains("static") && arguments["static"].is_boolean()) objectManager.setObjectStatic(*objectHandle, arguments["static"].get<bool>());
		const nlohmann::json newMetadata = {
			{"name", objectManager.getObjectName(*objectHandle)},
			{"tag", objectManager.getObjectTag(*objectHandle)},
			{"layer", objectManager.getObjectLayer(*objectHandle)},
			{"active", objectManager.isObjectActive(*objectHandle)},
			{"static", objectManager.isObjectStatic(*objectHandle)}
		};
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetObjectMetadataCommand>(object, oldMetadata, newMetadata));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.set_metadata", "Updated object metadata.");
		return BuildSuccess(requestId, "Updated object metadata.", {{"object", SerializeObjectSummary(object, true)}});
	}

	nlohmann::json EditorAutomationService::HandleObjectSetParent(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget childTarget = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!childTarget.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Child object was not found.", childTarget.input);
		}
		const entt::entity child = *childTarget.entity;
		std::optional<TerraEngine::UUID> oldParentUuid;
		std::optional<Object> childObject = ObjectManager::instance().getObject(child);
		if (!childObject)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Child object was not found.", childTarget.input);
		}
		if (std::optional<Object> oldParent = ObjectManager::instance().getParent(*childObject))
		{
			oldParentUuid = ObjectManager::instance().getUUID(*oldParent);
		}
		std::optional<Object> parent;
		std::optional<TerraEngine::UUID> newParentUuid;
		if (arguments.contains("parent") && !arguments["parent"].is_null())
		{
			EditorTarget parentTarget = ResolveTarget(arguments["parent"]);
			if (!parentTarget.resolved)
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parentTarget.input);
			}
			parent = ObjectManager::instance().getObject(*parentTarget.entity);
			if (!parent)
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parentTarget.input);
			}
			newParentUuid = ObjectManager::instance().getUUID(*parent);
		}
		if (!ObjectManager::instance().setParent(*childObject, parent))
		{
			return BuildError(requestId, ErrorCategory::Conflict, "Parent change would create an invalid hierarchy.");
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetParentCommand>(child, oldParentUuid, newParentUuid));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.set_parent", "Updated object parent.");
		return BuildSuccess(requestId, "Updated object parent.", {{"object", SerializeObjectSummary(child, true)}});
	}

	nlohmann::json EditorAutomationService::HandleComponentAdd(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		if (!target.resolved || component.empty())
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object or component name was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end())
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Component is not registered for automation.", component);
		}
		it->second.entityAddComponent(objectManager.getRegistry(), object);
		if (std::optional<Object> objectHandle = objectManager.getObject(object))
		{
			objectManager.notifyObjectStructureChanged(*objectHandle);
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<AddComponentCommand>(object, component));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.add", "Added component " + component + ".");
		return BuildSuccess(requestId, "Added component.", {{"object", target.displayName}, {"component", component}});
	}

	nlohmann::json EditorAutomationService::HandleComponentRemove(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		if (!target.resolved || component.empty())
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object or component name was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.entityHasComponent(objectManager.getRegistry(), object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		// RemoveComponentCommand captures the existing component payload for
		// undo, so construct it while the component is still attached.
		auto command = std::make_unique<RemoveComponentCommand>(object, component);
		it->second.entityRemoveComponent(objectManager.getRegistry(), object);
		if (std::optional<Object> objectHandle = objectManager.getObject(object))
		{
			objectManager.notifyObjectStructureChanged(*objectHandle);
		}
		UndoRedoSystem::Instance().PushCommand(std::move(command));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.remove", "Removed component " + component + ".");
		return BuildSuccess(requestId, "Removed component.", {{"object", target.displayName}, {"component", component}});
	}

	nlohmann::json EditorAutomationService::HandleComponentGet(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		if (!target.resolved || component.empty())
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object or component name was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.entityHasComponent(objectManager.getRegistry(), object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		return BuildSuccess(requestId, "Read component.", {{"component", component}, {"properties", it->second.entitySerialize(objectManager.getRegistry(), object)}});
	}

	nlohmann::json EditorAutomationService::HandleComponentSetProperty(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		const std::string property = arguments.value("property", "");
		const ReflectionMutationPreflight preflight = PreflightReflectionMutation(arguments);
		if (!preflight.valid)
		{
			return BuildError(requestId, ErrorCategory::Validation, preflight.message);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.entityHasComponent(objectManager.getRegistry(), object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		const nlohmann::json oldPayload = it->second.entitySerialize(objectManager.getRegistry(), object);
		it->second.entityDeserialize(objectManager.getRegistry(), object, nlohmann::json{{property, arguments["value"]}});
		const nlohmann::json newPayload = it->second.entitySerialize(objectManager.getRegistry(), object);
		if (std::optional<Object> objectHandle = objectManager.getObject(object))
		{
			objectManager.notifyObjectStructureChanged(*objectHandle);
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetComponentPayloadCommand>(object, component, oldPayload, newPayload));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.set_property", "Updated component property.");
		return BuildSuccess(requestId, "Updated component property.", {{"component", component}, {"property", property}});
	}

	nlohmann::json EditorAutomationService::ValidateProjectRelativePath(const nlohmann::json& arguments, std::string_view key, const std::string& requestId) const
	{
		if (!arguments.contains(std::string(key)) || !arguments[std::string(key)].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Expected a project-relative path string.");
		}
		const std::filesystem::path relative = arguments[std::string(key)].get<std::string>();
		if (relative.is_absolute() || relative.string().find("..") != std::string::npos)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Path must stay inside the active project.");
		}
		return nlohmann::json{{"ok", true}, {"path", relative.generic_string()}};
	}

	nlohmann::json EditorAutomationService::ValidateAssetPath(const nlohmann::json& arguments, const std::string& requestId) const
	{
		nlohmann::json validation = ValidateProjectRelativePath(arguments, "path", requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		std::filesystem::path relative = validation["path"].get<std::string>();
		if (!relative.empty() && relative.begin()->string() == "Assets")
		{
			relative = std::filesystem::relative(relative, "Assets");
		}
		if (relative.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Asset path must name a file under Assets.");
		}
		return nlohmann::json{{"ok", true}, {"path", relative.generic_string()}};
	}

	nlohmann::json EditorAutomationService::HandleAssetList(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string search = arguments.value("search", "");
		nlohmann::json assets = nlohmann::json::array();
		if (editorContext != nullptr && editorContext->assetDatabase != nullptr)
		{
			const std::filesystem::path assetRoot = editorContext->assetDatabase->GetAssetRoot();
			for (const auto& [absolutePath, metadata] : editorContext->assetDatabase->GetAssets())
			{
				const std::string path = std::filesystem::relative(absolutePath, assetRoot).generic_string();
				if (search.empty() || ContainsCaseInsensitive(path, search) || ContainsCaseInsensitive(metadata.importerName, search))
				{
					assets.push_back({
						{"path", path},
						{"guid", TerraEngine::ToHexString(metadata.guid)},
						{"importer", metadata.importerName},
						{"compiledPath", metadata.compiledPath.generic_string()},
						{"status", editorContext->assetDatabase->IsAssetCompiling(absolutePath) ? "compiling" : "known"}
					});
				}
			}
		}
		return BuildSuccess(requestId, "Listed project assets.", {{"assets", assets}});
	}

	nlohmann::json EditorAutomationService::HandleAssetImport(const nlohmann::json& arguments, const std::string& requestId)
	{
		nlohmann::json validation = ValidateAssetPath(arguments, requestId);
		if (!validation.value("ok", false)) return validation;
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}
		const std::filesystem::path relative = validation["path"].get<std::string>();
		const std::filesystem::path absolutePath = editorContext->assetDatabase->GetAssetRoot() / relative;
		if (!std::filesystem::exists(absolutePath) || std::filesystem::is_directory(absolutePath))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset file was not found.", relative.generic_string());
		}
		const std::filesystem::path canonicalAssetRoot = std::filesystem::weakly_canonical(editorContext->assetDatabase->GetAssetRoot());
		const std::filesystem::path canonicalAssetPath = std::filesystem::weakly_canonical(absolutePath);
		const std::string rootText = canonicalAssetRoot.generic_string();
		const std::string assetText = canonicalAssetPath.generic_string();
		if (assetText.size() < rootText.size()
			|| assetText.compare(0, rootText.size(), rootText) != 0
			|| (assetText.size() > rootText.size() && assetText[rootText.size()] != '/'))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Asset import path must stay inside the active project's Assets folder.", relative.generic_string());
		}
		editorContext->assetDatabase->ImportAsset(absolutePath, arguments.value("force", false));
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "asset.import", "Queued asset import.");
		}

		nlohmann::json data = {
			{"path", validation["path"]},
			{"force", arguments.value("force", false)},
			{"status", "queued"}
		};
		if (TerraEngine::AssetPipeline::IsGltfPackageSourceExtension(absolutePath))
		{
			const std::filesystem::path resultPath = TerraEngine::AssetPipeline::GetImportResultPath(absolutePath);
			if (std::filesystem::exists(resultPath))
			{
				std::ifstream in(resultPath);
				nlohmann::json importResult = nlohmann::json::object();
				in >> importResult;
				data["status"] = importResult.value("status", "Unknown");
				data["importResult"] = std::move(importResult);
			}
			else
			{
				data["importResult"] = nullptr;
			}
		}
		return BuildSuccess(requestId, "Queued asset import.", std::move(data));
	}

	nlohmann::json EditorAutomationService::HandleAssetPrepareDelete(const nlohmann::json& arguments, const std::string& requestId)
	{
		nlohmann::json validation = ValidateAssetPath(arguments, requestId);
		if (!validation.value("ok", false)) return validation;
		return BuildSuccess(requestId, "Prepared asset delete approval.", {{"path", validation["path"]}, {"requiresApproval", true}});
	}

	nlohmann::json EditorAutomationService::HandleExportValidate(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string profile = arguments.value("profile", "");
		if (profile.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "export.validate requires a profile name.");
		}
		const std::filesystem::path projectRoot = editorContext->projectManager->GetProjectRoot();
		std::optional<BuildProfile> buildProfile = FindBuildProfile(projectRoot, profile);
		if (!buildProfile)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Build profile was not found.", profile);
		}
		nlohmann::json data = SerializeExportValidation(ExportValidator{}.ValidateAll(*buildProfile, projectRoot));
		data["profile"] = buildProfile->name;
		data["profileId"] = buildProfile->id;
		return BuildSuccess(requestId, "Validated export profile.", std::move(data));
	}

	nlohmann::json EditorAutomationService::HandleExportStart(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string profile = arguments.value("profile", "");
		if (profile.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "export.start requires a profile name.");
		}
		nlohmann::json validation = HandleExportValidate(arguments, requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		ExportJob job;
		job.jobId = requestId + "-export";
		job.profile = validation["data"].value("profile", profile);
		job.state = validation["data"].value("canExport", false) ? "queued" : "blocked";
		job.progress = job.state == "queued" ? 0.05f : 0.0f;
		exportJobs.insert(exportJobs.begin(), job);
		if (exportJobs.size() > 16)
		{
			exportJobs.pop_back();
		}
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Export, requestId, "export.start", "Created export job " + job.jobId + ".");
		}
		return BuildSuccess(requestId, "Started export job.", {{"jobId", job.jobId}, {"profile", job.profile}, {"progress", job.progress}, {"state", job.state}});
	}

	nlohmann::json EditorAutomationService::HandleExportCancel(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string jobId = arguments.value("jobId", "");
		if (jobId.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "export.cancel requires a jobId.");
		}
		for (ExportJob& job : exportJobs)
		{
			if (job.jobId == jobId)
			{
				job.cancelRequested = true;
				job.state = "canceled";
				job.progress = 0.0f;
				if (activityCallback)
				{
					activityCallback(ActivityEventType::Export, requestId, "export.cancel", "Canceled export job " + jobId + ".");
				}
				return BuildSuccess(requestId, "Canceled export job.", {{"jobId", jobId}, {"state", job.state}});
			}
		}
		return BuildError(requestId, ErrorCategory::MissingTarget, "Export job was not found.", jobId);
	}

	nlohmann::json EditorAutomationService::HandleDiagnosticsGetSummary(const nlohmann::json& arguments, const std::string& requestId)
	{
		return BuildSuccess(requestId, "Read diagnostics summary.", BuildDiagnosticsSummary(arguments.value("includeDetails", false)));
	}

	void EditorAutomationService::MarkSceneMutated(ActivityEventType type, const std::string& requestId, const std::string& capability, const std::string& message) const
	{
		// Scene mutations are serialized through the editor-facing service so
		// ObjectManager callbacks, dirty-state tracking, and editor selection
		// all observe the change in the same order as user-driven edits.
		SceneManager::Instance().ForceMarkActiveSceneDirty();
		if (activityCallback)
		{
			activityCallback(type, requestId, capability, message);
		}
	}
}
