// EditorDiagnosticsAutomationProvider.cpp - Registers existing diagnostic tools.

#include "EditorDiagnosticsAutomationProvider.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		// Diagnostics handlers may be backed by an older renderer snapshot. Keep
		// the immutable evidence contract stable by retaining measured values and
		// explicitly marking identity lanes unavailable when no snapshot supplied them.
		AutomationHandler WrapIdentityEvidence(AutomationHandler handler)
		{
			return [handler = std::move(handler)](const AutomationRequestContext& request) {
				AutomationDispatchResult result = handler ? handler(request) : AutomationDispatchResult{};
				if(!result.data.is_object()) result.data = nlohmann::json::object();
				auto unavailable = result.data.value("unavailableMetrics", nlohmann::json::array());
				if(!unavailable.is_array()) unavailable = nlohmann::json::array();
				for(const std::string_view field : { "objectIdentity", "primitiveIdentity", "materialIdentity",
					"pickingIdentity", "depthIdentity", "routeGeneration" })
				{
					if(!result.data.contains(field))
					{
						result.data[field] = nullptr;
						unavailable.push_back(field);
					}
				}
				result.data["unavailableMetrics"] = std::move(unavailable);
				return result;
			};
		}

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
					{"profilerCaptureEvidence", {{"description", "Additive v2 stage, boundary, responsiveness, and work-volume summaries."}}},
					{"objectIdentity", {{"type", {"string", "null"}}}}, {"primitiveIdentity", {{"type", {"string", "null"}}}},
					{"materialIdentity", {{"type", {"string", "null"}}}}, {"pickingIdentity", {{"type", {"string", "null"}}}},
					{"depthIdentity", {{"type", {"string", "null"}}}}, {"routeGeneration", {{"type", {"integer", "null"}}}},
					{"unavailableMetrics", {{"description", "Metrics that were not measured; unavailable values are never substituted with zero."}}}
				}}};
				descriptor.execution.preparationReads.insert(descriptor.execution.preparationReads.end(), {"FrameExecutionPlan", "SystemScheduler", "ThreadPoolTelemetry"});
			}
			if (descriptor.name == "renderer.virtualGeometry.stats")
			{
				// Shadow uses renderer-published selection diagnostics instead of the
				// fence-delayed Game/Scene timing store. Advertise every accepted
				// selector so discovery clients can request the supported evidence.
				descriptor.description = "Read virtual-geometry statistics and diagnostics for Game, Scene, or Shadow views.";
				descriptor.inputSchema = {{"type", "object"}, {"properties", {
					{"viewType", {{"type", "string"}, {"enum", {"Game", "Scene", "Shadow"}},
						{"description", "Render view whose current virtual-geometry diagnostics are requested."}}}
				}}, {"additionalProperties", true}};
			}
			if (descriptor.name == "route.lock")
			{
				descriptor.description = "Read or set a transient representation-route lock for validation captures.";
				descriptor.inputSchema = {{"type", "object"}, {"properties", {{"route", {{"type", "string", "enum", {"automatic", "direct", "virtual-hardware", "virtual-compute"}}}}, {"enabled", {{"type", "boolean"}}}}}};
				descriptor.resultSchema = {{"type", "object"}, {"properties", {{"route", {{"type", "string"}}}, {"locked", {{"type", "boolean"}}}}}};
				descriptor.execution.visibilityBoundary = "immutable_snapshot";
			}
			if (descriptor.name == "diagnostics.snapshot")
			{
				descriptor.description = "Return an immutable diagnostics snapshot with explicit unavailable metrics.";
				descriptor.resultSchema = {{"type", "object"}, {"properties", {{"snapshotId", {{"type", "string"}}},
					{"objectIdentity", {{"type", {"string", "null"}}}}, {"primitiveIdentity", {{"type", {"string", "null"}}}},
					{"materialIdentity", {{"type", {"string", "null"}}}}, {"pickingIdentity", {{"type", {"string", "null"}}}},
					{"depthIdentity", {{"type", {"string", "null"}}}}, {"routeGeneration", {{"type", {"integer", "null"}}}},
					{"unavailableMetrics", {{"type", "array"}}}}}};
				descriptor.execution.visibilityBoundary = "immutable_snapshot";
			}
			if (descriptor.name == "capture.provenance")
			{
				descriptor.description = "Return capture command, revision, hardware, graphics-path, and artifact provenance.";
				descriptor.resultSchema = {{"type", "object"}, {"properties", {{"commandHash", {{"type", "string"}}}, {"runtimeRevision", {{"type", "string"}}}, {"graphicsPath", {{"type", "string"}}}, {"unavailableMetrics", {{"type", "array"}}}}}};
				descriptor.execution.visibilityBoundary = "immutable_snapshot";
			}
			if (requiresRenderer)
			{
				descriptor.execution.preparationReads.push_back("RendererDiagnostics");
			}
			descriptor.execution.commitWrites = mutationMode == MutationMode::ReadOnly
				? std::vector<std::string>{}
				: std::vector<std::string>{"ProjectState", "EditorAutomationSession"};

			const std::string toolName = descriptor.name;
			AutomationHandler handler = resolver(toolName);
			if (toolName == "route.lock")
			{
				// Keep the public diagnostics operation stable while delegating to the
				// installed RenderSystem route mutation handlers. Representation names
				// are translated to the renderer's real Vulkan validation routes.
				handler = [resolver](const AutomationRequestContext& request) {
					const bool enabled = request.arguments.value("enabled", true);
					AutomationHandler target = resolver(enabled
						? "renderer.virtualGeometry.validationRoute.set"
						: "renderer.virtualGeometry.validationRoute.reset");
					if (!target) return AutomationDispatchResult{ false, ErrorCategory::MissingTarget, "Renderer route handler is unavailable.", {} };
					AutomationRequestContext translated = request;
					if (enabled)
					{
						const std::string route = request.arguments.value("route", "automatic");
						if (route == "automatic")
						{
							// Automatic means no forced route; use the real reset handler
							// rather than manufacturing an indexed override.
							target = resolver("renderer.virtualGeometry.validationRoute.reset");
							if (!target) return AutomationDispatchResult{ false, ErrorCategory::MissingTarget, "Renderer route reset handler is unavailable.", {} };
						}
						else if (route == "direct") translated.arguments = {{"route", "Indexed"}, {"routeLock", "direct"}};
						else if (route == "virtual-hardware") translated.arguments = {{"route", "Hardware"}, {"routeLock", route}};
						else if (route == "virtual-compute") translated.arguments = {{"route", "Compute"}, {"routeLock", route}};
						else return AutomationDispatchResult{ false, ErrorCategory::Validation, "route must be automatic, direct, virtual-hardware, or virtual-compute.", {} };
					}
					return target(translated);
				};
			}
			if (toolName == "diagnostics.snapshot" || toolName == "diagnostics.get_summary")
				handler = WrapIdentityEvidence(std::move(handler));
			return {
				std::move(descriptor),
				std::move(handler),
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
		capabilities.reserve(18);
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
			capabilities.push_back(MakeRegistration(resolveHandler, "route.lock", "renderer", MutationMode::EditorSelection, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false, false, true));
			capabilities.push_back(MakeRegistration(resolveHandler, "diagnostics.snapshot", "diagnostics", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
			capabilities.push_back(MakeRegistration(resolveHandler, "capture.provenance", "capture", MutationMode::ReadOnly, AutomationPermission::Read, AutomationApprovalPolicy::None, false, false, true));
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
