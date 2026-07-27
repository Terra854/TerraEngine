// RendererRecovery.h - backend-neutral renderer failure state.
//
// Device loss can make Vulkan, ImGui, and viewport render targets unavailable.
// These value types deliberately stay CPU-only so editor windows, export logs,
// automation, and tests can still explain the terminal renderer failure without
// touching invalid GPU handles.

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
		RenderTargetRecreation
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
		FatalDialog
	};

	enum class RendererRecoveryOutcome : std::uint8_t
	{
		None,
		DeviceLostLatched,
		FatalExportFailure
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

	struct RendererRecoverySnapshot
	{
		std::uint64_t renderSessionGeneration = 1u;
		RendererRecoveryStateKind state = RendererRecoveryStateKind::Active;
		std::string incidentId;
		// Retained for diagnostic schema compatibility. Terminal device-loss
		// handling does not create a secondary renderer session, so this stays empty.
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
		// Terminal device loss keeps this false because the process-modal fatal
		// dialog leaves no reachable editor action before termination.
		bool canSaveProjectData = false;
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
		std::vector<std::string> recentRenderPhases;
		std::uint32_t suppressedFailures = 0u;
		std::vector<std::string> safeActions;
	};

	TERRAENGINE_API const char* ToString(RendererRecoveryStateKind state);
	TERRAENGINE_API const char* ToString(RendererDeviceFailureStage stage);
	TERRAENGINE_API const char* ToString(RendererRecoveryAffectedView view);
	TERRAENGINE_API const char* ToString(RendererRecoveryAction action);
	TERRAENGINE_API const char* ToString(RendererRecoveryOutcome outcome);

	TERRAENGINE_API bool IsRendererRecoveryActive(RendererRecoveryStateKind state);
	TERRAENGINE_API bool CanSubmitNormalGpuWork(const RendererRecoverySnapshot& snapshot);
	// These helpers validate a recovered renderer snapshot without creating one.
	// They preserve the terminal device-loss policy: only the renderer owner may
	// supply a later Active generation after it has recreated its own backend.
	TERRAENGINE_API bool IsRendererGenerationCurrent(
		const RendererRecoverySnapshot& snapshot,
		std::uint64_t renderSessionGeneration);
	TERRAENGINE_API bool CanAcceptRecoveredGpuResources(
		const RendererRecoverySnapshot& previous,
		const RendererRecoverySnapshot& recovered);
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
	TERRAENGINE_API RendererRecoverySnapshot AttachFatalDialogReport(
		const RendererRecoverySnapshot& previous,
		std::string_view reportId,
		std::string_view crashDumpPath,
		std::string_view crashSummarySidecarPath,
		std::string_view crashDumpStatus);
	TERRAENGINE_API RecoveryDiagnosticSnapshot BuildRecoveryDiagnosticSnapshot(
		const RendererRecoverySnapshot& snapshot,
		std::vector<std::string> recentRenderPhases = {});
}
