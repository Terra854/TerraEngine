// EditorMcpBridge.cpp - Policy/state layer for TerraEngine editor MCP access.
//
// Mutating editor work is queued through this bridge and executed by the
// automation service from editor-owned update flow. That serialization is
// deliberate: scene state, undo/redo, dirty flags, and selection are all
// editor-owned concepts, so concurrent transport callbacks must not mutate
// them directly.

#include "EditorMcpBridge.h"

#include "EditorMcpTransport.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"

#include <algorithm>
#include <chrono>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		bool IsCaptureTool(std::string_view name)
		{
			return name == "game_view.capture" || name == "camera.capture_object" || name == "camera.capture_custom";
		}

		const char* JobStateJson(AutomationJobState state)
		{
			switch (state)
			{
			case AutomationJobState::Queued: return "queued";
			case AutomationJobState::Preparing: return "preparing";
			case AutomationJobState::Running: return "running";
			case AutomationJobState::CancelRequested: return "cancel_requested";
			case AutomationJobState::Succeeded: return "succeeded";
			case AutomationJobState::Failed: return "failed";
			case AutomationJobState::Canceled: return "canceled";
			}
			return "failed";
		}
	}

	EditorMcpBridge::EditorMcpBridge()
	{
		session.state = BridgeState::Disabled;
		service.SetBridgeToolCallback([this](const nlohmann::json& request) {
			const std::string name = request.value("name", "");
			const nlohmann::json arguments = request.value("arguments", nlohmann::json::object());
			const std::string requestId = request.value("requestId", "");
			if (name == "activity.list")
			{
				const std::size_t limit = arguments.value("limit", 64u);
				return service.BuildSuccess(requestId, "Listed recent activity.", service.BuildActivityList(GetRecentActivity(limit)));
			}
			return service.BuildError(requestId, ErrorCategory::UnsupportedCapability, "Bridge callback received an unknown tool.");
		});
		service.SetActivityCallback([this](ActivityEventType type, std::string requestId, std::string capability, std::string message) {
			RecordActivity(type, std::move(requestId), std::move(capability), std::move(message), "completed");
		});
	}

	EditorMcpBridge::~EditorMcpBridge()
	{
		Shutdown();
	}

	void EditorMcpBridge::Initialize(EditorContext* context, MainEditorSubsystem* editor)
	{
		editorContext = context;
		mainEditor = editor;
		service.SetEditorContext(context);
		service.SetMainEditor(editor);
		transport = std::make_unique<EditorMcpTransport>(*this);
		session.sessionId = "terraengine-editor-local";
		session.clientName.clear();
		session.connectedClientCount = 0;
		session.state = BridgeState::Disabled;
		writeMode = WriteMode::ReadOnly;
		RecordActivity(ActivityEventType::Configuration, {}, "bridge.initialize", "Initialized editor MCP bridge.", "completed");
	}

	void EditorMcpBridge::Shutdown()
	{
		session.state = BridgeState::Stopping;
		// Draining requests on shutdown prevents half-applied assistant work from
		// running after editor subsystems have begun tearing down.
		for (AssistantRequest& request : requestQueue)
		{
			request.status = RequestStatus::Canceled;
			request.canceled = true;
		}
		requestQueue.clear();
		if (transport)
		{
			transport->StopListening();
		}
		{
			std::lock_guard lock(pendingToolCallsMutex);
			for (const std::shared_ptr<PendingToolCall>& call : pendingToolCalls)
			{
				std::lock_guard callLock(call->mutex);
				call->result = service.BuildError(NextRequestId(), ErrorCategory::Canceled, "Bridge shut down before the request could run.");
				call->completed = true;
				call->completedSignal.notify_all();
			}
			pendingToolCalls.clear();
		}
		approvals.clear();
		session.connectedClientCount = 0;
		session.state = BridgeState::Stopped;
		RecordActivity(ActivityEventType::Connection, {}, "bridge.shutdown", "Stopped editor MCP bridge.", "canceled");
	}

	void EditorMcpBridge::Update(float)
	{
		ExpirePendingApprovals();
		ProcessPendingToolCalls();
		// The queue is FIFO so repeated assistant commands produce deterministic
		// scene/editor state even when a client sends overlapping requests.
		while (!requestQueue.empty())
		{
			AssistantRequest request = requestQueue.front();
			requestQueue.pop_front();
			if (request.canceled)
			{
				RecordActivity(ActivityEventType::Request, request.requestId, request.capability, "Skipped canceled request.", "canceled");
				continue;
			}
			(void)CallTool(request.capability, request.arguments);
		}
	}

	void EditorMcpBridge::SetMainEditor(MainEditorSubsystem* editor)
	{
		mainEditor = editor;
		service.SetMainEditor(editor);
	}

	void EditorMcpBridge::SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter)
	{
		service.SetSelectionCallbacks(std::move(getter), std::move(setter));
	}

	void EditorMcpBridge::SetEnabled(bool enabled)
	{
		if (enabled)
		{
			listenPortConfigurationError.clear();
			session.state = session.connectedClientCount > 0 ? BridgeState::Connected : BridgeState::Listening;
			if (transport && !transport->IsListening())
			{
				// Always bind the saved port. Falling back to port zero would create a
				// random endpoint and break external MCP client configuration.
				if (!transport->StartListening(configuredListenPort))
				{
					session.state = BridgeState::Disabled;
					RecordActivity(ActivityEventType::Rejection, {}, "transport.listen", transport->GetLastError(), "failed");
					return;
				}
			}
			RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_enabled", "Enabled local MCP bridge.", "completed");
			PersistPreferences();
		}
		else
		{
			if (transport)
			{
				transport->StopListening();
			}
			session.connectedClientCount = 0;
			session.clientName.clear();
			session.state = BridgeState::Disabled;
			RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_enabled", "Disabled local MCP bridge.", "completed");
			PersistPreferences();
		}
	}

	bool EditorMcpBridge::IsEnabled() const
	{
		return session.state == BridgeState::Listening || session.state == BridgeState::Connected;
	}

	void EditorMcpBridge::SetWriteMode(WriteMode mode)
	{
		writeMode = mode;
		RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_write_mode", std::string("Changed write mode to ") + ToString(mode) + ".", "completed");
		PersistPreferences();
	}

	WriteMode EditorMcpBridge::GetWriteMode() const
	{
		return writeMode;
	}

	bool EditorMcpBridge::SetListenPort(std::uint16_t port)
	{
		if (!TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(port))
		{
			listenPortConfigurationError = "MCP listen port must be between 1024 and 65535.";
			return false;
		}
		if (IsEnabled())
		{
			// Rebinding would disconnect the active client. Requiring an explicit
			// disable makes that disruption visible and intentional in Preferences.
			listenPortConfigurationError = "Disable the MCP bridge before changing its listen port.";
			return false;
		}

		configuredListenPort = port;
		listenPortConfigurationError.clear();
		RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_listen_port", "Changed MCP listen port to " + std::to_string(port) + ".", "completed");
		PersistPreferences();
		return true;
	}

	std::uint16_t EditorMcpBridge::GetConfiguredListenPort() const
	{
		return configuredListenPort;
	}

	BridgeState EditorMcpBridge::GetState() const
	{
		return session.state;
	}

	void EditorMcpBridge::OnLocalClientConnected(std::string clientName, std::string peerAddress)
	{
		session.clientName = std::move(clientName);
		session.peerAddress = std::move(peerAddress);
		session.connectedClientCount = 1;
		session.state = BridgeState::Connected;
		RecordActivity(ActivityEventType::Connection, {}, "transport.connect", "Accepted local MCP client.", "completed");
	}

	void EditorMcpBridge::OnLocalClientDisconnected(std::string reason)
	{
		session.connectedClientCount = 0;
		session.clientName.clear();
		session.state = IsEnabled() ? BridgeState::Listening : BridgeState::Disabled;
		RecordActivity(ActivityEventType::Connection, {}, "transport.disconnect", std::move(reason), "completed");
	}

	void EditorMcpBridge::RecordRejectedPeer(std::string peerAddress, std::string reason)
	{
		RecordActivity(ActivityEventType::Rejection, {}, "transport.reject", "Rejected " + peerAddress + ": " + reason, "rejected");
	}

	std::string EditorMcpBridge::NextRequestId()
	{
		return "mcp-" + std::to_string(nextRequestSequence++);
	}

	void EditorMcpBridge::EnqueueRequest(std::string capability, nlohmann::json arguments)
	{
		AssistantRequest request;
		request.requestId = NextRequestId();
		request.capability = std::move(capability);
		request.arguments = std::move(arguments);
		requestQueue.push_back(request);
		RecordActivity(ActivityEventType::Request, request.requestId, request.capability, "Queued request.", "queued");
	}

	std::optional<AssistantRequest> EditorMcpBridge::PopNextQueuedRequest()
	{
		if (requestQueue.empty())
		{
			return std::nullopt;
		}
		AssistantRequest request = requestQueue.front();
		requestQueue.pop_front();
		return request;
	}

	std::vector<AssistantRequest> EditorMcpBridge::GetQueuedRequests() const
	{
		return {requestQueue.begin(), requestQueue.end()};
	}

	nlohmann::json EditorMcpBridge::CallTool(std::string_view name, const nlohmann::json& arguments)
	{
		if (!IsEnabled())
		{
			return service.BuildError(NextRequestId(), ErrorCategory::Permission, "The MCP bridge is disabled.");
		}

		const std::string requestId = NextRequestId();
		// Policy follows the same provider-owned descriptor used by discovery and
		// dispatch. This also covers component/subsystem declarations registered
		// after startup without adding them to a legacy bridge capability list.
		const auto capability = service.GetRegistry().Find(name, 1, service.BuildAutomationStateSnapshot());
		if (capability && RequiresWritePermission(capability->descriptor) && writeMode != WriteMode::WritesEnabled)
		{
			RecordActivity(ActivityEventType::Rejection, requestId, std::string(name), "Blocked write while bridge is read-only.", "rejected");
			return service.BuildError(requestId, ErrorCategory::Permission, "The bridge is in read-only mode.");
		}
		if (capability && RequiresApproval(capability->descriptor))
		{
			return BuildApprovalResponse(capability->descriptor, arguments, requestId);
		}

		const bool captureTool = IsCaptureTool(name);
		if (captureTool)
		{
			RecordActivity(ActivityEventType::Capture, requestId, std::string(name), "Accepted capture request.", "accepted");
		}
		if (name == "diagnostics.get_summary")
		{
			lastDiagnosticRequest = arguments.dump();
		}
		nlohmann::json result = service.HandleTool(name, arguments, writeMode, requestId);
		if (captureTool)
		{
			RecordActivity(ActivityEventType::Capture, requestId, std::string(name), result.value("message", ""), result.value("ok", false) ? "completed" : "rejected");
		}
		else
		{
			RecordActivity(result.value("ok", false) ? ActivityEventType::Request : ActivityEventType::Rejection, requestId, std::string(name), result.value("message", ""), result.value("status", "completed"));
		}
		return result;
	}

	nlohmann::json EditorMcpBridge::SubmitToolCall(std::string name, nlohmann::json arguments)
	{
		auto call = std::make_shared<PendingToolCall>();
		call->name = std::move(name);
		call->arguments = std::move(arguments);
		{
			std::lock_guard lock(pendingToolCallsMutex);
			pendingToolCalls.push_back(call);
		}

		std::unique_lock callLock(call->mutex);
		const bool completed = call->completedSignal.wait_for(callLock, std::chrono::seconds(30), [&call]() {
			return call->completed;
		});
		if (!completed)
		{
			const std::string requestId = NextRequestId();
			if (IsCaptureTool(call->name))
			{
				RecordActivity(ActivityEventType::Capture, requestId, call->name, "Timed out waiting for the editor update to process the capture request.", "canceled");
			}
			return service.BuildError(requestId, ErrorCategory::Canceled, "Timed out waiting for the editor update to process the request.");
		}
		return call->result;
	}

	nlohmann::json EditorMcpBridge::ReadResource(std::string_view uri) const
	{
		const std::string resource(uri);
		if (resource == "terraengine://bridge/status") return BuildStatusJson();
		if (resource == "terraengine://bridge/capabilities") return BuildCapabilityListJson();
		if (resource == "terraengine://project/state") return service.BuildProjectState();
		if (resource == "terraengine://scene/summary") return service.BuildSceneSummary();
		if (resource == "terraengine://diagnostics/summary") return service.BuildDiagnosticsSummary(false);
		if (resource == "terraengine://renderer/recovery")
		{
			nlohmann::json summary = service.BuildDiagnosticsSummary(true);
			return {
				{"ok", true},
				{"rendererRecovery", summary.value("rendererRecovery", nlohmann::json::object())},
				{"safeSaveTool", "renderer.recovery.safe_save_project"}
			};
		}
		if (resource == "terraengine://capture/availability") return BuildCaptureAvailabilityJson();
		if (resource == "terraengine://activity/recent") return service.BuildActivityList(GetRecentActivity());
		if (resource == "terraengine://jobs/recent")
		{
			nlohmann::json jobs = nlohmann::json::array();
			for (const AutomationJobSnapshot& job : service.GetJobService().List(100u))
			{
				jobs.push_back({{"jobId", job.jobId}, {"kind", job.kind}, {"capability", job.capability}, {"state", JobStateJson(job.state)}, {"progress", job.progress}, {"stage", job.stage}, {"cancellable", job.cancellable}, {"cancelRequested", job.cancelRequested}, {"correlationId", job.correlationId}, {"artifacts", job.artifacts.size()}, {"result", job.result}});
			}
			return {{"ok", true}, {"jobs", jobs}, {"oldestRetainedSequence", service.GetJobService().OldestRetainedSequence()}};
		}
		if (resource == "terraengine://logs/recent")
		{
			StructuredLogQuery query;
			query.pageSize = 100u;
			const StructuredLogPage page = service.GetLogStore().Query(query);
			nlohmann::json records = nlohmann::json::array();
			for (const StructuredLogRecord& record : page.records) records.push_back({{"sequence", record.sequence}, {"category", record.category}, {"source", record.source}, {"message", record.message}, {"correlationId", record.correlationId}, {"requestId", record.requestId}, {"jobId", record.jobId}});
			return {{"ok", page.valid}, {"records", records}, {"nextCursor", page.nextCursor}, {"droppedBeforeCursor", page.droppedBeforeCursor}};
		}
		if (resource == "terraengine://approvals/pending")
		{
			nlohmann::json pending = nlohmann::json::array();
			for (const ApprovalPrompt& approval : GetPendingApprovals()) pending.push_back({{"approvalId", approval.approvalId}, {"requestId", approval.requestId}, {"capability", approval.capability}, {"summary", approval.summary}, {"state", approval.resolved ? (approval.approved ? "approved" : "rejected") : "pending"}});
			return {{"ok", true}, {"approvals", pending}};
		}
		return {{"ok", false}, {"error", {{"category", ToString(ErrorCategory::UnsupportedCapability)}, {"details", "Unknown resource URI."}}}};
	}

	nlohmann::json EditorMcpBridge::ListResources() const
	{
		return nlohmann::json::array({
			{{"uri", "terraengine://bridge/status"}, {"name", "Bridge Status"}},
			{{"uri", "terraengine://bridge/capabilities"}, {"name", "Bridge Capabilities"}},
			{{"uri", "terraengine://project/state"}, {"name", "Project State"}},
			{{"uri", "terraengine://scene/summary"}, {"name", "Scene Summary"}},
			{{"uri", "terraengine://diagnostics/summary"}, {"name", "Diagnostics Summary"}},
			{{"uri", "terraengine://renderer/recovery"}, {"name", "Renderer Recovery"}},
			{{"uri", "terraengine://capture/availability"}, {"name", "Capture Availability"}},
			{{"uri", "terraengine://activity/recent"}, {"name", "Recent Activity"}},
			{{"uri", "terraengine://jobs/recent"}, {"name", "Recent Jobs"}},
			{{"uri", "terraengine://logs/recent"}, {"name", "Recent Structured Logs"}},
			{{"uri", "terraengine://approvals/pending"}, {"name", "Pending Approvals"}}
		});
	}

	void EditorMcpBridge::RecordActivity(ActivityEventType type, std::string requestId, std::string capability, std::string message, std::string outcome)
	{
		ActivityRecord record;
		record.sequence = nextActivitySequence++;
		record.type = type;
		record.requestId = std::move(requestId);
		record.capability = std::move(capability);
		record.message = std::move(message);
		record.outcome = std::move(outcome);
		activityHistory.push_front(std::move(record));
		// Keep the newest records and discard old ones so long editor sessions
		// cannot grow unbounded memory just by serving assistant activity logs.
		while (activityHistory.size() > maxActivityRecords)
		{
			activityHistory.pop_back();
		}
	}

	std::vector<ActivityRecord> EditorMcpBridge::GetRecentActivity(std::size_t limit) const
	{
		const std::size_t count = std::min(limit, activityHistory.size());
		return {activityHistory.begin(), activityHistory.begin() + static_cast<std::ptrdiff_t>(count)};
	}

	nlohmann::json EditorMcpBridge::BuildStatusJson() const
	{
		return {
			{"enabled", IsEnabled()},
			{"state", ToString(session.state)},
			{"writeMode", ToString(writeMode)},
			{"endpoint", GetListenEndpoint()},
			{"port", GetListenPort()},
			{"configuredPort", GetConfiguredListenPort()},
			{"connectedClientCount", session.connectedClientCount},
			{"clientName", session.clientName},
			{"pendingRequestCount", requestQueue.size()},
			{"pendingApprovalCount", GetPendingApprovals().size()},
			{"recentActivityCount", activityHistory.size()},
			{"lastDiagnosticRequest", lastDiagnosticRequest}
		};
	}

	std::uint16_t EditorMcpBridge::GetListenPort() const
	{
		return transport ? transport->GetListenPort() : 0;
	}

	std::string EditorMcpBridge::GetListenEndpoint() const
	{
		return transport ? transport->GetEndpoint() : std::string();
	}

	std::string EditorMcpBridge::GetTransportLastError() const
	{
		if (!listenPortConfigurationError.empty())
		{
			return listenPortConfigurationError;
		}
		return transport ? transport->GetLastError() : std::string();
	}

	nlohmann::json EditorMcpBridge::BuildCapabilityListJson() const
	{
		return service.BuildCapabilityList();
	}

	nlohmann::json EditorMcpBridge::BuildCaptureAvailabilityJson() const
	{
		return service.BuildCaptureAvailability();
	}

	ApprovalPrompt EditorMcpBridge::CreateApprovalPrompt(std::string requestId, std::string capability, std::string summary, nlohmann::json arguments, std::chrono::system_clock::time_point expiresAt)
	{
		ApprovalPrompt prompt;
		prompt.approvalId = "approval-" + std::to_string(nextApprovalSequence++);
		prompt.requestId = std::move(requestId);
		prompt.capability = std::move(capability);
		prompt.summary = std::move(summary);
		prompt.arguments = std::move(arguments);
		prompt.expiresAt = expiresAt == std::chrono::system_clock::time_point{}
			? std::chrono::system_clock::now() + std::chrono::minutes(5)
			: expiresAt;
		approvals.push_back(prompt);
		RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability, "Created approval prompt.", "waiting_approval");
		return prompt;
	}

	bool EditorMcpBridge::ResolveApproval(std::string_view approvalId, bool approved)
	{
		for (ApprovalPrompt& prompt : approvals)
		{
			if (prompt.approvalId == approvalId && !prompt.resolved)
			{
				prompt.resolved = true;
				prompt.approved = approved;
				RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability, approved ? "Approved request." : "Denied request.", approved ? "completed" : "rejected");
				if (approved)
				{
					nlohmann::json result = service.HandleTool(prompt.capability, prompt.arguments, writeMode, prompt.requestId);
					RecordActivity(result.value("ok", false) ? ActivityEventType::Request : ActivityEventType::Rejection, prompt.requestId, prompt.capability, result.value("message", ""), result.value("status", "completed"));
				}
				return true;
			}
		}
		return false;
	}

	bool EditorMcpBridge::CancelApproval(std::string_view approvalId)
	{
		for (ApprovalPrompt& prompt : approvals)
		{
			if (prompt.approvalId == approvalId && !prompt.resolved)
			{
				prompt.resolved = true;
				prompt.approved = false;
				RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability, "Canceled approval prompt.", "canceled");
				return true;
			}
		}
		return false;
	}

	std::vector<ApprovalPrompt> EditorMcpBridge::GetPendingApprovals() const
	{
		std::vector<ApprovalPrompt> pending;
		for (const ApprovalPrompt& prompt : approvals)
		{
			if (!prompt.resolved)
			{
				pending.push_back(prompt);
			}
		}
		return pending;
	}

	bool EditorMcpBridge::RequiresWritePermission(const AutomationCapabilityDescriptor& capability) const
	{
		return capability.mutationMode != MutationMode::ReadOnly;
	}

	bool EditorMcpBridge::RequiresApproval(const AutomationCapabilityDescriptor& capability) const
	{
		// Destructive/high-impact actions stop at an explicit approval prompt
		// before execution so the editor user remains in control of data loss,
		// file deletion, export overwrite, or bridge exposure changes.
		return capability.approvalPolicy != AutomationApprovalPolicy::None;
	}

	void EditorMcpBridge::PersistPreferences() const
	{
		// MCP defaults are editor preferences rather than project data. The bridge
		// reports the live state here, and ProjectManager owns the JSON file format.
		if (editorContext == nullptr || editorContext->projectManager == nullptr)
		{
			return;
		}
		editorContext->projectManager->SetEditorMcpPreferences(IsEnabled(), std::string(ToString(writeMode)), GetListenEndpoint(), configuredListenPort);
	}

	nlohmann::json EditorMcpBridge::BuildApprovalResponse(const AutomationCapabilityDescriptor& capability, const nlohmann::json& arguments, const std::string& requestId)
	{
		ApprovalPrompt prompt = CreateApprovalPrompt(requestId, capability.name, "Approval required for " + capability.name, arguments);
		nlohmann::json result;
		result["ok"] = false;
		result["requestId"] = requestId;
		result["status"] = ToString(RequestStatus::WaitingApproval);
		result["message"] = "Approval is required before this request can run.";
		result["data"] = {{"approvalId", prompt.approvalId}, {"capability", prompt.capability}};
		result["error"] = nullptr;
		return result;
	}

	void EditorMcpBridge::ProcessPendingToolCalls()
	{
		std::deque<std::shared_ptr<PendingToolCall>> calls;
		{
			std::lock_guard lock(pendingToolCallsMutex);
			calls.swap(pendingToolCalls);
		}

		for (const std::shared_ptr<PendingToolCall>& call : calls)
		{
			nlohmann::json result = CallTool(call->name, call->arguments);
			{
				std::lock_guard callLock(call->mutex);
				call->result = std::move(result);
				call->completed = true;
			}
			call->completedSignal.notify_all();
		}
	}

	bool EditorMcpBridge::CancelQueuedRequest(std::string_view requestId)
	{
		for (AssistantRequest& request : requestQueue)
		{
			if (request.requestId == requestId && !request.canceled)
			{
				request.canceled = true;
				request.status = RequestStatus::Canceled;
				RecordActivity(ActivityEventType::Request, request.requestId, request.capability, "Canceled queued request.", "canceled");
				return true;
			}
		}
		return false;
	}

	void EditorMcpBridge::ExpirePendingApprovals()
	{
		const auto now = std::chrono::system_clock::now();
		for (ApprovalPrompt& prompt : approvals)
		{
			if (!prompt.resolved && prompt.expiresAt <= now)
			{
				prompt.resolved = true;
				prompt.approved = false;
				RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability, "Approval prompt expired.", "canceled");
			}
		}
	}
}
