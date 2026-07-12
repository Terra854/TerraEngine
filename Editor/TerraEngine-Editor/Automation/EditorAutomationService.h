// EditorAutomationService.h - Editor action facade used by the MCP bridge.
//
// The transport never edits scenes directly. It asks this service to validate a
// named capability and execute it against editor-owned systems, which keeps one
// documented choke point for permission checks, target lookup, and result JSON.

#pragma once

#include "EditorAutomationTypes.h"
#include "EditorAutomationIdempotencyStore.h"
#include "EditorAutomationJobService.h"
#include "EditorAutomationLogStore.h"
#include "EditorAutomationRegistry.h"
#include "../Subsystems/MainEditorSubsystem.h"

#include "Engine/Core/Object/Object.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct EditorContext;

namespace TerraEngine::Editor::Automation
{
	class EditorAutomationService
	{
	public:
		using BridgeToolCallback = std::function<nlohmann::json(const nlohmann::json&)>;
		using ActivityCallback = std::function<void(ActivityEventType, std::string, std::string, std::string)>;

		EditorAutomationService();

		void SetEditorContext(EditorContext* context);
		void SetMainEditor(MainEditorSubsystem* editor);
		void SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter);
		void SetBridgeToolCallback(BridgeToolCallback callback);
		void SetActivityCallback(ActivityCallback callback);

		bool RegisterCapability(const AssistantCapability& capability);
		void RegisterDefaultCapabilities();
		const std::vector<AssistantCapability>& GetCapabilities() const;
		const AssistantCapability* FindCapability(std::string_view name) const;
		EditorAutomationRegistry& GetRegistry();
		const EditorAutomationRegistry& GetRegistry() const;
		EditorAutomationJobService& GetJobService();
		const EditorAutomationJobService& GetJobService() const;
		EditorAutomationLogStore& GetLogStore();
		const EditorAutomationLogStore& GetLogStore() const;
		EditorAutomationIdempotencyStore& GetIdempotencyStore();
		const EditorAutomationIdempotencyStore& GetIdempotencyStore() const;
		[[nodiscard]] AutomationStateSnapshot BuildAutomationStateSnapshot() const;

		nlohmann::json HandleTool(std::string_view name, const nlohmann::json& arguments, WriteMode writeMode, const std::string& requestId);
		nlohmann::json BuildSuccess(std::string_view requestId, std::string_view message, nlohmann::json data = nlohmann::json::object()) const;
		nlohmann::json BuildError(std::string_view requestId, ErrorCategory category, std::string_view message, std::string_view target = {}) const;

		nlohmann::json BuildCapabilityList() const;
		nlohmann::json BuildProjectState() const;
		nlohmann::json BuildSceneSummary() const;
		nlohmann::json BuildDiagnosticsSummary(bool includeDetails) const;
		nlohmann::json BuildCaptureAvailability() const;
		McpCaptureLimits GetMcpCaptureLimits() const;
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

		struct VirtualGeometrySettingsRequest
		{
			TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
			bool usePerMeshOverride = false;
			bool rebuildAfterApply = false;
			std::vector<float> targetRatios;
		};

		struct VirtualGeometrySettingsResponse
		{
			bool accepted = false;
			bool meshAsset = false;
			std::string settingsSource = "ProjectDefaults";
			std::vector<std::string> validationErrors;
		};

		struct VirtualGeometryInspectResponse
		{
			bool meshAsset = false;
			std::uint32_t requestedTargets = 0u;
			std::uint32_t generatedLevels = 0u;
			std::uint32_t skippedDetails = 0u;
			std::uint32_t clusters = 0u;
			std::uint32_t pages = 0u;
			std::vector<std::string> warnings;
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
		nlohmann::json HandleVirtualGeometrySettingsGet(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleVirtualGeometrySettingsSet(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleVirtualGeometrySettingsReset(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleVirtualGeometryRebuild(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleVirtualGeometryInspect(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleAssetPrepareDelete(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleExportValidate(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleExportStart(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleExportCancel(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleGameViewCapture(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleCameraObjectCapture(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleCustomCameraCapture(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleRendererVirtualGeometryStats(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json HandleDiagnosticsGetSummary(const nlohmann::json& arguments, const std::string& requestId);
		nlohmann::json ExecuteProviderHandler(std::uint64_t operation, std::string_view operationName, const nlohmann::json& arguments, WriteMode writeMode, const std::string& requestId);
		nlohmann::json ValidateCaptureArguments(const nlohmann::json& arguments, const std::string& requestId) const;
		ErrorCategory MapCaptureFailureCategory(std::string_view category) const;
		nlohmann::json ValidateProjectRelativePath(const nlohmann::json& arguments, std::string_view key, const std::string& requestId) const;
		nlohmann::json ValidateAssetPath(const nlohmann::json& arguments, const std::string& requestId) const;
		void MarkSceneMutated(ActivityEventType type, const std::string& requestId, const std::string& capability, const std::string& message) const;

		EditorContext* editorContext = nullptr;
		MainEditorSubsystem* mainEditor = nullptr;
		BridgeToolCallback bridgeToolCallback;
		ActivityCallback activityCallback;
		std::function<std::optional<entt::entity>()> getSelectedEntity;
		std::function<void(std::optional<entt::entity>)> setSelectedEntity;
		std::vector<AssistantCapability> capabilities;
		std::vector<ExportJob> exportJobs;
		std::unordered_map<std::string, std::size_t> capabilityIndexByName;
		// Provider names are bound to stable operation IDs once during registry
		// construction. Retaining the reverse map detects the otherwise extremely
		// unlikely case of two provider names hashing to the same ID.
		std::unordered_map<std::uint64_t, std::string> boundProviderOperationNames;
		EditorAutomationRegistry capabilityRegistry;
		EditorAutomationJobService jobService;
		EditorAutomationLogStore logStore;
		EditorAutomationIdempotencyStore idempotencyStore;
		// Script compilation is single-flight in the owning subsystem. Retaining
		// its durable job ID lets polling mirror native progress and diagnostics.
		std::string activeScriptCompilationJobId;
		// Profiler capture persistence completes asynchronously after stop. The
		// baseline IDs let jobs.get identify the new project-contained artifact
		// without exposing or accepting an arbitrary filesystem path.
		std::string activeProfilerCaptureJobId;
		std::unordered_set<std::string> profilerArtifactIdsBeforeStop;
		std::string activeProfilerLoadJobId;
		std::string activeProfilerLoadArtifactId;
		std::uint64_t nextTransactionId = 1u;
		PrefabAuthoringService prefabAuthoringService;
	};
}
