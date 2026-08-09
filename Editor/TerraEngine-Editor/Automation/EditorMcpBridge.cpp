// EditorMcpBridge.cpp - Policy/state layer for TerraEngine editor MCP access.
//
// Mutating editor work is queued through this bridge and executed by the
// automation service from editor-owned update flow. That serialization is
// deliberate: scene state, undo/redo, dirty flags, and selection are all
// editor-owned concepts, so concurrent transport callbacks must not mutate
// them directly.

#include "EditorMcpBridge.h"

#include "AnimationAutomationSchemas.h"
#include "EditorMcpTransport.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "Engine/Physics/PhysicsSystem.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		bool IsCaptureTool(std::string_view name)
		{
			return name == "game_view.capture"
				|| name == "camera.capture_object"
				|| name == "camera.capture_custom"
				|| name == "animation.camera.capture";
		}

		bool IsEnabledState(BridgeState state)
		{
			return state == BridgeState::Listening || state == BridgeState::Connected;
		}

		std::string StableApprovalFingerprint(std::string_view value)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for (unsigned char byte : value)
			{
				hash ^= byte;
				hash *= 1099511628211ull;
			}
			std::ostringstream stream;
			stream << std::hex << hash;
			return stream.str();
		}

		std::string PhysicsImpactScope(std::string_view capability)
		{
			if (capability.starts_with("physics.settings.")) return "physics.settings";
			if (capability.starts_with("physics.layer.")) return "physics.layer";
			if (capability.starts_with("physics.material.")) return "physics.material";
			if (capability.starts_with("physics.collider.")) return "physics.collider";
			return std::string(capability);
		}

		nlohmann::json ApprovalImpactPayload(nlohmann::json arguments)
		{
			// Transport-only and replay-control fields are not part of the authored
			// reference impact. The semantic patch and target identities remain.
			for (const char* field : {"approval", "correlationId", "idempotencyKey", "worldGeneration", "projectGeneration", "sceneGeneration"})
			{
				arguments.erase(field);
			}
			return arguments;
		}

		std::string BuildImpactFingerprint(
			std::string_view capability,
			const nlohmann::json& arguments,
			std::uint64_t projectGeneration,
			std::uint64_t sceneGeneration)
		{
			return StableApprovalFingerprint(
				PhysicsImpactScope(capability) + "|" + ApprovalImpactPayload(arguments).dump()
				+ "|" + std::to_string(projectGeneration) + "|" + std::to_string(sceneGeneration));
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
		std::lock_guard lifecycleLock(lifecycleMutex);
		if (transport)
		{
			// One bridge instance has one terminal lifetime. Silently retaining the
			// existing transport is safer than replacing a thread-owning object.
			return;
		}
		editorContext = context;
		mainEditor = editor;
		service.SetEditorContext(context);
		service.SetMainEditor(editor);
		transport = std::make_unique<EditorMcpTransport>(*this);
		{
			std::lock_guard lock(stateMutex);
			session.sessionId = "terraengine-editor-local-" + std::to_string(nextSessionSequence++);
			session.clientName.clear();
			session.peerAddress.clear();
			session.connectedClientCount = 0;
			session.state = BridgeState::Disabled;
			writeMode = WriteMode::ReadOnly;
		}
		{
			std::lock_guard lock(pendingToolCallsMutex);
			acceptingToolCalls = false;
		}
		RecordActivity(ActivityEventType::Configuration, {}, "bridge.initialize", "Initialized editor MCP bridge.", "completed");
	}

	void EditorMcpBridge::Shutdown()
	{
		// Close transport admission before waiting for an in-flight handler. This
		// also gives ProcessPendingToolCalls a lock-independent stop signal between
		// calls so requests not yet processing remain available for cancellation.
		{
			std::lock_guard lock(pendingToolCallsMutex);
			acceptingToolCalls = false;
		}
		std::lock_guard lifecycleLock(lifecycleMutex);
		{
			// An enable operation that already owned the lifecycle gate may have
			// reopened admission after the early stop signal above. Reassert the
			// terminal value while owning that same gate before draining queues.
			std::lock_guard lock(pendingToolCallsMutex);
			acceptingToolCalls = false;
		}
		{
			std::lock_guard lock(stateMutex);
			if (session.state == BridgeState::Stopping || session.state == BridgeState::Stopped)
			{
				return;
			}
		}
		std::deque<std::shared_ptr<PendingToolCall>> calls;
		{
			// Close admission and detach queued calls before joining the listener.
			// A listener may be waiting inside SubmitToolCall, and completing that
			// call first lets it finish its HTTP response without a 30-second join.
			std::lock_guard lock(pendingToolCallsMutex);
			calls.swap(pendingToolCalls);
		}

		std::vector<ApprovalPrompt> canceledApprovals;
		{
			std::lock_guard lock(stateMutex);
			session.state = BridgeState::Stopping;
			// Draining requests on shutdown prevents half-applied assistant work
			// from running after editor subsystems have begun tearing down.
			for (AssistantRequest& request : requestQueue)
			{
				request.status = RequestStatus::Canceled;
				request.canceled = true;
			}
			requestQueue.clear();
			for (ApprovalPrompt& prompt : approvals)
			{
				if (!prompt.resolved)
				{
					prompt.resolved = true;
					prompt.approved = false;
					prompt.dispatchCompleted = true;
					prompt.resolutionState = "canceled";
					canceledApprovals.push_back(prompt);
				}
			}
			approvals.clear();
			session.sessionId = "terraengine-editor-local-" + std::to_string(nextSessionSequence++);
			session.clientName.clear();
			session.peerAddress.clear();
			session.connectedClientCount = 0;
		}

		for (const std::shared_ptr<PendingToolCall>& call : calls)
		{
			const nlohmann::json cancellation = service.BuildError(NextRequestId(), ErrorCategory::Canceled,
				"Bridge shut down before the request could run.");
			bool notify = false;
			{
				std::lock_guard callLock(call->mutex);
				if (call->state != PendingToolCall::State::Completed
					&& call->state != PendingToolCall::State::Abandoned)
				{
					call->result = cancellation;
					call->state = PendingToolCall::State::Completed;
					notify = true;
				}
			}
			if (notify)
			{
				call->completedSignal.notify_all();
			}
		}
		for (const ApprovalPrompt& prompt : canceledApprovals)
		{
			RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability,
				"Canceled approval because the MCP bridge shut down.", "canceled");
		}
		if (transport)
		{
			transport->StopListening();
		}
		{
			std::lock_guard lock(stateMutex);
			session.state = BridgeState::Stopped;
		}
		RecordActivity(ActivityEventType::Connection, {}, "bridge.shutdown", "Stopped editor MCP bridge.", "canceled");
	}

	void EditorMcpBridge::Update(float)
	{
		ExpirePendingApprovals();
		ProcessPendingToolCalls();
		// The queue is FIFO so repeated assistant commands produce deterministic
		// scene/editor state even when a client sends overlapping requests.
		for (;;)
		{
			const std::optional<AssistantRequest> queued = PopNextQueuedRequest();
			if (!queued)
			{
				break;
			}
			AssistantRequest request = *queued;
			if (request.canceled)
			{
				RecordActivity(ActivityEventType::Request, request.requestId, request.capability, "Skipped canceled request.", "canceled");
				continue;
			}
			(void)CallTool(request.capability, request.arguments);
		}
	}

	void EditorMcpBridge::PumpAnimationRecordingsAtProtectedFrame()
	{
		// The bridge owns the one shared automation service and durable job store.
		// Capture itself reads only immutable AnimationSystem publications; transport
		// enablement and client connection lifetime deliberately do not participate.
		service.PumpAnimationRecordingsAtProtectedFrame();
	}

	void EditorMcpBridge::PumpRendererValidationMutationsAtProtectedFrame()
	{
		// The service produces one typed transaction from a retained target set.
		// Transport requests only start or inspect the operation; they never pace it.
		service.PumpRendererValidationMutationsAtProtectedFrame();
	}

	void EditorMcpBridge::SetMainEditor(MainEditorSubsystem* editor)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		{
			std::lock_guard lock(stateMutex);
			// Service wiring is startup configuration. Replacing a raw editor
			// pointer while a listener is dispatching would race provider reads.
			if (session.state != BridgeState::Disabled)
			{
				return;
			}
		}
		mainEditor = editor;
		service.SetMainEditor(editor);
	}

	void EditorMcpBridge::SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		{
			std::lock_guard lock(stateMutex);
			// std::function assignment is not safe while a provider may invoke the
			// old callback, so selection wiring is allowed only before enablement.
			if (session.state != BridgeState::Disabled)
			{
				return;
			}
		}
		service.SetSelectionCallbacks(std::move(getter), std::move(setter));
	}

	void EditorMcpBridge::SetEnabled(bool enabled)
	{
		if (!enabled)
		{
			// Stop accepting work before waiting for the lifecycle gate. The
			// current provider may finish, while later queued calls stay cancelable.
			std::lock_guard lock(pendingToolCallsMutex);
			acceptingToolCalls = false;
		}
		std::lock_guard lifecycleLock(lifecycleMutex);
		if (!enabled)
		{
			// SetEnabled(true) may have been ahead of this caller at the lifecycle
			// gate and republished admission after the early close. Close it again
			// under the gate so Disabled and Stopped can never accept a late call.
			std::lock_guard lock(pendingToolCallsMutex);
			acceptingToolCalls = false;
		}
		{
			std::lock_guard lock(stateMutex);
			// Stopping and Stopped are terminal states. In particular, a late
			// preference callback must never reopen a listener after Shutdown.
			if (session.state == BridgeState::Stopping || session.state == BridgeState::Stopped)
			{
				return;
			}
			if ((enabled && IsEnabledState(session.state))
				|| (!enabled && session.state == BridgeState::Disabled))
			{
				return;
			}
		}
		if (enabled)
		{
	#if defined(TERRAENGINE_EDITOR_TESTS)
			if (enablePublicationHookForTests)
			{
				// This hook deliberately runs while the lifecycle gate is held. It lets
				// the focused test prove shutdown re-closes admission after an enable
				// operation was already admitted, without changing production behavior.
				enablePublicationHookForTests();
			}
	#endif
			std::uint16_t port = 0;
			{
				// Publish enabled state and tool admission as one transition so
				// connection workers can never observe a half-enabled bridge.
				std::scoped_lock lock(stateMutex, pendingToolCallsMutex);
				const bool beginsListenerSession = !IsEnabledState(session.state);
				if (beginsListenerSession)
				{
					// The transport handles one HTTP request per socket, but all of
					// those short-lived sockets belong to one user-controlled listener
					// session. Rotate only at this explicit lifetime boundary.
					session.sessionId = "terraengine-editor-local-" + std::to_string(nextSessionSequence++);
					session.clientName.clear();
					session.peerAddress.clear();
				}
				listenPortConfigurationError.clear();
				session.state = session.connectedClientCount > 0 ? BridgeState::Connected : BridgeState::Listening;
				port = configuredListenPort;
				acceptingToolCalls = true;
			}
			if (transport && !transport->IsListening())
			{
				// Always bind the saved port. Falling back to port zero would create a
				// random endpoint and break external MCP client configuration.
				if (!transport->StartListening(port))
				{
					{
						std::scoped_lock lock(stateMutex, pendingToolCallsMutex);
						acceptingToolCalls = false;
						session.state = BridgeState::Disabled;
					}
					RecordActivity(ActivityEventType::Rejection, {}, "transport.listen", transport->GetLastError(), "failed");
					return;
				}
			}
			RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_enabled", "Enabled local MCP bridge.", "completed");
			PersistPreferences();
		}
		else
		{
			std::deque<std::shared_ptr<PendingToolCall>> calls;
			{
				std::lock_guard lock(pendingToolCallsMutex);
				calls.swap(pendingToolCalls);
			}

			std::deque<AssistantRequest> canceledRequests;
			std::vector<ApprovalPrompt> canceledApprovals;
			{
				std::lock_guard lock(stateMutex);
				const bool endsListenerSession = IsEnabledState(session.state);
				session.state = BridgeState::Stopping;
				// Legacy producers use the assistant request queue instead of the
				// synchronous tool-call queue. Detach it at the same listener
				// boundary so disable/re-enable cannot replay an earlier session.
				canceledRequests.swap(requestQueue);
				if (endsListenerSession)
				{
					// Disabling the listener is the explicit security boundary
					// between logical clients. Resolve outstanding prompts and rotate
					// the identity before the listener thread can finish disconnecting.
					for (ApprovalPrompt& prompt : approvals)
					{
						if (!prompt.resolved)
						{
							prompt.resolved = true;
							prompt.approved = false;
							prompt.dispatchCompleted = true;
							prompt.resolutionState = "canceled";
							canceledApprovals.push_back(prompt);
						}
					}
					session.sessionId = "terraengine-editor-local-" + std::to_string(nextSessionSequence++);
				}
				session.connectedClientCount = 0;
				session.clientName.clear();
				session.peerAddress.clear();
			}

			for (AssistantRequest& request : canceledRequests)
			{
				request.status = RequestStatus::Canceled;
				request.canceled = true;
				RecordActivity(ActivityEventType::Request, request.requestId, request.capability,
					"Canceled queued request because the MCP listener was disabled.", "canceled");
			}
			for (const std::shared_ptr<PendingToolCall>& call : calls)
			{
				const nlohmann::json cancellation = service.BuildError(NextRequestId(), ErrorCategory::Canceled,
					"Bridge was disabled before the request could run.");
				bool notify = false;
				{
					std::lock_guard callLock(call->mutex);
					if (call->state != PendingToolCall::State::Completed
						&& call->state != PendingToolCall::State::Abandoned)
					{
						call->result = cancellation;
						call->state = PendingToolCall::State::Completed;
						notify = true;
					}
				}
				if (notify)
				{
					call->completedSignal.notify_all();
				}
			}
			for (const ApprovalPrompt& prompt : canceledApprovals)
			{
				RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability,
					"Canceled approval because the MCP listener was disabled.", "canceled");
			}
			if (transport)
			{
				transport->StopListening();
			}
			{
				std::lock_guard lock(stateMutex);
				session.state = BridgeState::Disabled;
			}
			RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_enabled", "Disabled local MCP bridge.", "completed");
			PersistPreferences();
		}
	}

	bool EditorMcpBridge::IsEnabled() const
	{
		std::lock_guard lock(stateMutex);
		return IsEnabledState(session.state);
	}

