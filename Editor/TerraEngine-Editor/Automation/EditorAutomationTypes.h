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
		InvalidCameraObject,
		UnsupportedCapability,
		UnsupportedReadback,
		QueueFull,
		OverBudget,
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
		Capture,
		Export
	};

	enum class McpCaptureSourceKind
	{
		GameView,
		CameraObject,
		CustomCamera
	};

	// Limits are mirrored into MCP availability and validation responses so
	// agents can size captures before spending a tool call on an invalid image.
	struct McpCaptureLimits
	{
		std::uint32_t defaultWidth = 1280u;
		std::uint32_t defaultHeight = 720u;
		// Null maxima tell MCP clients there is no fixed resolution policy. The
		// renderer still rejects malformed dimensions and reports backend errors
		// if a requested output cannot be allocated or encoded.
		std::optional<std::uint32_t> maxWidth;
		std::optional<std::uint32_t> maxHeight;
		std::uint32_t maxConcurrentCaptures = 1u;
		std::uint32_t maxQueuedCaptures = 4u;
		std::uint32_t defaultTimeoutMs = 2000u;
	};

	// Camera-object captures keep object identity and active-state facts beside
	// the request because disabled cameras are valid authored targets when
	// includeDisabled is true.
	struct McpCameraObjectReference
	{
		std::string target;
		std::string displayName;
		std::string objectId;
		std::optional<entt::entity> entity;
		bool objectActive = true;
		bool cameraComponentActive = true;
		bool includeDisabled = true;
	};

	// Custom cameras are ad-hoc view definitions supplied entirely by the MCP
	// caller. They do not create scene objects or mutate existing cameras.
	struct McpCustomCameraDefinition
	{
		std::vector<float> position;
		std::vector<float> lookAt;
		std::vector<float> forward;
		std::vector<float> up;
		std::string projection = "perspective";
		float verticalFovDegrees = 45.0f;
		float orthographicHeight = 10.0f;
		float nearPlane = 0.1f;
		float farPlane = 1000.0f;
	};

	// Normalized capture request used by the service before converting to the
	// renderer's typed queue request. Keeping this value object separate from
	// raw JSON makes validation testable without invoking the transport.
	struct McpCaptureRequest
	{
		std::string requestId;
		McpCaptureSourceKind sourceKind = McpCaptureSourceKind::GameView;
		McpCaptureLimits limits;
		std::uint32_t width = 1280u;
		std::uint32_t height = 720u;
		std::string format = "image/png";
		std::string freshness = "latestCompleted";
		bool includeOverlays = false;
		std::uint32_t timeoutMs = 2000u;
		McpCameraObjectReference cameraObject;
		McpCustomCameraDefinition customCamera;
	};

	// Result metadata paired with MCP image content. The binary image itself is
	// returned as an MCP image content item rather than embedded in this struct.
	struct McpCapturedImageResult
	{
		std::string requestId;
		bool ok = false;
		std::string mimeType = "image/png";
		std::uint32_t width = 0u;
		std::uint32_t height = 0u;
		std::string sourceKind;
		std::string sourceName;
		std::string base64Data;
		std::string errorCategory;
		std::string message;
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
