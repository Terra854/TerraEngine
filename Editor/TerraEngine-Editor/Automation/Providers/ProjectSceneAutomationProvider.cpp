// ProjectSceneAutomationProvider.cpp - Typed descriptors for scene graph tools.

#include "ProjectSceneAutomationProvider.h"

#include <tuple>
#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;

		// Object references intentionally accept the three representations already
		// supported by the legacy resolver: stable UUID/name strings and entity IDs.
		Json ObjectTargetSchema()
		{
			return {
				{"oneOf", Json::array({
					Json{{"type", "string"}, {"minLength", 1}},
					Json{{"type", "integer"}, {"minimum", 0}}
				})}
			};
		}

		Json ObjectSchema(Json properties = Json::object(), Json required = Json::array())
		{
			return {
				{"type", "object"},
				{"properties", std::move(properties)},
				{"required", std::move(required)},
				{"additionalProperties", false}
			};
		}

		Json ResultSchema()
		{
			// Legacy handlers return capability-specific payloads in data. Keeping
			// that field open preserves the version-one result envelope.
			return {
				{"type", "object"},
				{"properties", {
					{"ok", {{"type", "boolean"}}},
					{"message", {{"type", "string"}}},
					{"data", {{"type", "object"}}}
				}},
				{"required", Json::array({"ok", "message", "data"})},
				{"additionalProperties", true}
			};
		}

		ExecutionBoundaryDeclaration ReadExecution(std::vector<std::string> reads)
		{
			ExecutionBoundaryDeclaration execution;
			execution.workerPreparation = false;
			execution.protectedCommit = false;
			execution.preparationReads = std::move(reads);
			return execution;
		}

		ExecutionBoundaryDeclaration WriteExecution(
			std::string visibilityBoundary,
			std::vector<std::string> reads,
			std::vector<std::string> writes)
		{
			ExecutionBoundaryDeclaration execution;
			// Request validation may run on a worker, but all legacy mutation
			// handlers are invoked only at the protected editor boundary.
			execution.workerPreparation = true;
			execution.protectedCommit = true;
			execution.visibilityBoundary = std::move(visibilityBoundary);
			execution.preparationReads = reads;
			execution.commitReads = std::move(reads);
			execution.commitWrites = std::move(writes);
			return execution;
		}

		AutomationAvailability Availability(bool requiresProject, bool requiresScene)
		{
			AutomationAvailability availability;
			availability.requiresProject = requiresProject;
			availability.requiresScene = requiresScene;
			return availability;
		}

		AutomationCapabilityRegistration Registration(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			std::string description,
			Json inputSchema,
			AutomationPermission permission,
			AutomationApprovalPolicy approvalPolicy,
			MutationMode mutationMode,
			AutomationAvailability availability,
			ExecutionBoundaryDeclaration execution)
		{
			AutomationCapabilityRegistration registration;
			registration.descriptor.name = std::move(name);
			registration.descriptor.version = 1;
			registration.descriptor.category = std::move(category);
			registration.descriptor.description = std::move(description);
			registration.descriptor.inputSchema = std::move(inputSchema);
			registration.descriptor.resultSchema = ResultSchema();
			registration.descriptor.permission = permission;
			registration.descriptor.approvalPolicy = approvalPolicy;
			registration.descriptor.mutationMode = mutationMode;
			registration.descriptor.availability = availability;
			registration.descriptor.execution = std::move(execution);

			const std::string capabilityName = registration.descriptor.name;
			registration.handler = resolver(capabilityName);
			// The predicate uses only the immutable request snapshot. The registry
			// applies these requirements consistently during discovery and dispatch.
			registration.availabilityPredicate = [availability](const AutomationStateSnapshot&) {
				return availability;
			};
			return registration;
		}
	}

	ProjectSceneAutomationProvider::ProjectSceneAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view ProjectSceneAutomationProvider::GetProviderId() const
	{
		return "project_scene";
	}

	std::vector<AutomationCapabilityRegistration> ProjectSceneAutomationProvider::GetCapabilities()
	{
		const Json target = ObjectTargetSchema();
		std::vector<AutomationCapabilityRegistration> capabilities;
		capabilities.reserve(20);

		capabilities.push_back(Registration(
			resolveHandler,
			"project.templates.list",
			"project",
			"List the editor-owned project templates available to automation.",
			ObjectSchema(),
			AutomationPermission::Read,
			AutomationApprovalPolicy::None,
			MutationMode::ReadOnly,
			Availability(false, false),
			ReadExecution({"ProjectTemplates"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"project.state",
			"project",
			"Read project/session identity, source-authority, and derived-scene cache decisions.",
			ObjectSchema(),
			AutomationPermission::Read,
			AutomationApprovalPolicy::None,
			MutationMode::ReadOnly,
			Availability(false, false),
			ReadExecution({"ProjectSession", "ActiveScene", "SceneDirtyState", "DerivedSceneCache"})));

		capabilities.push_back(Registration(resolveHandler, "scene.list", "scene", "List project scene documents and active/dirty state.", ObjectSchema(), AutomationPermission::Read, AutomationApprovalPolicy::None, MutationMode::ReadOnly, Availability(true, false), ReadExecution({"ProjectSession", "SceneInventory"})));
		capabilities.push_back(Registration(resolveHandler, "scene.create", "scene", "Create and optionally open a project-relative scene.", ObjectSchema({{"path", {{"type", "string"}}}, {"name", {{"type", "string"}}}, {"open", {{"type", "boolean"}}}}, Json::array({"path"})), AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, MutationMode::SceneMutation, Availability(true, false), WriteExecution("scene_lifecycle_commit", {"ProjectSession", "SceneInventory"}, {"SceneFile", "ActiveScene"})));
		capabilities.push_back(Registration(resolveHandler, "scene.save_as", "scene", "Save the active scene to a new identity.", ObjectSchema({{"path", {{"type", "string"}}}}, Json::array({"path"})), AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, MutationMode::SceneMutation, Availability(true, true), WriteExecution("scene_lifecycle_commit", {"ActiveScene", "ObjectStorage"}, {"SceneFile", "SceneDirtyState"})));
		capabilities.push_back(Registration(resolveHandler, "scene.rename", "scene", "Rename a scene after reference-impact preflight.", ObjectSchema({{"source", {{"type", "string"}}}, {"destination", {{"type", "string"}}}}, Json::array({"source", "destination"})), AutomationPermission::ProjectWrite, AutomationApprovalPolicy::HighImpact, MutationMode::SceneMutation, Availability(true, false), WriteExecution("scene_lifecycle_commit", {"SceneInventory", "BuildProfiles"}, {"SceneFile"})));
		capabilities.push_back(Registration(resolveHandler, "scene.duplicate", "scene", "Duplicate a scene to a distinct file identity.", ObjectSchema({{"source", {{"type", "string"}}}, {"destination", {{"type", "string"}}}}, Json::array({"source", "destination"})), AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, MutationMode::SceneMutation, Availability(true, false), WriteExecution("scene_lifecycle_commit", {"SceneInventory"}, {"SceneFile"})));
		capabilities.push_back(Registration(resolveHandler, "scene.prepare_delete", "scene", "Report scene references and create a destructive approval preflight.", ObjectSchema({{"path", {{"type", "string"}}}}, Json::array({"path"})), AutomationPermission::ProjectWrite, AutomationApprovalPolicy::Destructive, MutationMode::Destructive, Availability(true, false), WriteExecution("approval_preflight_only", {"SceneInventory", "BuildProfiles"}, {"EditorAutomationSession"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"scene.open",
			"scene",
			"Open a project-relative scene after the existing approval check.",
			ObjectSchema({{"path", {{"type", "string"}, {"minLength", 1}}}}, Json::array({"path"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::HighImpact,
			MutationMode::SceneMutation,
			Availability(true, false),
			WriteExecution("scene_open_commit", {"ProjectSession", "SceneInventory"}, {"ActiveScene", "ObjectStorage"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"scene.save",
			"scene",
			"Save the active scene, optionally to a project-relative path.",
			ObjectSchema({{"path", {{"type", "string"}, {"minLength", 1}}}}),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			MutationMode::SceneMutation,
			Availability(true, true),
			WriteExecution("scene_persistence_commit", {"ProjectSession", "ActiveScene", "ObjectStorage"}, {"SceneFile", "SceneDirtyState"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"scene.list_objects",
			"scene",
			"List a bounded stable page of scene objects with optional component names and text filtering.",
			ObjectSchema({
				{"includeComponents", {{"type", "boolean"}}},
				{"search", {{"type", "string"}}},
				{"offset", {{"type", "integer"}, {"minimum", 0}}},
				{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 128}}}
			}),
			AutomationPermission::Read,
			AutomationApprovalPolicy::None,
			MutationMode::ReadOnly,
			Availability(false, false),
			ReadExecution({"ObjectStorage", "ComponentAccessRegistry"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"object.create",
			"object",
			"Create a scene object with an optional parent and selection update.",
			ObjectSchema({
				{"name", {{"type", "string"}, {"minLength", 1}}},
				{"parent", Json{{"oneOf", Json::array({target, Json{{"type", "null"}}})}}},
				{"select", {{"type", "boolean"}}}
			}, Json::array({"name"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			MutationMode::SceneMutation,
			Availability(false, false),
			WriteExecution("post_editor_automation_flush", {"ObjectStorage", "EditorSelection"}, {"ObjectStorage", "SceneDirtyState", "EditorHistory", "EditorSelection"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"object.delete",
			"object",
			"Delete a scene object after approval while retaining undo data.",
			ObjectSchema({{"object", target}}, Json::array({"object"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::Destructive,
			MutationMode::Destructive,
			Availability(false, true),
			WriteExecution("post_editor_automation_flush", {"ObjectStorage"}, {"ObjectStorage", "SceneDirtyState", "EditorHistory"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"object.select",
			"object",
			"Select a scene object in the editor.",
			ObjectSchema({{"object", target}}, Json::array({"object"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			MutationMode::EditorSelection,
			Availability(false, true),
			WriteExecution("editor_selection_commit", {"ObjectStorage", "EditorSelection"}, {"EditorSelection"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"object.set_metadata",
			"object",
			"Update reflected object metadata and retain one undoable edit.",
			ObjectSchema({
				{"object", target},
				{"name", {{"type", "string"}}},
				{"tag", {{"type", "string"}}},
				{"layer", {{"type", "string"}}},
				{"active", {{"type", "boolean"}}},
				{"static", {{"type", "boolean"}}}
			}, Json::array({"object"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			MutationMode::SceneMutation,
			Availability(false, true),
			WriteExecution("post_editor_automation_flush", {"ObjectStorage"}, {"ObjectStorage", "SceneDirtyState", "EditorHistory"})));

		capabilities.push_back(Registration(
			resolveHandler,
			"object.set_parent",
			"object",
			"Reparent or unparent a scene object with hierarchy validation.",
			ObjectSchema({
				{"object", target},
				{"parent", Json{{"oneOf", Json::array({target, Json{{"type", "null"}}})}}}
			}, Json::array({"object"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			MutationMode::SceneMutation,
			Availability(false, true),
			WriteExecution("post_editor_automation_flush", {"ObjectStorage"}, {"ObjectHierarchy", "SceneDirtyState", "EditorHistory"})));

		for (const auto& [name, description, mutationMode] : std::vector<std::tuple<std::string, std::string, MutationMode>>{
			{"component.add", "Add an automation-registered component to a scene object.", MutationMode::SceneMutation},
			{"component.remove", "Remove an automation-registered component while retaining undo data.", MutationMode::SceneMutation},
			{"component.get", "Read reflected properties from an automation-registered component.", MutationMode::ReadOnly}
		})
		{
			const bool readOnly = mutationMode == MutationMode::ReadOnly;
			capabilities.push_back(Registration(
				resolveHandler,
				name,
				"component",
				description,
				ObjectSchema({
					{"object", target},
					{"component", {{"type", "string"}, {"minLength", 1}}}
				}, Json::array({"object", "component"})),
				readOnly ? AutomationPermission::Read : AutomationPermission::ProjectWrite,
				AutomationApprovalPolicy::None,
				mutationMode,
				Availability(false, true),
				readOnly
					? ReadExecution({"ObjectStorage", "ComponentAccessRegistry"})
					: WriteExecution("post_editor_automation_flush", {"ObjectStorage", "ComponentAccessRegistry"}, {"ComponentStorage", "SceneDirtyState", "EditorHistory"})));
		}

		capabilities.push_back(Registration(
			resolveHandler,
			"component.set_property",
			"component",
			"Set one reflected component property while retaining undo data.",
			ObjectSchema({
				{"object", target},
				{"component", {{"type", "string"}, {"minLength", 1}}},
				{"property", {{"type", "string"}, {"minLength", 1}}},
				{"value", Json::object()}
			}, Json::array({"object", "component", "property", "value"})),
			AutomationPermission::ProjectWrite,
			AutomationApprovalPolicy::None,
			MutationMode::SceneMutation,
			Availability(false, true),
			WriteExecution("post_editor_automation_flush", {"ObjectStorage", "ComponentAccessRegistry"}, {"ComponentStorage", "SceneDirtyState", "EditorHistory"})));

		return capabilities;
	}
}
