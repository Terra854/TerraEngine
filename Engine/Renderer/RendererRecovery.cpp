// RendererRecovery.cpp - CPU-only renderer recovery state helpers.

#include "Renderer/RendererRecovery.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace TerraEngine::Renderer
{
	namespace
	{
		std::string MakeIncidentId(std::uint64_t generation, std::uint64_t frameId)
		{
			return "renderer-incident-" + std::to_string(generation) + "-" + std::to_string(frameId);
		}

		std::string MakeSessionId(std::string_view incidentId, std::uint32_t attemptNumber)
		{
			return std::string(incidentId) + "-rebuild-" + std::to_string(attemptNumber);
		}

		bool ContainsToken(std::string_view text, std::string_view token)
		{
			return text.find(token) != std::string_view::npos;
		}
	}

	const char* ToString(RendererRecoveryStateKind state)
	{
		switch (state)
		{
		case RendererRecoveryStateKind::Active: return "Active";
		case RendererRecoveryStateKind::DeviceLost: return "DeviceLost";
		case RendererRecoveryStateKind::SavingSafeData: return "SavingSafeData";
		case RendererRecoveryStateKind::RebuildPending: return "RebuildPending";
		case RendererRecoveryStateKind::RebuildInProgress: return "RebuildInProgress";
		case RendererRecoveryStateKind::RebuildSucceeded: return "RebuildSucceeded";
		case RendererRecoveryStateKind::RebuildFailed: return "RebuildFailed";
		case RendererRecoveryStateKind::RendererRecoveryFailed: return "RendererRecoveryFailed";
		case RendererRecoveryStateKind::FatalExportFailure: return "FatalExportFailure";
		}
		return "Unknown";
	}

	const char* ToString(RendererDeviceFailureStage stage)
	{
		switch (stage)
		{
		case RendererDeviceFailureStage::Unknown: return "Unknown";
		case RendererDeviceFailureStage::Submit: return "Submit";
		case RendererDeviceFailureStage::Present: return "Present";
		case RendererDeviceFailureStage::Acquire: return "Acquire";
		case RendererDeviceFailureStage::FenceWait: return "FenceWait";
		case RendererDeviceFailureStage::DeviceIdle: return "DeviceIdle";
		case RendererDeviceFailureStage::QueueIdle: return "QueueIdle";
		case RendererDeviceFailureStage::Upload: return "Upload";
		case RendererDeviceFailureStage::DescriptorResourceMutation: return "DescriptorResourceMutation";
		case RendererDeviceFailureStage::Resize: return "Resize";
		case RendererDeviceFailureStage::SwapchainRecovery: return "SwapchainRecovery";
		case RendererDeviceFailureStage::RenderTargetRecreation: return "RenderTargetRecreation";
		case RendererDeviceFailureStage::RebuildCreateDevice: return "RebuildCreateDevice";
		case RendererDeviceFailureStage::RebuildRestoreDefaults: return "RebuildRestoreDefaults";
		case RendererDeviceFailureStage::RebuildRestoreResources: return "RebuildRestoreResources";
		case RendererDeviceFailureStage::RebuildPublishSession: return "RebuildPublishSession";
		}
		return "Unknown";
	}

	const char* ToString(RendererRecoveryAffectedView view)
	{
		switch (view)
		{
		case RendererRecoveryAffectedView::None: return "None";
		case RendererRecoveryAffectedView::SceneView: return "SceneView";
		case RendererRecoveryAffectedView::GameView: return "GameView";
		case RendererRecoveryAffectedView::EditorUi: return "EditorUi";
		case RendererRecoveryAffectedView::ExportRuntime: return "ExportRuntime";
		case RendererRecoveryAffectedView::OffscreenCapture: return "OffscreenCapture";
		}
		return "None";
	}

	const char* ToString(RendererRecoveryAction action)
	{
		switch (action)
		{
		case RendererRecoveryAction::None: return "None";
		case RendererRecoveryAction::RecordIncident: return "RecordIncident";
		case RendererRecoveryAction::SaveSafeData: return "SaveSafeData";
		case RendererRecoveryAction::AutomaticRebuild: return "AutomaticRebuild";
		case RendererRecoveryAction::UserRequestedRebuild: return "UserRequestedRebuild";
		case RendererRecoveryAction::ExportPolicyRebuild: return "ExportPolicyRebuild";
		case RendererRecoveryAction::FatalDialog: return "FatalDialog";
		}
		return "None";
	}

	const char* ToString(RendererRecoveryOutcome outcome)
	{
		switch (outcome)
		{
		case RendererRecoveryOutcome::None: return "None";
		case RendererRecoveryOutcome::DeviceLostLatched: return "DeviceLostLatched";
		case RendererRecoveryOutcome::RebuildStarted: return "RebuildStarted";
		case RendererRecoveryOutcome::RebuildSucceeded: return "RebuildSucceeded";
		case RendererRecoveryOutcome::RebuildFailed: return "RebuildFailed";
		case RendererRecoveryOutcome::RecoveryFailed: return "RecoveryFailed";
		case RendererRecoveryOutcome::FatalExportFailure: return "FatalExportFailure";
		}
		return "None";
	}

	const char* ToString(RendererRecoverySessionPhase phase)
	{
		switch (phase)
		{
		case RendererRecoverySessionPhase::None: return "None";
		case RendererRecoverySessionPhase::Plan: return "Plan";
		case RendererRecoverySessionPhase::Teardown: return "Teardown";
		case RendererRecoverySessionPhase::CreateDevice: return "CreateDevice";
		case RendererRecoverySessionPhase::RestoreDefaults: return "RestoreDefaults";
		case RendererRecoverySessionPhase::RestoreResources: return "RestoreResources";
		case RendererRecoverySessionPhase::RestoreViews: return "RestoreViews";
		case RendererRecoverySessionPhase::Publish: return "Publish";
		case RendererRecoverySessionPhase::Failed: return "Failed";
		case RendererRecoverySessionPhase::Complete: return "Complete";
		}
		return "None";
	}

	bool IsRendererRecoveryActive(RendererRecoveryStateKind state)
	{
		return state != RendererRecoveryStateKind::Active
			&& state != RendererRecoveryStateKind::RebuildSucceeded;
	}

	bool CanSubmitNormalGpuWork(const RendererRecoverySnapshot& snapshot)
	{
		return snapshot.state == RendererRecoveryStateKind::Active
			|| snapshot.state == RendererRecoveryStateKind::RebuildSucceeded;
	}

	std::string ValidateRecoverySnapshot(const RendererRecoverySnapshot& snapshot)
	{
		if (snapshot.state == RendererRecoveryStateKind::Active && !snapshot.incidentId.empty())
		{
			return "Active recovery snapshots must not retain an incident id.";
		}
		if (snapshot.state == RendererRecoveryStateKind::RebuildInProgress
			&& (snapshot.incidentId.empty() || snapshot.sessionId.empty()))
		{
			return "RebuildInProgress requires both incident id and session id.";
		}
		if (snapshot.state == RendererRecoveryStateKind::RebuildSucceeded
			&& snapshot.renderSessionGeneration == 0u)
		{
			return "RebuildSucceeded requires a fresh render session generation.";
		}
		if (snapshot.state == RendererRecoveryStateKind::RebuildFailed
			&& snapshot.lastError.empty())
		{
			return "RebuildFailed requires a failure reason.";
		}
		return {};
	}

	RendererDeviceFailureStage ClassifyFailureStage(std::string_view operation)
	{
		if (ContainsToken(operation, "submit")) return RendererDeviceFailureStage::Submit;
		if (ContainsToken(operation, "present")) return RendererDeviceFailureStage::Present;
		if (ContainsToken(operation, "acquire")) return RendererDeviceFailureStage::Acquire;
		if (ContainsToken(operation, "fence wait")) return RendererDeviceFailureStage::FenceWait;
		if (ContainsToken(operation, "device idle")) return RendererDeviceFailureStage::DeviceIdle;
		if (ContainsToken(operation, "queue idle")) return RendererDeviceFailureStage::QueueIdle;
		if (ContainsToken(operation, "upload") || ContainsToken(operation, "immediate command")) return RendererDeviceFailureStage::Upload;
		if (ContainsToken(operation, "descriptor")) return RendererDeviceFailureStage::DescriptorResourceMutation;
		if (ContainsToken(operation, "resize")) return RendererDeviceFailureStage::Resize;
		if (ContainsToken(operation, "swapchain")) return RendererDeviceFailureStage::SwapchainRecovery;
		if (ContainsToken(operation, "offscreen") || ContainsToken(operation, "render target")) return RendererDeviceFailureStage::RenderTargetRecreation;
		return RendererDeviceFailureStage::Unknown;
	}

	RendererRecoveryAffectedView ClassifyAffectedView(std::string_view operation)
	{
		if (ContainsToken(operation, "scene")) return RendererRecoveryAffectedView::SceneView;
		if (ContainsToken(operation, "game") || ContainsToken(operation, "runtime")) return RendererRecoveryAffectedView::GameView;
		if (ContainsToken(operation, "editor UI") || ContainsToken(operation, "UI queue")) return RendererRecoveryAffectedView::EditorUi;
		if (ContainsToken(operation, "capture")) return RendererRecoveryAffectedView::OffscreenCapture;
		return RendererRecoveryAffectedView::None;
	}

	RendererRecoverySnapshot MakeActiveRecoverySnapshot(std::uint64_t renderSessionGeneration)
	{
		RendererRecoverySnapshot snapshot;
		snapshot.renderSessionGeneration = std::max<std::uint64_t>(1u, renderSessionGeneration);
		snapshot.state = RendererRecoveryStateKind::Active;
		snapshot.message = "Renderer active.";
		return snapshot;
	}

	RendererRecoverySnapshot RecordDeviceLossIncident(
		const RendererRecoverySnapshot& previous,
		std::string_view result,
		std::string_view operation,
		std::uint64_t frameId,
		std::uint64_t renderSessionGeneration,
		RendererRecoveryAffectedView affectedView,
		std::string affectedResourceId,
		std::string affectedResourceName)
	{
		RendererRecoverySnapshot snapshot = previous;
		snapshot.renderSessionGeneration = std::max<std::uint64_t>(1u, renderSessionGeneration);
		snapshot.frameId = frameId;
		// Once the first incident exists, later Vulkan errors are symptoms. We
		// count them for diagnostics while preserving the original root cause.
		if (!snapshot.incidentId.empty() && snapshot.state != RendererRecoveryStateKind::Active)
		{
			++snapshot.duplicateFailureCount;
			snapshot.message = "Renderer device loss already latched; suppressing duplicate GPU failure.";
			return snapshot;
		}

		snapshot.state = RendererRecoveryStateKind::DeviceLost;
		snapshot.incidentId = MakeIncidentId(snapshot.renderSessionGeneration, frameId);
		snapshot.sessionId.clear();
		snapshot.message = "Graphics device lost; rendering is paused while renderer recovery is prepared.";
		snapshot.firstFailureStage = ClassifyFailureStage(operation);
		snapshot.firstFailureResult = std::string(result);
		snapshot.affectedView = affectedView == RendererRecoveryAffectedView::None
			? ClassifyAffectedView(operation)
			: affectedView;
		snapshot.affectedResourceId = std::move(affectedResourceId);
		snapshot.affectedResourceName = std::move(affectedResourceName);
		snapshot.duplicateFailureCount = 0u;
		snapshot.canSaveProjectData = true;
		snapshot.canAttemptRebuild = true;
		snapshot.canRetryRendererRebuild = true;
		snapshot.selectedAction = RendererRecoveryAction::RecordIncident;
		snapshot.finalOutcome = RendererRecoveryOutcome::DeviceLostLatched;
		snapshot.lastError.clear();
		return snapshot;
	}

	RendererRecoverySnapshot BeginRecoveryRebuild(
		const RendererRecoverySnapshot& previous,
		std::uint32_t attemptNumber,
		std::uint32_t maxAttempts,
		RendererRecoveryAction requestedBy)
	{
		RendererRecoverySnapshot snapshot = previous;
		snapshot.state = RendererRecoveryStateKind::RebuildInProgress;
		snapshot.sessionId = MakeSessionId(snapshot.incidentId, attemptNumber);
		snapshot.message = "Renderer rebuild in progress.";
		snapshot.canAttemptRebuild = false;
		snapshot.canRetryRendererRebuild = attemptNumber < maxAttempts;
		snapshot.selectedAction = requestedBy;
		snapshot.finalOutcome = RendererRecoveryOutcome::RebuildStarted;
		return snapshot;
	}

	RendererRecoverySnapshot CompleteRecoveryRebuild(
		const RendererRecoverySnapshot& previous,
		bool success,
		std::uint64_t restoredRenderSessionGeneration,
		std::string_view failureReason)
	{
		RendererRecoverySnapshot snapshot = previous;
		if (success)
		{
			snapshot = MakeActiveRecoverySnapshot(restoredRenderSessionGeneration);
			snapshot.state = RendererRecoveryStateKind::RebuildSucceeded;
			snapshot.message = "Renderer rebuild succeeded; normal rendering may resume.";
			snapshot.finalOutcome = RendererRecoveryOutcome::RebuildSucceeded;
			return snapshot;
		}

		snapshot.state = snapshot.canRetryRendererRebuild
			? RendererRecoveryStateKind::RebuildFailed
			: RendererRecoveryStateKind::RendererRecoveryFailed;
		snapshot.message = snapshot.canRetryRendererRebuild
			? "Renderer rebuild failed; another bounded attempt is available."
			: "Renderer recovery failed after the bounded rebuild budget was exhausted.";
		snapshot.lastError = failureReason.empty() ? "Renderer rebuild failed." : std::string(failureReason);
		snapshot.canAttemptRebuild = snapshot.canRetryRendererRebuild;
		snapshot.finalOutcome = snapshot.canRetryRendererRebuild
			? RendererRecoveryOutcome::RebuildFailed
			: RendererRecoveryOutcome::RecoveryFailed;
		return snapshot;
	}

	RendererRecoverySnapshot AttachFatalDialogReport(
		const RendererRecoverySnapshot& previous,
		std::string_view reportId,
		std::string_view crashDumpPath,
		std::string_view crashSummarySidecarPath,
		std::string_view crashDumpStatus)
	{
		RendererRecoverySnapshot snapshot = previous;
		snapshot.fatalDialogReportId = std::string(reportId);
		snapshot.crashDumpPath = std::string(crashDumpPath);
		snapshot.crashSummarySidecarPath = std::string(crashSummarySidecarPath);
		snapshot.crashDumpStatus = std::string(crashDumpStatus);
		snapshot.selectedAction = RendererRecoveryAction::FatalDialog;
		snapshot.fatalDialogShown = false;
		return snapshot;
	}

	ResourceRestorationPlan BuildResourceRestorationPlan(ResourceRestorationPlanInput input)
	{
		ResourceRestorationPlan plan;
		// The restoration plan is a CPU-side checklist for a future protected
		// renderer rebuild. It intentionally contains only stable resource names
		// and policy labels, never Vulkan handles from the lost device.
		plan.renderSessionGeneration = input.renderSessionGeneration;
		plan.requiredShaders = std::move(input.requiredShaders);
		plan.defaultResources = std::move(input.defaultResources);
		plan.visibleMeshes = std::move(input.visibleMeshes);
		plan.visibleMaterials = std::move(input.visibleMaterials);
		plan.visibleTextures = std::move(input.visibleTextures);
		plan.virtualGeometryResidency = std::move(input.virtualGeometryResidency);
		plan.textureResidency = std::move(input.textureResidency);
		plan.viewTargets = std::move(input.viewTargets);
		plan.profilerAndDiagnosticResources = std::move(input.profilerAndDiagnosticResources);
		plan.deferredUploads = std::move(input.deferredUploads);
		plan.missingOrDeferredResources = std::move(input.missingOrDeferredResources);
		return plan;
	}

	RecoveryDiagnosticSnapshot BuildRecoveryDiagnosticSnapshot(
		const RendererRecoverySnapshot& snapshot,
		std::vector<std::string> recentRenderPhases,
		std::vector<std::string> resourceRestorationSummary)
	{
		RecoveryDiagnosticSnapshot diagnostics;
		diagnostics.recoveryState = snapshot;
		diagnostics.incident.incidentId = snapshot.incidentId;
		diagnostics.incident.firstFailureStage = snapshot.firstFailureStage;
		diagnostics.incident.firstResult = snapshot.firstFailureResult;
		diagnostics.incident.affectedView = snapshot.affectedView;
		diagnostics.incident.affectedResourceId = snapshot.affectedResourceId;
		diagnostics.incident.affectedResourceName = snapshot.affectedResourceName;
		diagnostics.incident.hardwareName = snapshot.hardwareName;
		diagnostics.incident.driverVersion = snapshot.driverVersion;
		diagnostics.incident.apiVersion = snapshot.apiVersion;
		diagnostics.incident.frameId = snapshot.frameId;
		diagnostics.incident.renderSessionGeneration = snapshot.renderSessionGeneration;
		diagnostics.incident.duplicateFailureCount = snapshot.duplicateFailureCount;
		diagnostics.incident.finalOutcome = snapshot.finalOutcome;
		diagnostics.activeSession.sessionId = snapshot.sessionId;
		diagnostics.activeSession.incidentId = snapshot.incidentId;
		diagnostics.recentRenderPhases = std::move(recentRenderPhases);
		diagnostics.resourceRestorationSummary = std::move(resourceRestorationSummary);
		diagnostics.suppressedFailures = snapshot.duplicateFailureCount;
		if (snapshot.canSaveProjectData)
		{
			diagnostics.safeActions.push_back("Save safe project data");
		}
		if (snapshot.canAttemptRebuild || snapshot.canRetryRendererRebuild)
		{
			diagnostics.safeActions.push_back("Request renderer rebuild");
		}
		return diagnostics;
	}
}
