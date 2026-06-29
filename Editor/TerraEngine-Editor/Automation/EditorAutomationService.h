// EditorAutomationService.h - Editor action facade used by the MCP bridge.
//
// The transport never edits scenes directly. It asks this service to validate a
// named capability and execute it against editor-owned systems, which keeps one
// documented choke point for permission checks, target lookup, and result JSON.

#pragma once

#include "EditorAutomationTypes.h"

#include "Engine/Core/Object/Object.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct EditorContext;
class MainEditorSubsystem;

namespace TerraEngine::Editor::Automation
{
	class EditorAutomationService
	{
	public:
		using BridgeToolCallback = std::function<nlohmann::json(const nlohmann::json&)>;
		using ActivityCallback = std::function<void(ActivityEventType, std::string, std::string, std::string)>;

		EditorAutomationService();

		void SetEditorContext(EditorContext* context);
		void SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter);
		void SetBridgeToolCallback(BridgeToolCallback callback);
		void SetActivityCallback(ActivityCallback callback);

		bool RegisterCapability(const AssistantCapability& capability);
		void RegisterDefaultCapabilities();
		const std::vector<AssistantCapability>& GetCapabilities() const;
		const AssistantCapability* FindCapability(std::string_view name) const;

		nlohmann::json HandleTool(std::string_view name, const nlohmann::json& arguments, WriteMode writeMode, const std::string& requestId);
		nlohmann::json BuildSuccess(std::string_view requestId, std::string_view message, nlohmann::json data = nlohmann::json::object()) const;
		nlohmann::json BuildError(std::string_view requestId, ErrorCategory category, std::string_view message, std::string_view target = {}) const;

		nlohmann::json BuildCapabilityList() const;
		nlohmann::json BuildProjectState() const;
		nlohmann::json BuildSceneSummary() const;
		nlohmann::json BuildDiagnosticsSummary(bool includeDetails) const;
		nlohmann::json BuildActivityList(const std::vector<ActivityRecord>& records) const;
		std::vector<ExportJob> GetExportJobs() const;

		struct ReflectionMutationPreflight
		{
			bool valid = false;
			std::string classification;
			std::string safeSubwork;
			std::string protectedCommit;
			std::string message;
		};

		[[nodiscard]] ReflectionMutationPreflight PreflightReflectionMutation(const nlohmann::json& arguments) const;

	private:
		EditorTarget ResolveTarget(const nlohmann::json& value) const;
		nlohmann::json SerializeObjectSummary(entt::entity object, bool includeComponents) const;
		nlohmann::json HandleSceneListObjects(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleSceneSave(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleObjectCreate(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleObjectDelete(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleObjectSelect(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleObjectSetMetadata(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleObjectSetParent(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleComponentAdd(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleComponentRemove(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleComponentGet(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleComponentSetProperty(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleAssetList(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleAssetImport(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleVirtualGeometryRebuild(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleVirtualGeometryInspect(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleAssetPrepareDelete(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleExportValidate(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleExportStart(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleExportCancel(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleDiagnosticsGetSummary(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json ValidateProjectRelativePath(const nlohmann::json& arguments, std::string_view key, const std::string& requestId) const;
		nlohmann::json ValidateAssetPath(const nlohmann::json& arguments, const std::string& requestId) const;
		void MarkSceneMutated(ActivityEventType type, const std::string& requestId, const std::string& capability, const std::string& message) const;

		EditorContext* editorContext = nullptr;
		BridgeToolCallback bridgeToolCallback;
		ActivityCallback activityCallback;
		std::function<std::optional<entt::entity>()> getSelectedEntity;
		std::function<void(std::optional<entt::entity>)> setSelectedEntity;
		std::vector<AssistantCapability> capabilities;
		std::vector<ExportJob> exportJobs;
		std::unordered_map<std::string, std::size_t> capabilityIndexByName;
	};
}
