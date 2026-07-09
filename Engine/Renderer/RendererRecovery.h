// RendererRecovery.h - backend-neutral renderer recovery state.
//
// Device loss can make Vulkan, ImGui, and viewport render targets unavailable.
// These value types deliberately stay CPU-only so editor windows, export logs,
// automation, and tests can still explain what happened without touching stale
// GPU handles.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Renderer
{
	enum class RendererRecoveryStateKind : std::uint8_t
	{
		Active,
		DeviceLost,
		SavingSafeData,
		RebuildPending,
		RebuildInProgress,
		RebuildSucceeded,
		RebuildFailed,
		RendererRecoveryFailed,
		FatalExportFailure
	};

	enum class RendererDeviceFailureStage : std::uint8_t
	{
		Unknown,
		Submit,
		Present,
		Acquire,
		FenceWait,
		DeviceIdle,
		QueueIdle,
		Upload,
		DescriptorResourceMutation,
		Resize,
		SwapchainRecovery,
		RenderTargetRecreation,
		RebuildCreateDevice,
		RebuildRestoreDefaults,
		RebuildRestoreResources,
		RebuildPublishSession
	};

	enum class RendererRecoveryAffectedView : std::uint8_t
	{
		None,
		SceneView,
		GameView,
		EditorUi,
		ExportRuntime,
		OffscreenCapture
	};

	enum class RendererRecoveryAction : std::uint8_t
	{
		None,
		RecordIncident,
		SaveSafeData,
		AutomaticRebuild,
		UserRequestedRebuild,
		ExportPolicyRebuild,
		FatalDialog
	};

	enum class RendererRecoveryOutcome : std::uint8_t
	{
		None,
		DeviceLostLatched,
		RebuildStarted,
		RebuildSucceeded,
		RebuildFailed,
		RecoveryFailed,
		FatalExportFailure
	};

	enum class RendererRecoverySessionPhase : std::uint8_t
	{
		None,
		Plan,
		Teardown,
		CreateDevice,
		RestoreDefaults,
		RestoreResources,
		RestoreViews,
		Publish,
		Failed,
		Complete
	};

	struct RendererDeviceIncident
	{
		std::string incidentId;
		RendererDeviceFailureStage firstFailureStage = RendererDeviceFailureStage::Unknown;
		std::string firstResult;
		RendererRecoveryAffectedView affectedView = RendererRecoveryAffectedView::None;
		std::string affectedResourceId;
		std::string affectedResourceName;
		std::string hardwareName;
		std::string driverVersion;
		std::string apiVersion;
		std::uint64_t frameId = 0u;
		std::uint64_t renderSessionGeneration = 0u;
		std::uint32_t duplicateFailureCount = 0u;
		std::string createdAtUtc;
		RendererRecoveryOutcome finalOutcome = RendererRecoveryOutcome::None;
	};

	struct RecoverySession
	{
		std::string sessionId;
		std::string incidentId;
		RendererRecoveryAction requestedBy = RendererRecoveryAction::None;
		std::string startedAtUtc;
		RendererRecoverySessionPhase phase = RendererRecoverySessionPhase::None;
		std::uint32_t attemptNumber = 0u;
		std::uint32_t maxAttemptsForIncident = 0u;
		std::string failureReason;
		std::uint64_t restoredRenderSessionGeneration = 0u;
	};

	struct ResourceRestorationPlan
	{
		std::uint64_t renderSessionGeneration = 0u;
		std::vector<std::string> requiredShaders;
		std::vector<std::string> defaultResources;
		std::vector<std::string> visibleMeshes;
		std::vector<std::string> visibleMaterials;
		std::vector<std::string> visibleTextures;
		std::vector<std::string> virtualGeometryResidency;
		std::vector<std::string> textureResidency;
		std::vector<std::string> viewTargets;
		std::vector<std::string> profilerAndDiagnosticResources;
		std::vector<std::string> deferredUploads;
		std::vector<std::string> missingOrDeferredResources;
	};

	struct RendererRecoveryState
	{
		RendererRecoveryStateKind state = RendererRecoveryStateKind::Active;
		std::string incidentId;
		std::string sessionId;
		bool canSaveProjectData = false;
		bool canAttemptRebuild = false;
		bool canRetryRendererRebuild = false;
		std::string message;
		std::string lastError;
		std::uint64_t updatedFrameId = 0u;
	};

	struct RendererRecoverySnapshot
	{
		std::uint64_t renderSessionGeneration = 1u;
		RendererRecoveryStateKind state = RendererRecoveryStateKind::Active;
		std::string incidentId;
		std::string sessionId;
		std::string message = "Renderer active.";
		RendererDeviceFailureStage firstFailureStage = RendererDeviceFailureStage::Unknown;
		std::string firstFailureResult;
		RendererRecoveryAffectedView affectedView = RendererRecoveryAffectedView::None;
		std::string affectedResourceId;
		std::string affectedResourceName;
		std::string hardwareName;
		std::string driverVersion;
		std::string apiVersion;
		std::uint64_t frameId = 0u;
		std::uint32_t duplicateFailureCount = 0u;
		bool canSaveProjectData = false;
		bool canAttemptRebuild = false;
		bool canRetryRendererRebuild = false;
		RendererRecoveryAction selectedAction = RendererRecoveryAction::None;
		RendererRecoveryOutcome finalOutcome = RendererRecoveryOutcome::None;
		std::string lastError;
		std::string fatalDialogReportId;
		bool fatalDialogShown = false;
		std::string crashDumpPath;
		std::string crashSummarySidecarPath;
		std::string crashDumpStatus;
	};

	struct RecoveryDiagnosticSnapshot
	{
		RendererRecoverySnapshot recoveryState;
		RendererDeviceIncident incident;
		RecoverySession activeSession;
		std::vector<std::string> recentRenderPhases;
		std::vector<std::string> resourceRestorationSummary;
		std::uint32_t suppressedFailures = 0u;
		std::vector<std::string> safeActions;
		std::string exportPolicyDecision;
	};

	struct ResourceRestorationPlanInput
	{
		std::uint64_t renderSessionGeneration = 0u;
		std::vector<std::string> requiredShaders;
		std::vector<std::string> defaultResources;
		std::vector<std::string> visibleMeshes;
		std::vector<std::string> visibleMaterials;
		std::vector<std::string> visibleTextures;
		std::vector<std::string> virtualGeometryResidency;
		std::vector<std::string> textureResidency;
		std::vector<std::string> viewTargets;
		std::vector<std::string> profilerAndDiagnosticResources;
		std::vector<std::string> deferredUploads;
		std::vector<std::string> missingOrDeferredResources;
	};

	TERRAENGINE_API const char* ToString(RendererRecoveryStateKind state);
	TERRAENGINE_API const char* ToString(RendererDeviceFailureStage stage);
	TERRAENGINE_API const char* ToString(RendererRecoveryAffectedView view);
	TERRAENGINE_API const char* ToString(RendererRecoveryAction action);
	TERRAENGINE_API const char* ToString(RendererRecoveryOutcome outcome);
	TERRAENGINE_API const char* ToString(RendererRecoverySessionPhase phase);

	TERRAENGINE_API bool IsRendererRecoveryActive(RendererRecoveryStateKind state);
	TERRAENGINE_API bool CanSubmitNormalGpuWork(const RendererRecoverySnapshot& snapshot);
	TERRAENGINE_API std::string ValidateRecoverySnapshot(const RendererRecoverySnapshot& snapshot);
	TERRAENGINE_API RendererDeviceFailureStage ClassifyFailureStage(std::string_view operation);
	TERRAENGINE_API RendererRecoveryAffectedView ClassifyAffectedView(std::string_view operation);
	TERRAENGINE_API RendererRecoverySnapshot MakeActiveRecoverySnapshot(std::uint64_t renderSessionGeneration = 1u);
	TERRAENGINE_API RendererRecoverySnapshot RecordDeviceLossIncident(
		const RendererRecoverySnapshot& previous,
		std::string_view result,
		std::string_view operation,
		std::uint64_t frameId,
		std::uint64_t renderSessionGeneration,
		RendererRecoveryAffectedView affectedView = RendererRecoveryAffectedView::None,
		std::string affectedResourceId = {},
		std::string affectedResourceName = {});
	TERRAENGINE_API RendererRecoverySnapshot BeginRecoveryRebuild(
		const RendererRecoverySnapshot& previous,
		std::uint32_t attemptNumber,
		std::uint32_t maxAttempts,
		RendererRecoveryAction requestedBy);
	TERRAENGINE_API RendererRecoverySnapshot CompleteRecoveryRebuild(
		const RendererRecoverySnapshot& previous,
		bool success,
		std::uint64_t restoredRenderSessionGeneration,
		std::string_view failureReason = {});
	TERRAENGINE_API RendererRecoverySnapshot AttachFatalDialogReport(
		const RendererRecoverySnapshot& previous,
		std::string_view reportId,
		std::string_view crashDumpPath,
		std::string_view crashSummarySidecarPath,
		std::string_view crashDumpStatus);
	TERRAENGINE_API ResourceRestorationPlan BuildResourceRestorationPlan(ResourceRestorationPlanInput input);
	TERRAENGINE_API RecoveryDiagnosticSnapshot BuildRecoveryDiagnosticSnapshot(
		const RendererRecoverySnapshot& snapshot,
		std::vector<std::string> recentRenderPhases = {},
		std::vector<std::string> resourceRestorationSummary = {});
}
