// EditorMcpBridge.h - Session, resource, approval, and activity state for MCP.
//
// The bridge owns editor-facing policy such as enablement, write mode, request
// ordering, and approval gates. Actual editor actions are delegated to
// EditorAutomationService so this class can stay focused on lifecycle state.

#pragma once

#include "EditorAutomationService.h"
#include "../Project/EditorMcpPreferences.h"

#include <atomic>
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
		// Runs only transport-originated calls at the later protected editor handoff.
		// It intentionally leaves assistant-request draining to Update(), so a late
		// acknowledgement cannot change the established assistant-command cadence.
		void PumpPendingTransportToolCallsAtProtectedFrame();
		// EditorSystem owns the scheduler-protected PostFrame boundary. This narrow
		// forwarder lets that owner advance durable recordings even when the MCP
		// listener is disabled, disconnected, or has no queued requests.
		void PumpAnimationRecordingsAtProtectedFrame();
		void PumpRendererValidationMutationsAtProtectedFrame();
		void SetMainEditor(MainEditorSubsystem* mainEditor);
		void SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter);

		void SetEnabled(bool enabled);
	#if defined(TERRAENGINE_EDITOR_TESTS)
		// Tests use this narrow synchronization seam to force enable/shutdown into
		// the ordering that previously regressed. Production builds omit it.
		void SetEnablePublicationHookForTests(std::function<void()> hook);
	#endif
		bool IsEnabled() const;
		void SetWriteMode(WriteMode mode);
		WriteMode GetWriteMode() const;
		bool SetListenPort(std::uint16_t port);
		std::uint16_t GetConfiguredListenPort() const;
		BridgeState GetState() const;

		bool TryOnLocalClientConnected(std::string clientName, std::string peerAddress);
		void OnLocalClientDisconnected(std::string reason);
		void RecordRejectedPeer(std::string peerAddress, std::string reason);

		std::string NextRequestId();
		void EnqueueRequest(std::string capability, nlohmann::json arguments);
		std::optional<AssistantRequest> PopNextQueuedRequest();
		std::vector<AssistantRequest> GetQueuedRequests() const;
		nlohmann::json CallTool(std::string_view name, const nlohmann::json& arguments);
		nlohmann::json SubmitToolCall(std::string name, nlohmann::json arguments, nlohmann::json transportRequestId = nullptr,
			const std::atomic<bool>* transportStopRequested = nullptr);
		bool CancelPendingToolCall(const nlohmann::json& transportRequestId);
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
			enum class State
			{
				Queued,
				Processing,
				Completed,
				Abandoned
			};

			std::string name;
			nlohmann::json arguments = nlohmann::json::object();
			// The JSON-RPC identity lets a cancellation arriving on another HTTP
			// connection find the exact queued call without exposing bridge IDs.
			nlohmann::json transportRequestId = nullptr;
			nlohmann::json result;
			State state = State::Queued;
			std::mutex mutex;
			std::condition_variable completedSignal;
		};

		bool RequiresWritePermission(const AutomationCapabilityDescriptor& capability) const;
		bool RequiresApproval(const AutomationCapabilityDescriptor& capability) const;
		nlohmann::json CallToolLocked(std::string_view name, const nlohmann::json& arguments);
		nlohmann::json BuildApprovalResponse(const AutomationCapabilityDescriptor& capability, const nlohmann::json& arguments, const std::string& requestId);
		ApprovalPrompt CreateApprovalPromptLocked(std::string requestId, std::string capability, std::string summary, nlohmann::json arguments, std::chrono::system_clock::time_point expiresAt);
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
		// Bridge policy/session containers are read by the loopback listener and
		// written by the editor UI. Keep them separate from activity and pending
		// call locks so service callbacks never require a recursive mutex.
		// Lifecycle policy is held across tool execution so disable/shutdown cannot
		// race an approved or ordinary handler past the security boundary.
		mutable std::mutex lifecycleMutex;
		mutable std::mutex stateMutex;
		mutable std::mutex activityMutex;
		mutable std::mutex pendingToolCallsMutex;
		bool acceptingToolCalls = false;
		std::vector<ApprovalPrompt> approvals;
		std::uint64_t nextActivitySequence = 1;
		std::uint64_t nextRequestSequence = 1;
		std::uint64_t nextApprovalSequence = 1;
		std::uint64_t nextSessionSequence = 1;
		std::size_t maxActivityRecords = 256;
		// Resolved approvals remain queryable for stateless loopback reconnects,
		// but the listener must never accumulate unbounded retained envelopes.
		std::size_t maxApprovalRecords = 256;
		std::string lastDiagnosticRequest = "never";
	#if defined(TERRAENGINE_EDITOR_TESTS)
		std::function<void()> enablePublicationHookForTests;
	#endif
	};
}
