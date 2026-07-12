// AuthoringAutomationProvider.cpp - Typed schemas for advanced editor surfaces.

#include "AuthoringAutomationProvider.h"
#include "../../Project/EditorPreferences.h"
#include "../../Project/ProjectSettings.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		AutomationCapabilityRegistration Make(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			std::vector<std::string> inputs,
			std::vector<std::string> outputs,
			AutomationPermission permission,
			AutomationApprovalPolicy approval,
			bool requiresProject,
			bool requiresScene = false,
			bool sceneView = false);

		nlohmann::json SettingKeySchema(bool projectScope)
		{
			nlohmann::json values = nlohmann::json::array();
			if (projectScope)
				for (const std::string_view key : TerraEngine::Editor::Settings::kProjectAutomationSettingKeys) values.push_back(key);
			else
				for (const std::string_view key : TerraEngine::Editor::Settings::kEditorAutomationSettingKeys) values.push_back(key);
			return {{"type", "string"}, {"enum", std::move(values)}};
		}

		AutomationCapabilityRegistration MakeSetting(
			const AutomationHandlerResolver& resolver,
			std::string name,
			bool projectScope,
			bool write)
		{
			auto registration = Make(
				resolver,
				std::move(name),
				"settings",
				write ? std::vector<std::string>{"key", "value"} : std::vector<std::string>{"key"},
				write ? std::vector<std::string>{"value", "allowedKeys", "persisted"} : std::vector<std::string>{"value", "allowedKeys"},
				write ? (projectScope ? AutomationPermission::ProjectWrite : AutomationPermission::GlobalWrite) : AutomationPermission::Read,
				write && !projectScope ? AutomationApprovalPolicy::HighImpact : AutomationApprovalPolicy::None,
				projectScope);
		registration.descriptor.inputSchema["properties"]["key"] = SettingKeySchema(projectScope);
		if (write)
		{
			// Each allowlisted editor setting is currently boolean; the project
			// display name is the only project-scoped string setting.
			registration.descriptor.inputSchema["properties"]["value"] = projectScope
				? nlohmann::json{{"type", "string"}, {"minLength", 1}}
				: nlohmann::json{{"type", "boolean"}};
		}
		return registration;
	}

		AutomationCapabilityRegistration Make(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			std::vector<std::string> inputs,
			std::vector<std::string> outputs,
			AutomationPermission permission,
			AutomationApprovalPolicy approval,
			bool requiresProject,
			bool requiresScene,
			bool sceneView)
		{
			nlohmann::json inputProperties = nlohmann::json::object();
			for (const std::string& input : inputs) inputProperties[input] = {{"description", "Semantic editor-owned value."}};
			nlohmann::json outputProperties = nlohmann::json::object();
			for (const std::string& output : outputs) outputProperties[output] = {{"description", "Structured editor-owned result."}};

			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.category = std::move(category);
			descriptor.description = "Advanced semantic authoring operation.";
			descriptor.inputSchema = {{"type", "object"}, {"properties", std::move(inputProperties)}, {"additionalProperties", false}};
			descriptor.resultSchema = {{"type", "object"}, {"properties", std::move(outputProperties)}};
			descriptor.permission = permission;
			descriptor.approvalPolicy = approval;
			descriptor.mutationMode = permission == AutomationPermission::Read ? MutationMode::ReadOnly : MutationMode::ProjectMutation;
			descriptor.availability.requiresProject = requiresProject;
			descriptor.availability.requiresScene = requiresScene;
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = permission != AutomationPermission::Read;
			descriptor.execution.visibilityBoundary = permission == AutomationPermission::Read ? "immutable_snapshot" : "editor_authoring_commit";
			descriptor.execution.preparationReads = {"EditorAutomationSession"};
			descriptor.execution.commitWrites = permission == AutomationPermission::Read ? std::vector<std::string>{} : std::vector<std::string>{"ProjectState", "EditorAutomationSession"};
			const std::string toolName = descriptor.name;
			AutomationCapabilityRegistration registration{std::move(descriptor), resolver(toolName), {}};
			if (sceneView)
			{
				registration.availabilityPredicate = [availability = registration.descriptor.availability](const AutomationStateSnapshot& state) mutable {
					auto result = availability;
					result.available = state.projectAvailable && state.sceneAvailable && state.rendererAvailable && state.availableServices.contains("SceneView");
					if (!result.available) result.reason = "The Scene View authoring surface is unavailable.";
					return result;
				};
			}
			return registration;
		}
	}

	AuthoringAutomationProvider::AuthoringAutomationProvider(AutomationHandlerResolver resolver) : resolveHandler(std::move(resolver)) {}
	std::string_view AuthoringAutomationProvider::GetProviderId() const { return "advanced_authoring"; }

	std::vector<AutomationCapabilityRegistration> AuthoringAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> result;
		result.push_back(Make(resolveHandler, "build_profile.list", "build_profile", {}, {"profiles"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.get", "build_profile", {"profileId"}, {"profile"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.create", "build_profile", {"name"}, {"profileId", "validation"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.update", "build_profile", {"profileId", "changes"}, {"profileId", "validation"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.duplicate", "build_profile", {"profileId", "name"}, {"profileId", "validation"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.prepare_delete", "build_profile", {"profileId"}, {"impact", "approvalId"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::Destructive, true));

		result.push_back(MakeSetting(resolveHandler, "settings.project.get", true, false));
		result.push_back(MakeSetting(resolveHandler, "settings.project.set", true, true));
		result.push_back(MakeSetting(resolveHandler, "settings.editor.get", false, false));
		result.push_back(MakeSetting(resolveHandler, "settings.editor.set", false, true));

		result.push_back(Make(resolveHandler, "scene_view.state", "scene_view", {}, {"state"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.camera.set", "scene_view", {"position", "focus"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.frame_object", "scene_view", {"target"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.tool.set", "scene_view", {"tool"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.orientation.set", "scene_view", {"orientation"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.snapping.set", "scene_view", {"enabled"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.visualization.set", "scene_view", {"mode"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));

		result.push_back(Make(resolveHandler, "profiler.capture.start", "profiler", {}, {"jobId", "state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.stop", "profiler", {}, {"jobId", "state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.list", "profiler", {}, {"artifacts"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.load", "profiler", {"artifactId"}, {"jobId", "state"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.summary", "profiler", {"artifactId"}, {"summary"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "editor.commands.list", "editor_commands", {}, {"commands"}, AutomationPermission::Read, AutomationApprovalPolicy::None, false));
		result.push_back(Make(resolveHandler, "editor.commands.invoke", "editor_commands", {"commandId"}, {"commandId", "completed"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false));
		return result;
	}
}
