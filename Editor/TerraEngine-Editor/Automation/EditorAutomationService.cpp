// EditorAutomationService.cpp - Contract implementation for editor automation.
//
// This file keeps assistant-facing tool behavior in one place. Transport code
// supplies JSON, bridge code supplies policy, and this service performs the
// editor validation/action work against TerraEngine's existing systems.

#include "EditorAutomationService.h"

#include "../Assets/AssetDatabase.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "../../Export/BuildProfile.h"
#include "../../Export/ExportValidator.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
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

		add("bridge.set_enabled", "bridge", MutationMode::EditorSelection, false, false, false, "enabled: bool, writeMode?: read_only|writes_enabled");
		add("bridge.set_write_mode", "bridge", MutationMode::EditorSelection, false, false, false, "writeMode: read_only|writes_enabled");
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
		add("asset.import", "asset", MutationMode::AssetMutation, true, false, false, "path: project-relative asset path, force?: bool");
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
		const bool bridgeConfigurationTool = toolName == "bridge.set_enabled" || toolName == "bridge.set_write_mode";
		if (capability->mutatesState && writeMode != WriteMode::WritesEnabled && !bridgeConfigurationTool)
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

		if ((toolName == "bridge.set_enabled" || toolName == "bridge.set_write_mode" || toolName == "activity.list") && bridgeToolCallback)
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
		if (getSelectedEntity && getSelectedEntity().has_value())
		{
			if (auto object = ObjectManager::instance().getObject(*getSelectedEntity()))
			{
				data["selectedObject"] = SerializeObjectSummary(*object, false);
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
			data["roots"].push_back(SerializeObjectSummary(object, true));
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
		data["surfaces"] = nlohmann::json::array({
			{{"surface", "render"}, {"state", "available"}, {"details", includeDetails ? nlohmann::json{{"note", "Render diagnostics are available through the profiler/resource windows."}} : nlohmann::json::object()}},
			{{"surface", "resources"}, {"state", assetJobCount > 0 ? "compiling" : "available"}, {"details", nlohmann::json{{"assetCompileJobs", assetJobCount}}}},
			{{"surface", "streaming"}, {"state", "available"}, {"details", nlohmann::json::object()}},
			{{"surface", "scheduler"}, {"state", "available"}, {"details", nlohmann::json::object()}},
			{{"surface", "scripting"}, {"state", ScriptingSystem::Instance().IsRunning() ? "running" : "idle"}, {"details", nlohmann::json{{"coordinatorState", editorContext ? editorContext->scripting.coordinatorState : "Idle"}}}},
			{{"surface", "export"}, {"state", exportJobs.empty() ? "idle" : exportJobs.front().state}, {"details", nlohmann::json{{"jobs", exportJobJson}}}}
		});
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

	EditorTarget EditorAutomationService::ResolveTarget(const nlohmann::json& value) const
	{
		EditorTarget target;
		if (value.is_number_unsigned() || value.is_number_integer())
		{
			const auto entity = static_cast<entt::entity>(value.get<std::uint32_t>());
			target.input = EntityToString(entity);
			if (auto object = ObjectManager::instance().getObject(entity))
			{
				target.entity = object->EntityId();
				target.displayName = object->getName();
				target.uuid = TerraEngine::ToHexString(object->getUUID());
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
			if (auto object = ObjectManager::instance().getObject(*parsed))
			{
				target.entity = object->EntityId();
				target.displayName = object->getName();
				target.uuid = TerraEngine::ToHexString(object->getUUID());
				target.resolved = true;
				return target;
			}
		}
		if (auto object = ObjectManager::instance().FindObjectByName(target.input))
		{
			target.entity = object->EntityId();
			target.displayName = object->getName();
			target.uuid = TerraEngine::ToHexString(object->getUUID());
			target.resolved = true;
		}
		return target;
	}

	nlohmann::json EditorAutomationService::SerializeObjectSummary(Object object, bool includeComponents) const
	{
		nlohmann::json data;
		data["entity"] = static_cast<std::uint32_t>(object.EntityId());
		data["uuid"] = TerraEngine::ToHexString(object.getUUID());
		data["name"] = object.getName();
		data["tag"] = object.getTag();
		data["layer"] = object.getLayer();
		data["active"] = object.isActive();
		data["static"] = object.isStatic();
		data["parent"] = ObjectManager::instance().getParentEntity(object.EntityId()) == entt::null ? nullptr : nlohmann::json(static_cast<std::uint32_t>(ObjectManager::instance().getParentEntity(object.EntityId())));
		if (includeComponents)
		{
			nlohmann::json components = nlohmann::json::array();
			std::lock_guard lock(ComponentAccessRegistryMutex());
			for (const auto& [name, access] : ComponentAccessRegistry())
			{
				Object mutableObject = object;
				if (access.hasComponent && access.hasComponent(mutableObject))
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
				|| ContainsCaseInsensitive(object.getName(), search)
				|| ContainsCaseInsensitive(object.getTag(), search)
				|| ContainsCaseInsensitive(object.getLayer(), search);
			if (matches)
			{
				objects.push_back(SerializeObjectSummary(object, includeComponents));
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
		if (arguments.contains("parent") && !arguments["parent"].is_null())
		{
			EditorTarget parent = ResolveTarget(arguments["parent"]);
			if (!parent.resolved)
			{
				ObjectManager::instance().removeObject(object);
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parent.input);
			}
			ObjectManager::instance().setParent(object, ObjectManager::instance().getObject(*parent.entity));
		}
		if (arguments.value("select", false) && setSelectedEntity)
		{
			setSelectedEntity(object.EntityId());
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(object.EntityId()));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.create", "Created object " + object.getName() + ".");
		return BuildSuccess(requestId, "Created object.", {{"object", SerializeObjectSummary(object, true)}});
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
		ObjectManager::instance().removeObject(*ObjectManager::instance().getObject(*target.entity));
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
		Object object = *ObjectManager::instance().getObject(*target.entity);
		const nlohmann::json oldMetadata = {
			{"name", object.getName()},
			{"tag", object.getTag()},
			{"layer", object.getLayer()},
			{"active", object.isActive()},
			{"static", object.isStatic()}
		};
		if (arguments.contains("name") && arguments["name"].is_string()) object.setName(arguments["name"].get<std::string>());
		if (arguments.contains("tag") && arguments["tag"].is_string()) object.setTag(arguments["tag"].get<std::string>());
		if (arguments.contains("layer") && arguments["layer"].is_string()) object.setLayer(arguments["layer"].get<std::string>());
		if (arguments.contains("active") && arguments["active"].is_boolean()) object.setActive(arguments["active"].get<bool>());
		if (arguments.contains("static") && arguments["static"].is_boolean()) object.setStatic(arguments["static"].get<bool>());
		const nlohmann::json newMetadata = {
			{"name", object.getName()},
			{"tag", object.getTag()},
			{"layer", object.getLayer()},
			{"active", object.isActive()},
			{"static", object.isStatic()}
		};
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetObjectMetadataCommand>(object.EntityId(), oldMetadata, newMetadata));
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
		Object child = *ObjectManager::instance().getObject(*childTarget.entity);
		std::optional<TerraEngine::UUID> oldParentUuid;
		if (const entt::entity oldParent = ObjectManager::instance().getParentEntity(child.EntityId()); oldParent != entt::null)
		{
			oldParentUuid = ObjectManager::instance().getUUID(oldParent);
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
			newParentUuid = parent->getUUID();
		}
		if (!ObjectManager::instance().setParent(child, parent))
		{
			return BuildError(requestId, ErrorCategory::Conflict, "Parent change would create an invalid hierarchy.");
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetParentCommand>(child.EntityId(), oldParentUuid, newParentUuid));
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
		Object object = *ObjectManager::instance().getObject(*target.entity);
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end())
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Component is not registered for automation.", component);
		}
		it->second.addComponent(object);
		ObjectManager::instance().notifyObjectStructureChanged(object);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<AddComponentCommand>(object.EntityId(), component));
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
		Object object = *ObjectManager::instance().getObject(*target.entity);
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.hasComponent(object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		// RemoveComponentCommand captures the existing component payload for
		// undo, so construct it while the component is still attached.
		auto command = std::make_unique<RemoveComponentCommand>(object.EntityId(), component);
		it->second.removeComponent(object);
		ObjectManager::instance().notifyObjectStructureChanged(object);
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
		Object object = *ObjectManager::instance().getObject(*target.entity);
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.hasComponent(object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		return BuildSuccess(requestId, "Read component.", {{"component", component}, {"properties", it->second.serialize(object)}});
	}

	nlohmann::json EditorAutomationService::HandleComponentSetProperty(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		const std::string property = arguments.value("property", "");
		if (!target.resolved || component.empty() || property.empty() || !arguments.contains("value"))
		{
			return BuildError(requestId, ErrorCategory::Validation, "component.set_property requires object, component, property, and value.");
		}
		Object object = *ObjectManager::instance().getObject(*target.entity);
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.hasComponent(object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		const nlohmann::json oldPayload = it->second.serialize(object);
		it->second.deserialize(object, nlohmann::json{{property, arguments["value"]}});
		const nlohmann::json newPayload = it->second.serialize(object);
		ObjectManager::instance().notifyObjectStructureChanged(object);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetComponentPayloadCommand>(object.EntityId(), component, oldPayload, newPayload));
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
		editorContext->assetDatabase->ImportAsset(absolutePath, arguments.value("force", false));
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "asset.import", "Queued asset import.");
		}
		return BuildSuccess(requestId, "Queued asset import.", {{"path", validation["path"]}, {"force", arguments.value("force", false)}, {"status", "queued"}});
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
