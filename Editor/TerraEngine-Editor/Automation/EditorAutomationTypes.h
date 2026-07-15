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
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
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
		ProjectMutation,
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
		ApprovalRequired,
		MissingTarget,
		InvalidCameraObject,
		UnsupportedCapability,
		UnsupportedReadback,
		QueueFull,
		OverBudget,
		Conflict,
		StaleTarget,
		Unavailable,
		Transport,
		Internal,
		Canceled,
		Timeout
	};

	// Permissions are separate from the bridge's current write mode. A
	// capability describes what it needs, while the bridge decides whether the
	// current local client is allowed to satisfy that need.
	enum class AutomationPermission
	{
		Read,
		ProjectWrite,
		GlobalWrite
	};

	enum class AutomationApprovalPolicy
	{
		None,
		Destructive,
		HighImpact
	};

	enum class AutomationJobState
	{
		Queued,
		Preparing,
		Running,
		CancelRequested,
		Succeeded,
		Failed,
		Canceled
	};

	enum class AutomationLogSeverity
	{
		Trace,
		Debug,
		Info,
		Warning,
		Error,
		Critical
	};

	enum class IdempotencyDisposition
	{
		NewRequest,
		Replay,
		Conflict,
		CapacityExceeded
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

	// Discovery evaluates these conditions against an immutable snapshot. The
	// optional predicate in a provider may narrow availability further without
	// reading mutable editor state from a worker.
	struct AutomationStateSnapshot
	{
		bool projectAvailable = false;
		bool sceneAvailable = false;
		bool rendererAvailable = false;
		std::string playState = "stopped";
		std::uint64_t projectGeneration = 0;
		std::uint64_t sceneGeneration = 0;
		std::set<std::string> availableServices;
	};

	struct AutomationAvailability
	{
		bool available = true;
		std::string reason;
		bool requiresProject = false;
		bool requiresScene = false;
		bool requiresRenderer = false;
		std::vector<std::string> allowedPlayStates;
	};

	// Scheduler declarations are data, not behavior. Diagnostics can therefore
	// expose the exact worker/protected split without invoking a provider.
	struct ExecutionBoundaryDeclaration
	{
		bool workerPreparation = false;
		bool protectedCommit = false;
		std::string visibilityBoundary;
		std::vector<std::string> preparationReads;
		std::vector<std::string> commitReads;
		std::vector<std::string> commitWrites;
	};

	struct AutomationCapabilityDescriptor
	{
		std::string name;
		std::uint32_t version = 1;
		std::string category;
		std::string description;
		nlohmann::json inputSchema = nlohmann::json{{"type", "object"}};
		nlohmann::json resultSchema = nlohmann::json{{"type", "object"}};
		AutomationPermission permission = AutomationPermission::Read;
		AutomationApprovalPolicy approvalPolicy = AutomationApprovalPolicy::None;
		MutationMode mutationMode = MutationMode::ReadOnly;
		AutomationAvailability availability;
		ExecutionBoundaryDeclaration execution;
		std::string replacement;
		bool deprecated = false;
	};

	struct AutomationRequestContext
	{
		std::string requestId;
		std::string clientId;
		std::string correlationId;
		std::string idempotencyKey;
		nlohmann::json arguments = nlohmann::json::object();
		AutomationStateSnapshot state;
		AutomationPermission grantedPermission = AutomationPermission::Read;
		// Version-one compatibility handlers retain their own detailed availability
		// and fallback validation. Discovery still reports current availability,
		// but dispatch must not replace established error categories prematurely.
		bool preserveLegacyAvailability = false;
	};

	struct AutomationDispatchResult
	{
		bool ok = false;
		ErrorCategory error = ErrorCategory::Internal;
		std::string message;
		nlohmann::json data = nlohmann::json::object();
	};

	struct AutomationArtifact
	{
		std::string artifactId;
		std::string kind;
		std::string displayPath;
		std::string mimeType;
		nlohmann::json metadata = nlohmann::json::object();
	};

	struct AutomationJobDiagnostic
	{
		AutomationLogSeverity severity = AutomationLogSeverity::Info;
		std::string category;
		std::string message;
		nlohmann::json details = nlohmann::json::object();
	};

	struct AutomationJobSnapshot
	{
		std::string jobId;
		std::string kind;
		std::string capability;
		std::string requestId;
		std::string correlationId;
		std::string idempotencyKey;
		AutomationJobState state = AutomationJobState::Queued;
		double progress = 0.0;
		std::string stage = "queued";
		bool cancellable = false;
		bool cancelRequested = false;
		std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
		std::optional<std::chrono::system_clock::time_point> startedAt;
		std::chrono::system_clock::time_point updatedAt = createdAt;
		std::optional<std::chrono::system_clock::time_point> completedAt;
		std::vector<AutomationJobDiagnostic> diagnostics;
		std::vector<AutomationArtifact> artifacts;
		nlohmann::json result;
		std::uint64_t retentionSequence = 0;
	};

	struct AutomationJobCreateRequest
	{
		std::string kind;
		std::string capability;
		std::string requestId;
		std::string correlationId;
		std::string idempotencyKey;
		bool cancellable = false;
	};

	struct StructuredLogRecord
	{
		std::uint64_t sequence = 0;
		std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
		AutomationLogSeverity severity = AutomationLogSeverity::Info;
		std::string category;
		std::string source;
		std::string correlationId;
		std::string requestId;
		std::string jobId;
		std::string message;
		nlohmann::json details = nlohmann::json::object();
	};

	struct StructuredLogQuery
	{
		std::optional<AutomationLogSeverity> minimumSeverity;
		std::string category;
		std::string source;
		std::string text;
		std::string correlationId;
		std::string requestId;
		std::string jobId;
		std::optional<std::chrono::system_clock::time_point> from;
		std::optional<std::chrono::system_clock::time_point> to;
		std::string cursor;
		std::size_t pageSize = 100;
	};

	struct StructuredLogPage
	{
		bool valid = true;
		std::string error;
		std::vector<StructuredLogRecord> records;
		std::string nextCursor;
		std::uint64_t oldestRetainedSequence = 0;
		bool droppedBeforeCursor = false;
		bool hasMore = false;
	};

	struct IdempotencyRequest
	{
		std::string clientScope;
		std::string capability;
		std::string key;
		std::string fingerprint;
		nlohmann::json initialResult;
	};

	struct IdempotencyDecision
	{
		IdempotencyDisposition disposition = IdempotencyDisposition::NewRequest;
		nlohmann::json result;
		bool terminal = false;
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
		std::string argumentsFingerprint;
		std::string impactFingerprint;
		std::string sessionId;
		std::string clientName;
		std::uint64_t projectGeneration = 0;
		std::uint64_t sceneGeneration = 0;
		std::uint32_t worldGeneration = 0;
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

	// Performance automation records keep configured preferences separate from
	// their effective runtime value. This distinction prevents clients from
	// mistaking an automatic large-scene safeguard for a user-authored setting.
	struct PerformanceSettingState
	{
		std::string name;
		nlohmann::json configuredValue;
		nlohmann::json effectiveValue;
		std::string effectiveReason;
	};

	// A reduction is an intentional presentation or diagnostic simplification.
	// It never represents missing physics state or a failed measurement.
	struct PerformanceReduction
	{
		std::string id;
		std::string reason;
		std::uint64_t affectedItems = 0;
		bool essentialInteractionPreserved = true;
	};

	// Cache decisions are additive diagnostics used by scene, managed, overlay,
	// and profiler automation results without exposing mutable cache storage.
	struct PerformanceCacheDecision
	{
		std::string cache;
		bool hit = false;
		std::string reason;
		std::string identity;
	};

	struct PerformanceStageEvidence
	{
		std::string stage;
		std::string unit = "nanoseconds";
		std::uint64_t samples = 0;
		std::uint64_t median = 0;
		std::uint64_t p95 = 0;
		std::uint64_t p99 = 0;
		std::uint64_t maximum = 0;
		bool available = false;
		std::string unavailableReason;
	};

	// Automation histories are always bounded. The dropped count tells clients
	// that an older sample was intentionally evicted instead of silently lost.
	template<typename T>
	struct BoundedPerformanceHistory
	{
		std::vector<T> samples;
		std::size_t capacity = 0;
		std::uint64_t droppedSamples = 0;
		std::uint32_t intervalFrames = 1;
	};

	struct SceneViewPerformanceState
	{
		std::string configuredLargeSceneMode = "auto";
		bool effectiveLargeSceneMode = false;
		std::string effectiveReason;
		std::vector<PerformanceReduction> reductions;
		std::uint64_t sourceMarkers = 0;
		std::uint64_t candidates = 0;
		std::uint64_t viewRejected = 0;
		std::uint64_t densityConsolidated = 0;
		std::uint64_t budgetRejected = 0;
		std::uint64_t presented = 0;
		std::uint64_t selectedExemptions = 0;
		std::uint64_t cameraExemptions = 0;
		std::uint64_t lightExemptions = 0;
		std::uint64_t presentationBatches = 0;
		BoundedPerformanceHistory<PerformanceStageEvidence> stageHistory;
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