#if defined(TERRAENGINE_EDITOR_TESTS)
	void EditorMcpBridge::SetEnablePublicationHookForTests(std::function<void()> hook)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		enablePublicationHookForTests = std::move(hook);
	}
#endif

	void EditorMcpBridge::SetWriteMode(WriteMode mode)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		{
			std::lock_guard lock(stateMutex);
			if (session.state == BridgeState::Stopping || session.state == BridgeState::Stopped)
			{
				return;
			}
			writeMode = mode;
		}
		RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_write_mode", std::string("Changed write mode to ") + ToString(mode) + ".", "completed");
		PersistPreferences();
	}

	WriteMode EditorMcpBridge::GetWriteMode() const
	{
		std::lock_guard lock(stateMutex);
		return writeMode;
	}

	bool EditorMcpBridge::SetListenPort(std::uint16_t port)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		if (!TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(port))
		{
			std::lock_guard lock(stateMutex);
			listenPortConfigurationError = "MCP listen port must be between 1024 and 65535.";
			return false;
		}
		{
			std::lock_guard lock(stateMutex);
			if (session.state == BridgeState::Stopping || session.state == BridgeState::Stopped)
			{
				listenPortConfigurationError = "A stopped MCP bridge cannot be reconfigured.";
				return false;
			}
			if (IsEnabledState(session.state))
			{
				// Rebinding would disconnect the active client. Requiring an explicit
				// disable makes that disruption visible and intentional in Preferences.
				listenPortConfigurationError = "Disable the MCP bridge before changing its listen port.";
				return false;
			}
			configuredListenPort = port;
			listenPortConfigurationError.clear();
		}
		RecordActivity(ActivityEventType::Configuration, {}, "bridge.set_listen_port", "Changed MCP listen port to " + std::to_string(port) + ".", "completed");
		PersistPreferences();
		return true;
	}

	std::uint16_t EditorMcpBridge::GetConfiguredListenPort() const
	{
		std::lock_guard lock(stateMutex);
		return configuredListenPort;
	}

	BridgeState EditorMcpBridge::GetState() const
	{
		std::lock_guard lock(stateMutex);
		return session.state;
	}

	bool EditorMcpBridge::TryOnLocalClientConnected(std::string clientName, std::string peerAddress)
	{
		{
			// Check the transport admission flag and session state atomically. Do
			// not take the lifecycle gate here: disable owns it while joining client
			// workers, so a worker waiting for that gate would deadlock shutdown.
			std::scoped_lock lock(pendingToolCallsMutex, stateMutex);
			if (!acceptingToolCalls || !IsEnabledState(session.state))
			{
				return false;
			}
			// A loopback HTTP socket carries one request and then closes. Keep the
			// listener-session identity stable across those ordinary socket
			// lifetimes; SetEnabled(false) remains the invalidation boundary.
			session.clientName = std::move(clientName);
			session.peerAddress = std::move(peerAddress);
			++session.connectedClientCount;
			session.state = BridgeState::Connected;
		}
		RecordActivity(ActivityEventType::Connection, {}, "transport.connect", "Accepted local MCP client.", "completed");
		return true;
	}

	void EditorMcpBridge::OnLocalClientDisconnected(std::string reason)
	{
		{
			std::lock_guard lock(stateMutex);
			// Preserve the client kind and listener-session identity. The HTTP
			// transport intentionally closes after every response, so clearing
			// either value here would make a freshly returned approval impossible
			// to resolve. A late disconnect must not resurrect a stopping bridge.
			if (session.connectedClientCount > 0)
			{
				--session.connectedClientCount;
			}
			if (session.state == BridgeState::Connected && session.connectedClientCount == 0)
			{
				session.state = BridgeState::Listening;
			}
		}
		RecordActivity(ActivityEventType::Connection, {}, "transport.disconnect", std::move(reason), "completed");
	}

	void EditorMcpBridge::RecordRejectedPeer(std::string peerAddress, std::string reason)
	{
		RecordActivity(ActivityEventType::Rejection, {}, "transport.reject", "Rejected " + peerAddress + ": " + reason, "rejected");
	}

	std::string EditorMcpBridge::NextRequestId()
	{
		std::lock_guard lock(stateMutex);
		return "mcp-" + std::to_string(nextRequestSequence++);
	}

	void EditorMcpBridge::EnqueueRequest(std::string capability, nlohmann::json arguments)
	{
		AssistantRequest request;
		bool accepted = false;
		{
			std::lock_guard lock(stateMutex);
			// Checking state and inserting under one lock prevents a producer from
			// adding work after shutdown has atomically drained the live queue.
			if (IsEnabledState(session.state))
			{
				request.requestId = "mcp-" + std::to_string(nextRequestSequence++);
				request.capability = std::move(capability);
				request.arguments = std::move(arguments);
				requestQueue.push_back(request);
				accepted = true;
			}
		}
		if (!accepted)
		{
			RecordActivity(ActivityEventType::Rejection, {}, capability,
				"Rejected queued request because the MCP bridge is disabled.", "rejected");
			return;
		}
		RecordActivity(ActivityEventType::Request, request.requestId, request.capability, "Queued request.", "queued");
	}

	std::optional<AssistantRequest> EditorMcpBridge::PopNextQueuedRequest()
	{
		std::lock_guard lock(stateMutex);
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
		std::lock_guard lock(stateMutex);
		return {requestQueue.begin(), requestQueue.end()};
	}

	nlohmann::json EditorMcpBridge::CallTool(std::string_view name, const nlohmann::json& arguments)
	{
		// Hold the lifecycle gate through provider dispatch. Disable and shutdown
		// therefore either happen before admission or wait for the current handler
		// to cross its normal transactional completion boundary.
		std::lock_guard lifecycleLock(lifecycleMutex);
		return CallToolLocked(name, arguments);
	}

	nlohmann::json EditorMcpBridge::CallToolLocked(std::string_view name, const nlohmann::json& arguments)
	{
		bool enabled = false;
		WriteMode capturedWriteMode = WriteMode::ReadOnly;
		{
			std::lock_guard lock(stateMutex);
			enabled = IsEnabledState(session.state);
			capturedWriteMode = writeMode;
		}
		bool accepting = false;
		{
			std::lock_guard lock(pendingToolCallsMutex);
			accepting = acceptingToolCalls;
		}
		if (!enabled || !accepting)
		{
			return service.BuildError(NextRequestId(), ErrorCategory::Permission, "The MCP bridge is disabled.");
		}

		const std::string requestId = NextRequestId();
		// Policy follows the same provider-owned descriptor used by discovery and
		// dispatch. This also covers component/subsystem declarations registered
		// after startup without adding them to a legacy bridge capability list.
		const auto capability = service.GetRegistry().Find(name, 1, service.BuildAutomationStateSnapshot());
		if (capability && RequiresWritePermission(capability->descriptor) && capturedWriteMode != WriteMode::WritesEnabled)
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
			std::lock_guard lock(stateMutex);
			lastDiagnosticRequest = arguments.dump();
		}
		nlohmann::json result = service.HandleTool(name, arguments, capturedWriteMode, requestId);
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

	nlohmann::json EditorMcpBridge::SubmitToolCall(
		std::string name,
		nlohmann::json arguments,
		nlohmann::json transportRequestId,
		const std::atomic<bool>* transportStopRequested)
	{
		auto call = std::make_shared<PendingToolCall>();
		call->name = std::move(name);
		call->arguments = std::move(arguments);
		call->transportRequestId = std::move(transportRequestId);
		bool accepted = false;
		{
			std::lock_guard lock(pendingToolCallsMutex);
			if (acceptingToolCalls)
			{
				pendingToolCalls.push_back(call);
				accepted = true;
			}
		}
		if (!accepted)
		{
			return service.BuildError(NextRequestId(), ErrorCategory::Canceled,
				"The MCP bridge is not accepting tool calls.");
		}

		std::unique_lock callLock(call->mutex);
		const auto requestDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		bool completed = call->state == PendingToolCall::State::Completed;
		while (!completed && std::chrono::steady_clock::now() < requestDeadline)
		{
			// A standalone transport owns its own worker threads. Polling a small,
			// bounded interval lets its destructor reclaim a queued call promptly
			// without changing the normal 30-second editor-dispatch contract.
			if (transportStopRequested != nullptr
				&& transportStopRequested->load(std::memory_order_acquire)
				&& call->state == PendingToolCall::State::Queued)
			{
				call->state = PendingToolCall::State::Abandoned;
				const std::string canceledName = call->name;
				callLock.unlock();
				const std::string requestId = NextRequestId();
				if (IsCaptureTool(canceledName))
				{
					RecordActivity(ActivityEventType::Capture, requestId, canceledName,
						"Canceled because its local MCP transport stopped before editor dispatch.", "canceled");
				}
				return service.BuildError(requestId, ErrorCategory::Canceled,
					"The local MCP transport stopped before the editor update processed the request.");
			}
			const auto nextWake = std::min(requestDeadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
			completed = call->completedSignal.wait_until(callLock, nextWake, [&call]() {
				return call->state == PendingToolCall::State::Completed;
			});
		}
		if (!completed)
		{
			// A queued request that times out must never run later after the client
			// has already received a timeout. Processing work is editor-owned and
			// may already have crossed its transactional commit boundary.
			if (call->state == PendingToolCall::State::Queued)
			{
				call->state = PendingToolCall::State::Abandoned;
			}
			const std::string timedOutName = call->name;
			callLock.unlock();
			const std::string requestId = NextRequestId();
			if (IsCaptureTool(timedOutName))
			{
				RecordActivity(ActivityEventType::Capture, requestId, timedOutName, "Timed out waiting for the editor update to process the capture request.", "canceled");
			}
			return service.BuildError(requestId, ErrorCategory::Canceled, "Timed out waiting for the editor update to process the request.");
		}
		return call->result;
	}

	bool EditorMcpBridge::CancelPendingToolCall(const nlohmann::json& transportRequestId)
	{
		if (transportRequestId.is_null())
		{
			return false;
		}

		// Build the stable response before removing anything. If JSON allocation
		// fails, the queued call remains reachable and can still complete normally.
		const std::string cancellationRequestId = NextRequestId();
		const nlohmann::json cancellation = service.BuildError(cancellationRequestId, ErrorCategory::Canceled,
			"The MCP client canceled the request before it began processing.");
		std::shared_ptr<PendingToolCall> call;
		{
			std::lock_guard lock(pendingToolCallsMutex);
			const auto found = std::find_if(pendingToolCalls.begin(), pendingToolCalls.end(),
				[&transportRequestId](const std::shared_ptr<PendingToolCall>& candidate) {
					return candidate->transportRequestId == transportRequestId;
				});
			if (found == pendingToolCalls.end())
			{
				return false;
			}
			call = *found;
			pendingToolCalls.erase(found);
		}

		bool notify = false;
		{
			std::lock_guard callLock(call->mutex);
			// Processing calls may already have crossed a transactional boundary.
			// Only calls still queued are safe to cancel without partial effects.
			if (call->state == PendingToolCall::State::Queued)
			{
				call->result = cancellation;
				call->state = PendingToolCall::State::Completed;
				notify = true;
			}
		}
		if (!notify)
		{
			return false;
		}
		call->completedSignal.notify_all();
		RecordActivity(ActivityEventType::Request, cancellationRequestId, call->name,
			"Canceled pending MCP tool call.", "canceled");
		return true;
	}

	nlohmann::json EditorMcpBridge::ReadResource(std::string_view uri) const
	{
		const std::string resource(uri);
		if (resource == "terraengine://bridge/status") return BuildStatusJson();
		if (resource == "terraengine://bridge/capabilities") return BuildCapabilityListJson();
		if (resource == "terraengine://animation/capabilities")
		{
			nlohmann::json animationCapabilities = nlohmann::json::array();
			// BuildCapabilityList returns a versioned envelope. Iterate its
			// capability array rather than the envelope values so this
			// animation-only resource cannot silently publish an empty list.
			const nlohmann::json capabilityList = service.BuildCapabilityList();
			for (const nlohmann::json& capability : capabilityList.value("capabilities", nlohmann::json::array()))
			{
				if (capability.value("category", std::string{}) == "animation")
				{
					animationCapabilities.push_back(capability);
				}
			}
			return {
				{"ok", true},
				{"schemaVersion", kAnimationAutomationSchemaVersion},
				{"capabilities", std::move(animationCapabilities)}
			};
		}
		if (resource == "terraengine://animation/jobs")
		{
			nlohmann::json jobs = nlohmann::json::array();
			for (const AutomationJobSnapshot& job : service.GetJobService().List(256U))
			{
				if (!job.capability.starts_with("animation."))
				{
					continue;
				}
				jobs.push_back({
					{"jobId", job.jobId},
					{"kind", job.kind},
					{"capability", job.capability},
					{"state", JobStateJson(job.state)},
					{"progress", job.progress},
					{"stage", job.stage},
					{"cancellable", job.cancellable},
					{"cancelRequested", job.cancelRequested},
					{"correlationId", job.correlationId},
					{"retentionSequence", job.retentionSequence}
				});
			}
			return {
				{"ok", true},
				{"schemaVersion", kAnimationAutomationSchemaVersion},
				{"jobs", std::move(jobs)},
				{"oldestRetainedSequence", service.GetJobService().OldestRetainedSequence()}
			};
		}
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
		if (resource == "terraengine://approvals/recent")
		{
			nlohmann::json recent = nlohmann::json::array();
			std::lock_guard lock(stateMutex);
			// HTTP uses one request per connection. Retain results across those
			// connections, but expose only the current listener session so an old
			// approval can never be confused with a newly authorized client.
			for (auto approval = approvals.rbegin();
				approval != approvals.rend() && recent.size() < 64U; ++approval)
			{
				if (approval->sessionId != session.sessionId)
				{
					continue;
				}
				recent.push_back({
					{"approvalId", approval->approvalId},
					{"requestId", approval->requestId},
					{"capability", approval->capability},
					{"summary", approval->summary},
					{"state", approval->resolutionState},
					{"approved", approval->approved},
					{"dispatchCompleted", approval->dispatchCompleted},
					{"dispatchResult", approval->dispatchResult},
					{"argumentsFingerprint", approval->argumentsFingerprint},
					{"impactFingerprint", approval->impactFingerprint},
					{"projectGeneration", approval->projectGeneration},
					{"sceneGeneration", approval->sceneGeneration},
					{"worldGeneration", approval->worldGeneration}
				});
			}
			return {{"ok", true}, {"approvals", std::move(recent)}};
		}
		return {{"ok", false}, {"error", {{"category", ToString(ErrorCategory::UnsupportedCapability)}, {"details", "Unknown resource URI."}}}};
	}

	nlohmann::json EditorMcpBridge::ListResources() const
	{
		return nlohmann::json::array({
			{{"uri", "terraengine://bridge/status"}, {"name", "Bridge Status"}},
			{{"uri", "terraengine://bridge/capabilities"}, {"name", "Bridge Capabilities"}},
			{{"uri", "terraengine://animation/capabilities"}, {"name", "Animation Capabilities and Limits"}},
			{{"uri", "terraengine://animation/jobs"}, {"name", "Animation Jobs"}},
			{{"uri", "terraengine://project/state"}, {"name", "Project State"}},
			{{"uri", "terraengine://scene/summary"}, {"name", "Scene Summary"}},
			{{"uri", "terraengine://diagnostics/summary"}, {"name", "Diagnostics Summary"}},
			{{"uri", "terraengine://renderer/recovery"}, {"name", "Renderer Recovery"}},
			{{"uri", "terraengine://capture/availability"}, {"name", "Capture Availability"}},
			{{"uri", "terraengine://activity/recent"}, {"name", "Recent Activity"}},
			{{"uri", "terraengine://jobs/recent"}, {"name", "Recent Jobs"}},
			{{"uri", "terraengine://logs/recent"}, {"name", "Recent Structured Logs"}},
			{{"uri", "terraengine://approvals/pending"}, {"name", "Pending Approvals"}},
			{{"uri", "terraengine://approvals/recent"}, {"name", "Recent Approval Results"}}
		});
	}

	void EditorMcpBridge::RecordActivity(ActivityEventType type, std::string requestId, std::string capability, std::string message, std::string outcome)
	{
		ActivityRecord record;
		record.type = type;
		record.requestId = std::move(requestId);
		record.capability = std::move(capability);
		record.message = std::move(message);
		record.outcome = std::move(outcome);
		std::lock_guard lock(activityMutex);
		record.sequence = nextActivitySequence++;
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
		std::lock_guard lock(activityMutex);
		const std::size_t count = std::min(limit, activityHistory.size());
		return {activityHistory.begin(), activityHistory.begin() + static_cast<std::ptrdiff_t>(count)};
	}

	nlohmann::json EditorMcpBridge::BuildStatusJson() const
	{
		BridgeSession capturedSession;
		WriteMode capturedWriteMode = WriteMode::ReadOnly;
		std::uint16_t capturedConfiguredPort = 0;
		std::size_t pendingRequestCount = 0;
		std::size_t pendingApprovalCount = 0;
		std::size_t recentApprovalCount = 0;
		std::string capturedLastDiagnosticRequest;
		{
			std::lock_guard lock(stateMutex);
			capturedSession = session;
			capturedWriteMode = writeMode;
			capturedConfiguredPort = configuredListenPort;
			pendingRequestCount = requestQueue.size();
			pendingApprovalCount = static_cast<std::size_t>(std::count_if(
				approvals.begin(), approvals.end(), [](const ApprovalPrompt& prompt) {
					return !prompt.resolved;
				}));
			recentApprovalCount = static_cast<std::size_t>(std::count_if(
				approvals.begin(), approvals.end(), [&capturedSession](const ApprovalPrompt& prompt) {
					return prompt.sessionId == capturedSession.sessionId;
				}));
			capturedLastDiagnosticRequest = lastDiagnosticRequest;
		}
		std::size_t recentActivityCount = 0;
		{
			std::lock_guard lock(activityMutex);
			recentActivityCount = activityHistory.size();
		}
		std::size_t pendingToolCallCount = 0;
		bool capturedToolCallAdmission = false;
		{
			std::lock_guard lock(pendingToolCallsMutex);
			pendingToolCallCount = pendingToolCalls.size();
			capturedToolCallAdmission = acceptingToolCalls;
		}
		return {
			{"enabled", IsEnabledState(capturedSession.state)},
			{"state", ToString(capturedSession.state)},
			{"writeMode", ToString(capturedWriteMode)},
			{"endpoint", GetListenEndpoint()},
			{"port", GetListenPort()},
			{"configuredPort", capturedConfiguredPort},
			{"connectedClientCount", capturedSession.connectedClientCount},
			{"clientName", capturedSession.clientName},
			{"pendingRequestCount", pendingRequestCount},
			{"pendingToolCallCount", pendingToolCallCount},
			{"acceptingToolCalls", capturedToolCallAdmission},
			{"pendingApprovalCount", pendingApprovalCount},
			{"recentApprovalCount", recentApprovalCount},
			{"recentActivityCount", recentActivityCount},
			{"lastDiagnosticRequest", capturedLastDiagnosticRequest}
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
		{
			std::lock_guard lock(stateMutex);
			if (!listenPortConfigurationError.empty())
			{
				return listenPortConfigurationError;
			}
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
		std::lock_guard lifecycleLock(lifecycleMutex);
		return CreateApprovalPromptLocked(std::move(requestId), std::move(capability), std::move(summary), std::move(arguments), expiresAt);
	}

	ApprovalPrompt EditorMcpBridge::CreateApprovalPromptLocked(std::string requestId, std::string capability, std::string summary, nlohmann::json arguments, std::chrono::system_clock::time_point expiresAt)
	{
		{
			std::lock_guard lock(pendingToolCallsMutex);
			if (!acceptingToolCalls)
			{
				return {};
			}
		}
		const AutomationStateSnapshot state = service.BuildAutomationStateSnapshot();
		ApprovalPrompt prompt;
		prompt.requestId = std::move(requestId);
		prompt.capability = std::move(capability);
		prompt.summary = std::move(summary);
		prompt.arguments = std::move(arguments);
		prompt.argumentsFingerprint = StableApprovalFingerprint(prompt.arguments.dump());
		prompt.impactFingerprint = BuildImpactFingerprint(prompt.capability, prompt.arguments, state.projectGeneration, state.sceneGeneration);
		prompt.projectGeneration = state.projectGeneration;
		prompt.sceneGeneration = state.sceneGeneration;
		prompt.worldGeneration = prompt.arguments.value("worldGeneration", TerraEngine::Physics::PhysicsSystem::Instance().WorldHandle().worldGeneration);
		prompt.expiresAt = expiresAt == std::chrono::system_clock::time_point{}
			? std::chrono::system_clock::now() + std::chrono::minutes(5)
			: expiresAt;
		{
			std::lock_guard lock(stateMutex);
			if (!IsEnabledState(session.state))
			{
				return {};
			}
			// Keep unresolved prompts until the user acts or they expire. When the
			// bounded history is full, discard only the oldest resolved entry;
			// never evict a still-actionable security decision to admit new work.
			while (approvals.size() >= maxApprovalRecords)
			{
				const auto resolved = std::find_if(approvals.begin(), approvals.end(),
					[](const ApprovalPrompt& candidate) {
						return candidate.resolved;
					});
				if (resolved == approvals.end())
				{
					return {};
				}
				approvals.erase(resolved);
			}
			prompt.approvalId = "approval-" + std::to_string(nextApprovalSequence++);
			prompt.sessionId = session.sessionId;
			prompt.clientName = session.clientName;
			approvals.push_back(prompt);
		}
		RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability, "Created approval prompt.", "waiting_approval");
		return prompt;
	}

	bool EditorMcpBridge::ResolveApproval(std::string_view approvalId, bool approved)
	{
		// Approval validation and protected dispatch are one lifecycle operation;
		// disable/shutdown cannot invalidate the binding between those two steps.
		std::lock_guard lifecycleLock(lifecycleMutex);
		const AutomationStateSnapshot state = service.BuildAutomationStateSnapshot();
		const std::uint32_t currentWorldGeneration = TerraEngine::Physics::PhysicsSystem::Instance().WorldHandle().worldGeneration;
		std::optional<ApprovalPrompt> resolvedPrompt;
		WriteMode capturedWriteMode = WriteMode::ReadOnly;
		bool bindingStillValid = false;
		{
			std::lock_guard lock(pendingToolCallsMutex);
			if (!acceptingToolCalls)
			{
				return false;
			}
		}
		{
			std::lock_guard lock(stateMutex);
			if (!IsEnabledState(session.state))
			{
				return false;
			}
			for (ApprovalPrompt& prompt : approvals)
			{
				if (prompt.approvalId == approvalId && !prompt.resolved)
				{
					// Generation checks catch lifecycle changes. Rebuild the semantic
					// fingerprint before injecting the approval so a retained token
					// cannot authorize a modified request.
					const std::string currentImpactFingerprint = BuildImpactFingerprint(
						prompt.capability, prompt.arguments, state.projectGeneration, state.sceneGeneration);
					bindingStillValid = prompt.expiresAt > std::chrono::system_clock::now()
						&& prompt.sessionId == session.sessionId
						&& prompt.clientName == session.clientName
						&& prompt.argumentsFingerprint == StableApprovalFingerprint(prompt.arguments.dump())
						&& prompt.impactFingerprint == currentImpactFingerprint
						&& prompt.projectGeneration == state.projectGeneration
						&& prompt.sceneGeneration == state.sceneGeneration
						&& (prompt.worldGeneration == 0U || prompt.worldGeneration == currentWorldGeneration);
					prompt.resolved = true;
					prompt.approved = approved && bindingStillValid;
					prompt.dispatchCompleted = !prompt.approved;
					if (!approved)
					{
						prompt.resolutionState = "denied";
					}
					else if (!bindingStillValid)
					{
						prompt.resolutionState = "binding_invalid";
					}
					else
					{
						// The prompt disappears from the actionable UI before the
						// handler runs, while recent-approval readers see an honest
						// in-progress state until the exact result is retained.
						prompt.resolutionState = "dispatching";
					}
					resolvedPrompt = prompt;
					capturedWriteMode = writeMode;
					break;
				}
			}
		}
		if (!resolvedPrompt)
		{
			return false;
		}
		ApprovalPrompt prompt = std::move(*resolvedPrompt);
		if (approved && !bindingStillValid)
		{
			RecordActivity(ActivityEventType::Rejection, prompt.requestId, prompt.capability,
				"Approval binding expired or its session/generation/fingerprint changed.", "rejected");
			return true;
		}
		RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability,
			approved ? "Approved request." : "Denied request.", approved ? "completed" : "rejected");
		if (approved)
		{
			if (prompt.capability.starts_with("physics."))
			{
				prompt.arguments["approval"] = {
					{"approvalId", prompt.approvalId},
					{"impactFingerprint", prompt.impactFingerprint},
					{"projectGeneration", prompt.projectGeneration},
					{"sceneGeneration", prompt.sceneGeneration}
				};
			}
			nlohmann::json result;
			try
			{
				result = service.HandleTool(
					prompt.capability, prompt.arguments, capturedWriteMode, prompt.requestId);
			}
			catch (const std::exception& exception)
			{
				// A provider exception must not strand a resolved approval forever
				// in "dispatching". Convert it into the same correlated envelope a
				// loopback client can safely inspect and report.
				result = service.BuildError(prompt.requestId, ErrorCategory::Internal,
					std::string("Approved handler failed unexpectedly: ") + exception.what());
			}
			catch (...)
			{
				result = service.BuildError(prompt.requestId, ErrorCategory::Internal,
					"Approved handler failed with an unknown exception.");
			}
			{
				std::lock_guard lock(stateMutex);
				const auto retained = std::find_if(approvals.begin(), approvals.end(),
					[&prompt](const ApprovalPrompt& candidate) {
						return candidate.approvalId == prompt.approvalId;
					});
				if (retained != approvals.end())
				{
					retained->dispatchResult = result;
					retained->dispatchCompleted = true;
					retained->resolutionState = "approved";
				}
			}
			RecordActivity(result.value("ok", false) ? ActivityEventType::Request : ActivityEventType::Rejection,
				prompt.requestId, prompt.capability, result.value("message", ""), result.value("status", "completed"));
		}
		return true;
	}

	bool EditorMcpBridge::CancelApproval(std::string_view approvalId)
	{
		std::optional<ApprovalPrompt> canceledPrompt;
		{
			std::lock_guard lock(stateMutex);
			for (ApprovalPrompt& prompt : approvals)
			{
				if (prompt.approvalId == approvalId && !prompt.resolved)
				{
					prompt.resolved = true;
					prompt.approved = false;
					prompt.dispatchCompleted = true;
					prompt.resolutionState = "canceled";
					canceledPrompt = prompt;
					break;
				}
			}
		}
		if (!canceledPrompt)
		{
			return false;
		}
		RecordActivity(ActivityEventType::Approval, canceledPrompt->requestId, canceledPrompt->capability,
			"Canceled approval prompt.", "canceled");
		return true;
	}

	std::vector<ApprovalPrompt> EditorMcpBridge::GetPendingApprovals() const
	{
		std::vector<ApprovalPrompt> pending;
		std::lock_guard lock(stateMutex);
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
		bool enabled = false;
		WriteMode capturedWriteMode = WriteMode::ReadOnly;
		std::uint16_t capturedConfiguredPort = 0;
		{
			std::lock_guard lock(stateMutex);
			enabled = IsEnabledState(session.state);
			capturedWriteMode = writeMode;
			capturedConfiguredPort = configuredListenPort;
		}
		editorContext->projectManager->SetEditorMcpPreferences(
			enabled, std::string(ToString(capturedWriteMode)), GetListenEndpoint(), capturedConfiguredPort);
	}

	nlohmann::json EditorMcpBridge::BuildApprovalResponse(const AutomationCapabilityDescriptor& capability, const nlohmann::json& arguments, const std::string& requestId)
	{
		ApprovalPrompt prompt = CreateApprovalPromptLocked(requestId, capability.name, "Approval required for " + capability.name, arguments, {});
		if (prompt.approvalId.empty())
		{
			return service.BuildError(requestId, ErrorCategory::Permission,
				"The MCP bridge stopped before the approval could be created.");
		}
		nlohmann::json result;
		result["ok"] = false;
		result["requestId"] = requestId;
		result["status"] = ToString(RequestStatus::WaitingApproval);
		result["message"] = "Approval is required before this request can run.";
		result["data"] = {
			{"approvalId", prompt.approvalId},
			{"capability", prompt.capability},
			{"impactFingerprint", prompt.impactFingerprint},
			{"projectGeneration", prompt.projectGeneration},
			{"sceneGeneration", prompt.sceneGeneration},
			{"worldGeneration", prompt.worldGeneration}
		};
		result["error"] = nullptr;
		return result;
	}

	void EditorMcpBridge::ProcessPendingToolCalls()
	{
		for (;;)
		{
			std::shared_ptr<PendingToolCall> call;
			bool notify = false;
			{
				// One lifecycle acquisition owns exactly one provider dispatch.
				// Disable/shutdown can therefore acquire the gate between handlers
				// and drain every request that has not entered Processing.
				std::lock_guard lifecycleLock(lifecycleMutex);
				{
					std::lock_guard lock(pendingToolCallsMutex);
					if (!acceptingToolCalls || pendingToolCalls.empty())
					{
						return;
					}
					call = pendingToolCalls.front();
					pendingToolCalls.pop_front();
				}
				{
					std::lock_guard callLock(call->mutex);
					if (call->state == PendingToolCall::State::Abandoned
						|| call->state == PendingToolCall::State::Completed)
					{
						continue;
					}
					call->state = PendingToolCall::State::Processing;
				}
				nlohmann::json result = CallToolLocked(call->name, call->arguments);
				{
					std::lock_guard callLock(call->mutex);
					call->result = std::move(result);
					call->state = PendingToolCall::State::Completed;
					notify = true;
				}
			}
			if (notify)
			{
				call->completedSignal.notify_all();
			}
		}
	}

	bool EditorMcpBridge::CancelQueuedRequest(std::string_view requestId)
	{
		std::optional<AssistantRequest> canceledRequest;
		{
			std::lock_guard lock(stateMutex);
			for (AssistantRequest& request : requestQueue)
			{
				if (request.requestId == requestId && !request.canceled)
				{
					request.canceled = true;
					request.status = RequestStatus::Canceled;
					canceledRequest = request;
					break;
				}
			}
		}
		if (!canceledRequest)
		{
			return false;
		}
		RecordActivity(ActivityEventType::Request, canceledRequest->requestId, canceledRequest->capability,
			"Canceled queued request.", "canceled");
		return true;
	}

	void EditorMcpBridge::ExpirePendingApprovals()
	{
		const auto now = std::chrono::system_clock::now();
		std::vector<ApprovalPrompt> expiredPrompts;
		{
			std::lock_guard lock(stateMutex);
			for (ApprovalPrompt& prompt : approvals)
			{
				if (!prompt.resolved && prompt.expiresAt <= now)
				{
					prompt.resolved = true;
					prompt.approved = false;
					prompt.dispatchCompleted = true;
					prompt.resolutionState = "expired";
					expiredPrompts.push_back(prompt);
				}
			}
		}
		for (const ApprovalPrompt& prompt : expiredPrompts)
		{
			RecordActivity(ActivityEventType::Approval, prompt.requestId, prompt.capability,
				"Approval prompt expired.", "canceled");
		}
	}
}
