// RendererRecovery.cpp - CPU-only renderer failure state helpers.

#include "Renderer/RendererRecovery.h"

#include <algorithm>
#include <utility>

namespace TerraEngine::Renderer
{
	namespace
	{
		std::string MakeIncidentId(std::uint64_t generation, std::uint64_t frameId)
		{
			return "renderer-incident-" + std::to_string(generation) + "-" + std::to_string(frameId);
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
		case RendererRecoveryOutcome::FatalExportFailure: return "FatalExportFailure";
		}
		return "None";
	}

	bool IsRendererRecoveryActive(RendererRecoveryStateKind state)
	{
		return state != RendererRecoveryStateKind::Active;
	}

	bool CanSubmitNormalGpuWork(const RendererRecoverySnapshot& snapshot)
	{
		return snapshot.state == RendererRecoveryStateKind::Active;
	}

	std::string ValidateRecoverySnapshot(const RendererRecoverySnapshot& snapshot)
	{
		if (snapshot.state == RendererRecoveryStateKind::Active && !snapshot.incidentId.empty())
		{
			return "Active renderer snapshots must not retain a device-loss incident id.";
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
		// Later Vulkan failures are symptoms of the first lost device. Preserve the
		// root cause and aggregate duplicates so logs do not flood every frame.
		if (!snapshot.incidentId.empty() && snapshot.state != RendererRecoveryStateKind::Active)
		{
			++snapshot.duplicateFailureCount;
			snapshot.message = "Renderer device loss already latched; suppressing duplicate GPU failure.";
			return snapshot;
		}

		snapshot.state = RendererRecoveryStateKind::DeviceLost;
		snapshot.incidentId = MakeIncidentId(snapshot.renderSessionGeneration, frameId);
		snapshot.sessionId.clear();
		snapshot.message = "Graphics device lost; rendering is unavailable and the current process cannot rebuild the Vulkan device.";
		snapshot.firstFailureStage = ClassifyFailureStage(operation);
		snapshot.firstFailureResult = std::string(result);
		snapshot.affectedView = affectedView == RendererRecoveryAffectedView::None
			? ClassifyAffectedView(operation)
			: affectedView;
		snapshot.affectedResourceId = std::move(affectedResourceId);
		snapshot.affectedResourceName = std::move(affectedResourceName);
		snapshot.duplicateFailureCount = 0u;
		snapshot.canSaveProjectData = true;
		snapshot.selectedAction = RendererRecoveryAction::RecordIncident;
		snapshot.finalOutcome = RendererRecoveryOutcome::DeviceLostLatched;
		snapshot.lastError.clear();
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

	RecoveryDiagnosticSnapshot BuildRecoveryDiagnosticSnapshot(
		const RendererRecoverySnapshot& snapshot,
		std::vector<std::string> recentRenderPhases)
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
		diagnostics.recentRenderPhases = std::move(recentRenderPhases);
		diagnostics.suppressedFailures = snapshot.duplicateFailureCount;
		if (snapshot.canSaveProjectData)
		{
			diagnostics.safeActions.push_back("Save safe project data");
		}
		return diagnostics;
	}
}
