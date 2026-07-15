// ScriptBuildAutomationProvider.cpp - Registers existing export MCP tools.

#include "ScriptBuildAutomationProvider.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		AutomationCapabilityRegistration MakeExportRegistration(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string description,
			MutationMode mutationMode,
			AutomationPermission permission,
			AutomationApprovalPolicy approvalPolicy,
			nlohmann::json inputSchema)
		{
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.version = 1;
			descriptor.category = "export";
			descriptor.description = std::move(description);
			descriptor.inputSchema = std::move(inputSchema);
			// Keep the historic open result envelope, but document the additive
			// readiness/cache fields so callers can distinguish a forced rebuild
			// from a reusable managed output without parsing status prose.
			descriptor.resultSchema = {
				{"type", "object"},
				{"properties", {
					{"ok", {{"type", "boolean"}}},
					{"message", {{"type", "string"}}},
					{"data", {{"type", "object"}}}
				}},
				{"additionalProperties", true}
			};
			descriptor.permission = permission;
			descriptor.approvalPolicy = approvalPolicy;
			descriptor.mutationMode = mutationMode;
			descriptor.availability.requiresProject = true;
			descriptor.availability.requiresScene = false;
			descriptor.availability.requiresRenderer = false;

			// Argument preparation can use an immutable project snapshot. Starting or
			// canceling an export must cross the editor's protected boundary because
			// those operations mutate the editor-owned export job state.
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = mutationMode != MutationMode::ReadOnly;
			descriptor.execution.visibilityBoundary = mutationMode == MutationMode::ReadOnly
				? "immutable_snapshot"
				: "editor_protected_commit";
			descriptor.execution.preparationReads = {"EditorAutomationSession", "ProjectState", "ExportProfiles"};
			if (mutationMode != MutationMode::ReadOnly)
			{
				descriptor.execution.commitReads = {"ProjectState", "ExportProfiles"};
				descriptor.execution.commitWrites = {"ExportJobs", "EditorAutomationSession"};
			}

			const std::string toolName = descriptor.name;
			return {
				std::move(descriptor),
				resolver(toolName),
				{}
			};
		}

		nlohmann::json ProfileInputSchema(bool acceptsOutputConfirmation)
		{
			nlohmann::json properties = {
				{"profile", {{"type", "string"}, {"minLength", 1}}}
			};
			if (acceptsOutputConfirmation)
			{
				properties["confirmOutput"] = {{"type", "string"}};
			}
			return {
				{"type", "object"},
				{"properties", std::move(properties)},
				{"required", nlohmann::json::array({"profile"})},
				// Legacy handlers may accept additive compatibility fields, so discovery
				// must not narrow their established version-one request contract.
				{"additionalProperties", true}
			};
		}
	}

	ScriptBuildAutomationProvider::ScriptBuildAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view ScriptBuildAutomationProvider::GetProviderId() const
	{
		return "script_build";
	}

	std::vector<AutomationCapabilityRegistration> ScriptBuildAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> capabilities;
		capabilities.reserve(9);
		capabilities.push_back(MakeExportRegistration(
			resolveHandler,
			"export.validate",
			"Validate an export profile without changing project or export state.",
			MutationMode::ReadOnly,
			AutomationPermission::Read,
			AutomationApprovalPolicy::None,
			ProfileInputSchema(false)));
		capabilities.push_back(MakeExportRegistration(
			resolveHandler,
			"export.start",
			"Start an approved export using an existing project profile.",
			MutationMode::ExportMutation,
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::HighImpact,
			ProfileInputSchema(true)));
		capabilities.push_back(MakeExportRegistration(
			resolveHandler,
			"export.cancel",
			"Request cancellation of an export job owned by this editor session.",
			MutationMode::ExportMutation,
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			{
				{"type", "object"},
				{"properties", {{"jobId", {{"type", "string"}, {"minLength", 1}}}}},
				{"required", nlohmann::json::array({"jobId"})},
				{"additionalProperties", true}
			}));
		for (const auto& [name, mode] : std::vector<std::pair<std::string, MutationMode>>{
			{"script.status", MutationMode::ReadOnly}, {"script.templates.list", MutationMode::ReadOnly}, {"script.create", MutationMode::AssetMutation},
			{"script.types.list", MutationMode::ReadOnly}, {"script.workspace.sync", MutationMode::ProjectMutation},
			{"script.compile", MutationMode::ProjectMutation}})
		{
			auto registration = MakeExportRegistration(
				resolveHandler, name, "Use approved editor script templates and managed compilation services.", mode,
				mode == MutationMode::ReadOnly ? AutomationPermission::Read : AutomationPermission::ProjectWrite,
				AutomationApprovalPolicy::None, {{"type", "object"}, {"additionalProperties", true}});
			registration.descriptor.category = "script";
			if (name == "script.status")
			{
				registration.descriptor.description = "Read managed coordinator, readiness-manifest, fingerprint, and loaded-output identity evidence.";
				registration.descriptor.execution.preparationReads.push_back("ManagedReadinessManifest");
			}
			if (name == "script.compile")
			{
				registration.descriptor.description = "Force a managed build and report additive readiness/cache evidence while preserving the durable-job result contract.";
				registration.descriptor.execution.preparationReads.push_back("ManagedReadinessManifest");
				registration.descriptor.execution.commitReads.push_back("ManagedReadinessManifest");
				registration.descriptor.execution.commitWrites.push_back("ManagedReadinessManifest");
			}
			capabilities.push_back(std::move(registration));
		}
		return capabilities;
	}
}
