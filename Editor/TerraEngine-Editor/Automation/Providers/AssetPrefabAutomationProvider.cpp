// AssetPrefabAutomationProvider.cpp - Registers existing asset MCP tools.

#include "AssetPrefabAutomationProvider.h"

#include <utility>
#include <tuple>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;

		Json ObjectSchema(Json properties, Json required = Json::array())
		{
			Json schema = {
				{"type", "object"},
				{"properties", std::move(properties)},
				{"additionalProperties", false}
			};
			if (!required.empty())
			{
				schema["required"] = std::move(required);
			}
			return schema;
		}

		Json ResultSchema()
		{
			// Existing handlers place their capability-specific payload in data and
			// may add compatibility envelope fields such as request status/error.
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

		AutomationCapabilityRegistration MakeRegistration(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string description,
			Json inputSchema,
			MutationMode mutationMode,
			AutomationApprovalPolicy approvalPolicy)
		{
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.category = "asset";
			descriptor.description = std::move(description);
			descriptor.inputSchema = std::move(inputSchema);
			descriptor.resultSchema = ResultSchema();
			descriptor.permission = mutationMode == MutationMode::ReadOnly
				? AutomationPermission::Read
				: AutomationPermission::ProjectWrite;
			descriptor.approvalPolicy = approvalPolicy;
			descriptor.mutationMode = mutationMode;
			descriptor.availability.requiresProject = true;
			descriptor.availability.requiresScene = false;
			descriptor.availability.requiresRenderer = false;

			// Asset enumeration and validation can prepare from immutable project
			// snapshots. Mutations publish only through the editor-owned asset
			// boundary so worker tasks never write the live AssetDatabase.
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = mutationMode != MutationMode::ReadOnly;
			descriptor.execution.visibilityBoundary = mutationMode == MutationMode::ReadOnly
				? "immutable_snapshot"
				: "editor_protected_commit";
			descriptor.execution.preparationReads = {
				"EditorAutomationSession",
				"ProjectState",
				"AssetDatabaseSnapshot"
			};
			if (mutationMode != MutationMode::ReadOnly)
			{
				descriptor.execution.commitReads = {"ProjectState", "AssetDatabase"};
				descriptor.execution.commitWrites = {"AssetDatabase", "EditorAutomationSession"};
			}

			const std::string toolName = descriptor.name;
			return {
				std::move(descriptor),
				resolver(toolName),
				{}
			};
		}

		Json AssetReferenceSchema()
		{
			return {{"type", "string"}, {"minLength", 1}};
		}

		Json OptionalAssetReferenceSchema()
		{
			return {{"oneOf", Json::array({AssetReferenceSchema(), Json{{"type", "null"}}})}};
		}

		AutomationCapabilityRegistration MakePrefabInstantiationRegistration(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string description,
			Json inputSchema)
		{
			auto registration = MakeRegistration(
				resolver,
				std::move(name),
				std::move(description),
				std::move(inputSchema),
				MutationMode::SceneMutation,
				AutomationApprovalPolicy::None);
			registration.descriptor.category = "prefab";
			registration.descriptor.availability.requiresScene = true;
			registration.descriptor.execution.visibilityBoundary = "prefab_bulk_publication";
			registration.descriptor.execution.commitReads = {
				"ProjectState",
				"AssetDatabase",
				"ObjectStorage",
				"SceneGraph"
			};
			registration.descriptor.execution.commitWrites = {
				"ObjectStorage",
				"SceneGraph",
				"EditorHistory",
				"EditorAutomationSession"
			};
			return registration;
		}
	}

	AssetPrefabAutomationProvider::AssetPrefabAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view AssetPrefabAutomationProvider::GetProviderId() const
	{
		return "asset_prefab";
	}

	std::vector<AutomationCapabilityRegistration> AssetPrefabAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> capabilities;
		capabilities.reserve(22);
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.list",
			"List assets in the active project with optional type and text filters.",
			ObjectSchema({
				{"type", {{"type", "string"}}},
				{"search", {{"type", "string"}}}
			}),
			MutationMode::ReadOnly,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.import",
			"Import or reimport a project-relative source asset.",
			ObjectSchema({
				{"path", AssetReferenceSchema()},
				{"force", {{"type", "boolean"}}},
				// The handler validates the same containment/database contract and
				// returns before mutation when dryRun is true. Advertise that public
				// preflight surface so strict MCP clients do not reject a supported key.
				{"dryRun", {{"type", "boolean"}}}
			}, {"path"}),
			MutationMode::AssetMutation,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.content.verify",
			"Read one project asset repeatedly through ResourceManager's default reusable staging and return bounded integrity and timing evidence.",
			ObjectSchema({
				{"path", AssetReferenceSchema()},
				{"requestCount", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32768}}},
				{"cancellationEvery", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32768}}},
				{"expectedPayloadBytes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 268435456}}}
			}, {"path", "requestCount"}),
			MutationMode::ReadOnly,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.virtualGeometry.settings.get",
			"Read project defaults and optional per-mesh virtual-geometry settings.",
			ObjectSchema({
				{"asset", OptionalAssetReferenceSchema()},
				{"includeResolved", {{"type", "boolean"}}}
			}),
			MutationMode::ReadOnly,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.virtualGeometry.settings.set",
			"Validate and optionally save sparse virtual-geometry settings for one mesh.",
			ObjectSchema({
				{"asset", AssetReferenceSchema()},
				{"override", {{"type", "object"}, {"additionalProperties", true}}},
				{"validateOnly", {{"type", "boolean"}}},
				{"rebuildAfterSave", {{"type", "boolean"}}}
			}, {"asset", "override"}),
			MutationMode::AssetMutation,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.virtualGeometry.settings.reset",
			"Remove a mesh override and optionally rebuild its virtual geometry.",
			ObjectSchema({
				{"asset", AssetReferenceSchema()},
				{"rebuildAfterReset", {{"type", "boolean"}}}
			}, {"asset"}),
			MutationMode::AssetMutation,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.virtualGeometry.rebuild",
			"Rebuild virtual geometry for one asset or every eligible project asset.",
			ObjectSchema({
				{"target", AssetReferenceSchema()},
				{"force", {{"type", "boolean"}}},
				{"includePackageOutputs", {{"type", "boolean"}}},
				{"diagnosticsLevel", {{"type", "string"}, {"enum", {"summary", "detailed", "debug"}}}}
			}, {"target"}),
			MutationMode::AssetMutation,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.virtualGeometry.inspect",
			"Inspect cooked virtual-geometry and optional runtime diagnostics for an asset.",
			ObjectSchema({
				{"asset", AssetReferenceSchema()},
				{"includeRuntimeState", {{"type", "boolean"}}},
				{"viewType", {{"type", "string"}, {"enum", {"Game", "Scene", "Shadow", "ExportRuntime"}}}}
			}, {"asset"}),
			MutationMode::ReadOnly,
			AutomationApprovalPolicy::None));
		capabilities.push_back(MakeRegistration(
			resolveHandler,
			"asset.prepare_delete",
			"Inspect deletion impact and request approval for a project asset.",
			ObjectSchema({{"path", AssetReferenceSchema()}}, {"path"}),
			MutationMode::Destructive,
			AutomationApprovalPolicy::Destructive));
		for (const auto& [name, mode, approval] : std::vector<std::tuple<std::string, MutationMode, AutomationApprovalPolicy>>{
			{"asset.inspect", MutationMode::ReadOnly, AutomationApprovalPolicy::None},
			{"asset.move", MutationMode::AssetMutation, AutomationApprovalPolicy::None},
			{"asset.rename", MutationMode::AssetMutation, AutomationApprovalPolicy::None},
			{"asset.duplicate", MutationMode::AssetMutation, AutomationApprovalPolicy::None}})
		{
			capabilities.push_back(MakeRegistration(resolveHandler, name, "Safely organize a project-contained asset.", ObjectSchema({}, {}), mode, approval));
		}
		capabilities.push_back(MakePrefabInstantiationRegistration(
			resolveHandler,
			"prefab.instantiate",
			"Instantiate one prefab through the protected atomic prefab publication boundary.",
			ObjectSchema({
				{"prefab", AssetReferenceSchema()},
				{"name", {{"type", "string"}}}
			}, {"prefab"})));
		auto instantiateMany = MakePrefabInstantiationRegistration(
			resolveHandler,
			"prefab.instantiate_many",
			"Instantiate a deterministic prefab grid as one protected atomic publication.",
			ObjectSchema({
				{"prefab", AssetReferenceSchema()},
				{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}},
				{"namePrefix", {{"type", "string"}}},
				{"spacing", {{"type", "number"}, {"minimum", 0.0}}},
				{"columns", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}},
				{"requestedAnimationMode", {
					{"type", "integer"},
					{"minimum", 0},
					{"maximum", 3},
					{"description", "Atomic AnimatorComponent execution policy override."}
				}},
				{"defaultClipAssetIds", {
					{"type", "array"},
					{"minItems", 1},
					{"maxItems", 4096},
					{"items", {{"type", "string"}, {"minLength", 32}, {"maxLength", 32}}},
					{"description", "Direct animation clips assigned round-robin before publication."}
				}},
				{"defaultClipStartFrameStep", {
					{"type", "integer"},
					{"minimum", 0},
					{"maximum", 1048576},
					{"description", "Deterministic fixed-timeline phase step between instances."}
				}},
				{"origin", {
					{"type", "array"},
					{"minItems", 3},
					{"maxItems", 3},
					{"items", {{"type", "number"}}}
				}}
			}, {"prefab", "count"}));
		instantiateMany.descriptor.limits = {
			{"maximumCount", 4096u},
			{"publication", "single_generation"}
		};
		instantiateMany.descriptor.diagnosticsContract = {
			{"rollbackField", "rolledBack"},
			{"visibilityBoundary", "prefab_bulk_publication"},
			{"identityFields", {"instanceIds", "instances[].instanceId", "instances[].rootIds"}}
		};
		capabilities.push_back(std::move(instantiateMany));
		for (const auto& [name, mode] : std::vector<std::pair<std::string, MutationMode>>{
			{"prefab.create", MutationMode::AssetMutation},
			{"prefab.overrides.inspect", MutationMode::ReadOnly}, {"prefab.overrides.apply", MutationMode::AssetMutation},
			{"prefab.overrides.revert", MutationMode::SceneMutation}, {"prefab.edit.open", MutationMode::ProjectMutation},
			{"prefab.edit.close", MutationMode::ProjectMutation}})
		{
			auto registration = MakeRegistration(resolveHandler, name, "Author prefabs through editor-owned identity and history services.", ObjectSchema({}, {}), mode, AutomationApprovalPolicy::None);
			registration.descriptor.category = "prefab";
			capabilities.push_back(std::move(registration));
		}
		return capabilities;
	}
}
