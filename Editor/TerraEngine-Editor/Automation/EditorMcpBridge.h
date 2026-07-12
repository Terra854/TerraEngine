// EditorMcpBridge.h - Session, resource, approval, and activity state for MCP.
//
// The bridge owns editor-facing policy such as enablement, write mode, request
// ordering, and approval gates. Actual editor actions are delegated to
// EditorAutomationService so this class can stay focused on lifecycle state.

#pragma once

#include "EditorAutomationService.h"
#include "../Project/EditorMcpPreferences.h"

#include <deque>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct EditorContext;
class MainEditorSubsystem;

namespace TerraEngine::Editor::Automation
{
	class EditorMcpTransport;

	class EditorMcpBridge
	{
	public:
		EditorMcpBridge();
		~EditorMcpBridge();

		void Initialize(EditorContext* context, MainEditorSubsystem* mainEditor = nullptr);
		void Shutdown();
		void Update(float deltaTime);
		void SetMainEditor(MainEditorSubsystem* mainEditor);
		void SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter);

		void SetEnabled(bool enabled);
		bool IsEnabled() const;
		void SetWriteMode(WriteMode mode);
		WriteMode GetWriteMode() const;
		bool SetListenPort(std::uint16_t port);
		std::uint16_t GetConfiguredListenPort() const;
		BridgeState GetState() const;

		void OnLocalClientConnected(std::string clientName, std::string peerAddress);
		void OnLocalClientDisconnected(std::string reason);
		void RecordRejectedPeer(std::string peerAddress, std::string reason);

		std::string NextRequestId();
		void EnqueueRequest(std::string capability, nlohmann::json arguments);
		std::optional<AssistantRequest> PopNextQueuedRequest();
		std::vector<AssistantRequest> GetQueuedRequests() const;
		nlohmann::json CallTool(std::string_view name, const nlohmann::json& arguments);
		nlohmann::json SubmitToolCall(std::string name, nlohmann::json arguments);
		nlohmann::json ReadResource(std::string_view uri) const;
		nlohmann::json ListResources() const;
		std::uint16_t GetListenPort() const;
		std::string GetListenEndpoint() const;
		std::string GetTransportLastError() const;

		void RecordActivity(ActivityEventType type, std::string requestId, std::string capability, std::string message, std::string outcome);
		std::vector<ActivityRecord> GetRecentActivity(std::size_t limit = 64) const;
		nlohmann::json BuildStatusJson() const;
		nlohmann::json BuildCapabilityListJson() const;
		nlohmann::json BuildCaptureAvailabilityJson() const;

		ApprovalPrompt CreateApprovalPrompt(std::string requestId, std::string capability, std::string summary, nlohmann::json arguments, std::chrono::system_clock::time_point expiresAt = {});
		bool ResolveApproval(std::string_view approvalId, bool approved);
		bool CancelApproval(std::string_view approvalId);
		std::vector<ApprovalPrompt> GetPendingApprovals() const;
		bool CancelQueuedRequest(std::string_view requestId);

	private:
		struct PendingToolCall
		{
			std::string name;
			nlohmann::json arguments = nlohmann::json::object();
			nlohmann::json result;
			bool completed = false;
			std::mutex mutex;
			std::condition_variable completedSignal;
		};

		bool RequiresWritePermission(const AutomationCapabilityDescriptor& capability) const;
		bool RequiresApproval(const AutomationCapabilityDescriptor& capability) const;
		nlohmann::json BuildApprovalResponse(const AutomationCapabilityDescriptor& capability, const nlohmann::json& arguments, const std::string& requestId);
		void PersistPreferences() const;
		void ProcessPendingToolCalls();
		void ExpirePendingApprovals();

		EditorContext* editorContext = nullptr;
		MainEditorSubsystem* mainEditor = nullptr;
		std::unique_ptr<EditorMcpTransport> transport;
		EditorAutomationService service;
		BridgeSession session;
		WriteMode writeMode = WriteMode::ReadOnly;
		std::uint16_t configuredListenPort = TerraEngine::Editor::Preferences::kDefaultEditorMcpListenPort;
		std::string listenPortConfigurationError;
		std::deque<ActivityRecord> activityHistory;
		std::deque<AssistantRequest> requestQueue;
		std::deque<std::shared_ptr<PendingToolCall>> pendingToolCalls;
		mutable std::mutex pendingToolCallsMutex;
		std::vector<ApprovalPrompt> approvals;
		std::uint64_t nextActivitySequence = 1;
		std::uint64_t nextRequestSequence = 1;
		std::uint64_t nextApprovalSequence = 1;
		std::size_t maxActivityRecords = 256;
		std::string lastDiagnosticRequest = "never";
	};
}
