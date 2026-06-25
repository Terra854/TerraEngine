// EditorAutomationTypes.h - Shared data contracts for editor assistant automation.
//
// These types are intentionally small value objects. They let tests, the MCP
// transport, the bridge, and the editor window talk about the same request,
// capability, approval, and activity concepts without sharing UI or socket code.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

namespace TerraEngine::Editor::Automation
{
	enum class BridgeState
	{
		Disabled,
		Listening,
		Connected,
		Stopping,
		Stopped
	};

	enum class WriteMode
	{
		ReadOnly,
		WritesEnabled
	};

	enum class RequestStatus
	{
		Queued,
		Running,
		WaitingApproval,
		Completed,
		Failed,
		Rejected,
		Canceled
	};

	enum class MutationMode
	{
		ReadOnly,
		EditorSelection,
		SceneMutation,
		AssetMutation,
		ExportMutation,
		Destructive
	};

	enum class ErrorCategory
	{
		Validation,
		Permission,
		MissingTarget,
		UnsupportedCapability,
		Conflict,
		Transport,
		Internal,
		Canceled
	};

	enum class ActivityEventType
	{
		Connection,
		Rejection,
		Configuration,
		Request,
		Approval,
		Mutation,
		Diagnostic,
		Export
	};

	struct BridgeSession
	{
		std::string sessionId;
		std::string clientName;
		std::string peerAddress = "127.0.0.1";
		BridgeState state = BridgeState::Disabled;
		std::uint64_t connectedClientCount = 0;
	};

	struct AssistantCapability
	{
		std::string name;
		std::string kind = "tool";
		std::string category;
		std::string inputSummary;
		MutationMode mutationMode = MutationMode::ReadOnly;
		bool mutatesState = false;
		bool requiresProject = false;
		bool requiresScene = false;
		bool requiresApproval = false;
	};

	struct AssistantRequest
	{
		std::string requestId;
		std::string capability;
		nlohmann::json arguments = nlohmann::json::object();
		RequestStatus status = RequestStatus::Queued;
		bool canceled = false;
	};

	struct ActivityRecord
	{
		std::uint64_t sequence = 0;
		std::string requestId;
		std::string capability;
		std::string message;
		std::string outcome;
		ActivityEventType type = ActivityEventType::Request;
		std::chrono::system_clock::time_point recordedAt = std::chrono::system_clock::now();
	};

	struct ApprovalPrompt
	{
		std::string approvalId;
		std::string requestId;
		std::string capability;
		std::string summary;
		nlohmann::json arguments = nlohmann::json::object();
		bool resolved = false;
		bool approved = false;
		std::chrono::system_clock::time_point expiresAt = std::chrono::system_clock::now();
	};

	struct EditorTarget
	{
		std::string input;
		std::string displayName;
		std::string uuid;
		std::optional<entt::entity> entity;
		bool resolved = false;
	};

	struct DiagnosticSnapshot
	{
		std::string surface;
		std::string state = "unavailable";
		nlohmann::json details = nlohmann::json::object();
	};

	struct ExportJob
	{
		std::string jobId;
		std::string profile;
		std::string state = "idle";
		float progress = 0.0f;
		bool cancelRequested = false;
	};

	const char* ToString(BridgeState value);
	const char* ToString(WriteMode value);
	const char* ToString(RequestStatus value);
	const char* ToString(MutationMode value);
	const char* ToString(ErrorCategory value);
	const char* ToString(ActivityEventType value);
	std::optional<WriteMode> ParseWriteMode(std::string_view value);
}
