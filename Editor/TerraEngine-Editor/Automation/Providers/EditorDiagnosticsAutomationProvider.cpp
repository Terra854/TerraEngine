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
		capabilities.reserve(12);
		capabilities.push_back(MakeRegistration(resolveHandler, "activity.list", "activity", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "game_view.capture", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "camera.capture_object", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, true, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "camera.capture_custom", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.virtualGeometry.stats", "renderer", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
		capabilities.push_back(MakeRegistration(resolveHandler, "renderer.recovery.safe_save_project", "renderer", MutationMode::ProjectMutation, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "diagnostics.get_summary", "diagnostics", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "jobs.list", "jobs", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "jobs.get", "jobs", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "jobs.cancel", "jobs", MutationMode::ProjectMutation, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false, false, false));
		capabilities.push_back(MakeRegistration(resolveHandler, "logs.query", "logs", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, false));
		return capabilities;
	}
}
