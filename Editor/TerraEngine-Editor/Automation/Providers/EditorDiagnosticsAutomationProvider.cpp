// EditorDiagnosticsAutomationProvider.cpp - Registers existing diagnostic tools.

#include "EditorDiagnosticsAutomationProvider.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		AutomationCapabilityRegistration MakeRegistration(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			MutationMode mutationMode,
			AutomationPermission permission,
			AutomationApprovalPolicy approval,
			bool requiresProject,
			bool requiresScene,
			bool requiresRenderer)
		{
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.category = std::move(category);
			descriptor.description = "Compatibility MCP operation owned by the editor diagnostics provider.";
			descriptor.inputSchema = {{"type", "object"}, {"additionalProperties", true}};
			descriptor.resultSchema = {{"type", "object"}};
			descriptor.permission = permission;
			descriptor.approvalPolicy = approval;
			descriptor.mutationMode = mutationMode;
			descriptor.availability.requiresProject = requiresProject;
			descriptor.availability.requiresScene = requiresScene;
			descriptor.availability.requiresRenderer = requiresRenderer;
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = mutationMode != MutationMode::ReadOnly;
			descriptor.execution.visibilityBoundary = mutationMode == MutationMode::ReadOnly
				? "immutable_snapshot"
				: "editor_protected_commit";
			descriptor.execution.preparationReads = {"EditorAutomationSession", "EditorDiagnostics"};
			if (descriptor.name == "diagnostics.get_summary")
			{
				// The summary reads published scheduler and worker snapshots only. Keeping
				// these declarations explicit lets automation diagnostics explain why this
				// operation remains safe during a running frame.
				descriptor.description = "Read immutable scheduler decisions, conditional flush state, and bounded worker telemetry; includeDetails adds retained histories.";
				descriptor.inputSchema = {{"type", "object"}, {"properties", {{"includeDetails", {{"type", "boolean"}, {"description", "Include bounded workload records, dependency edges, worker histories, and profiler v2 evidence."}}}}}, {"additionalProperties", true}};
				descriptor.resultSchema = {{"type", "object"}, {"properties", {
					{"profilerTimingStreams", {{"description", "Legacy status-aware timing stream summaries."}}},
					{"profilerCaptureEvidence", {{"description", "Additive v2 stage, boundary, responsiveness, and work-volume summaries."}}}
				}}};
				descriptor.execution.preparationReads.insert(descriptor.execution.preparationReads.end(), {"FrameExecutionPlan", "SystemScheduler", "ThreadPoolTelemetry"});
			}
			if (requiresRenderer)
			{
				descriptor.execution.preparationReads.push_back("RendererDiagnostics");
			}
			descriptor.execution.commitWrites = mutationMode == MutationMode::ReadOnly
				? std::vector<std::string>{}
				: std::vector<std::string>{"ProjectState", "EditorAutomationSession"};

			const std::string toolName = descriptor.name;
			return {
				std::move(descriptor),
				resolver(toolName),
				{}
			};
		}
	}

	EditorDiagnosticsAutomationProvider::EditorDiagnosticsAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view EditorDiagnosticsAutomationProvider::GetProviderId() const
	{
		return "editor_diagnostics";
	}

	std::vector<AutomationCapabilityRegistration> EditorDiagnosticsAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> capabilities;
		capabilities.reserve(15);
		capabilities.push_back(MakeRegistration(resolveHandler, "activity.list", "activity", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "game_view.capture", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "camera.capture_object", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, true, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "camera.capture_custom", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.virtualGeometry.stats", "renderer", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.virtualGeometry.capabilityEvidence", "renderer", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		// Validation-route overrides are transient session selections, like selecting
		// an authored object. Project-write permission is sufficient and avoids
		// invalidating this provider with the stricter global-write approval rule.
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.virtualGeometry.validationRoute.set", "renderer", MutationMode::EditorSelection, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.virtualGeometry.validationRoute.reset", "renderer", MutationMode::EditorSelection, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.virtualGeometry.authoritativePick", "renderer", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.recovery.safe_save_project", "renderer", MutationMode::ProjectMutation, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "diagnostics.get_summary", "diagnostics", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "jobs.list", "jobs", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "jobs.get", "jobs", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "jobs.cancel", "jobs", MutationMode::ProjectMutation, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "logs.query", "logs", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		return capabilities;
	}
}
