// PhysicsAutomationProvider.cpp - Bounded, self-registering physics MCP tools.

#include "PhysicsAutomationProvider.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;

		constexpr std::uint32_t kMaximumQueryHits = 64u;
		constexpr std::uint32_t kMaximumQueryBatchSize = 1024u;
		constexpr double kMaximumQueryDistance = 100000.0;
		constexpr double kMaximumShapeExtent = 10000.0;

		Json ObjectSchema(Json properties, Json required = Json::array())
		{
			Json schema = {
				{"type", "object"},
				{"properties", std::move(properties)},
				{"additionalProperties", false}
			};
			if (!required.empty())
				schema["required"] = std::move(required);
			return schema;
		}

		Json UIntSchema()
		{
			return {{"type", "integer"}, {"minimum", 0}};
		}

		Json IdentifierSchema()
		{
			return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
		}

		Json FiniteNumberSchema(double minimum, double maximum)
		{
			// JSON has no portable finite-number keyword.  The explicit bounds let
			// the service reject infinities and NaNs before a command reaches physics.
			return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
		}

		Json Vector3Schema(double minimum = -kMaximumQueryDistance, double maximum = kMaximumQueryDistance)
		{
			return {
				{"type", "array"}, {"minItems", 3}, {"maxItems", 3},
				{"items", FiniteNumberSchema(minimum, maximum)}
			};
		}

		Json TargetSchema(bool requiresRecord = false)
		{
			Json required = Json::array({"objectId", "componentGeneration", "worldGeneration"});
			if (requiresRecord)
				required.push_back("recordId");
			return ObjectSchema({
				{"objectId", IdentifierSchema()},
				{"componentGeneration", UIntSchema()},
				{"recordId", IdentifierSchema()},
				{"recordGeneration", UIntSchema()},
				{"worldGeneration", UIntSchema()}
			}, std::move(required));
		}

		Json ApprovalSchema()
		{
			return ObjectSchema({
				{"approvalId", IdentifierSchema()},
				{"impactFingerprint", IdentifierSchema()},
				{"projectGeneration", UIntSchema()},
				{"sceneGeneration", UIntSchema()}
			}, {"approvalId", "impactFingerprint", "projectGeneration"});
		}

		Json PreflightSchema()
		{
			return ObjectSchema({
				{"impactFingerprint", IdentifierSchema()},
				{"projectGeneration", UIntSchema()},
				{"sceneGeneration", UIntSchema()},
				{"referenceCount", UIntSchema()},
				{"affectedObjects", { {"type", "array"}, {"maxItems", 256}, {"items", IdentifierSchema()} }},
				{"requiresApproval", { {"type", "boolean"} }},
				{"expirySeconds", { {"type", "integer"}, {"minimum", 1}, {"maximum", 600} }}
			}, {"impactFingerprint", "projectGeneration", "referenceCount", "requiresApproval"});
		}

		Json MutationReceiptSchema(bool durable = false)
		{
			Json properties = {
				{"correlationId", IdentifierSchema()},
				{"worldGeneration", UIntSchema()},
				{"tick", UIntSchema()},
				{"accepted", {{"type", "boolean"}}},
				{"idempotencyDisposition", {{"type", "string"}, {"enum", {"new", "replay", "conflict"}}}},
				{"diagnostics", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "object"}}}}}
			};
			if (durable)
				properties["jobId"] = IdentifierSchema();
			return ObjectSchema(std::move(properties), {"correlationId", "accepted", "idempotencyDisposition", "diagnostics"});
		}

		Json SnapshotResultSchema()
		{
			return ObjectSchema({
				{"correlationId", IdentifierSchema()},
				{"worldGeneration", UIntSchema()},
				{"tick", UIntSchema()},
				{"truncated", {{"type", "boolean"}}},
				{"overflow", {{"type", "boolean"}}},
				{"diagnostics", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "object"}}}}},
				{"data", {{"type", "object"}}}
			}, {"correlationId", "worldGeneration", "tick", "truncated", "overflow", "diagnostics", "data"});
		}

		Json CommandEnvelope(Json properties, Json required)
		{
			properties["worldGeneration"] = UIntSchema();
			properties["idempotencyKey"] = IdentifierSchema();
			properties["correlationId"] = IdentifierSchema();
			required.push_back("worldGeneration");
			required.push_back("idempotencyKey");
			return ObjectSchema(std::move(properties), std::move(required));
		}

		AutomationCapabilityRegistration MakeRegistration(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			std::string description,
			Json inputSchema,
			Json resultSchema,
			MutationMode mutationMode,
			AutomationApprovalPolicy approvalPolicy,
			bool requiresScene,
			std::vector<std::string> allowedPlayStates,
			std::string visibilityBoundary,
			std::vector<std::string> preparationReads,
			std::vector<std::string> commitWrites = {})
		{
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.category = std::move(category);
			descriptor.description = std::move(description);
			descriptor.inputSchema = std::move(inputSchema);
			descriptor.resultSchema = std::move(resultSchema);
			descriptor.permission = mutationMode == MutationMode::ReadOnly ? AutomationPermission::Read : AutomationPermission::ProjectWrite;
			descriptor.approvalPolicy = approvalPolicy;
			descriptor.mutationMode = mutationMode;
			descriptor.availability.requiresProject = true;
			descriptor.availability.requiresScene = requiresScene;
			descriptor.availability.allowedPlayStates = std::move(allowedPlayStates);
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = mutationMode != MutationMode::ReadOnly;
			descriptor.execution.visibilityBoundary = std::move(visibilityBoundary);
			descriptor.execution.preparationReads = std::move(preparationReads);
			if (mutationMode != MutationMode::ReadOnly)
			{
				descriptor.execution.commitReads = {"EditorAutomationSession", "PhysicsSettings", "PhysicsSnapshot"};
				descriptor.execution.commitWrites = std::move(commitWrites);
			}

			const std::string toolName = descriptor.name;
			return {std::move(descriptor), resolver(toolName), {}};
		}

		Json QuerySchema(std::string_view kind)
		{
			Json properties = {
				{"worldGeneration", UIntSchema()},
				{"origin", Vector3Schema()},
				{"layerMask", UIntSchema()},
				{"includeTriggers", {{"type", "boolean"}}},
				{"maximumHits", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumQueryHits}}},
				{"batchSize", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumQueryBatchSize}}},
				{"correlationId", IdentifierSchema()}
			};
			if (kind == "ray" || kind == "shape_cast")
			{
				properties["direction"] = Vector3Schema(-1.0, 1.0);
				properties["distance"] = FiniteNumberSchema(0.0, kMaximumQueryDistance);
			}
			if (kind == "shape_cast" || kind == "overlap")
				properties["halfExtents"] = Vector3Schema(0.0001, kMaximumShapeExtent);
			return ObjectSchema(std::move(properties), {"worldGeneration", "origin", "maximumHits"});
		}
	}

	PhysicsAutomationProvider::PhysicsAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view PhysicsAutomationProvider::GetProviderId() const
	{
		return "physics";
	}

	std::optional<TerraEngine::Physics::PhysicsCommand> PhysicsAutomationProvider::TryCreateApplyForceCommand(
		std::string_view applicationMode,
		const TerraEngine::Physics::PhysicsBodyCommandTarget& target,
		TerraEngine::Physics::PhysicsVector3 value,
		TerraEngine::Physics::PhysicsCommandMetadata metadata)
	{
		// Automation exposes only force and mass-independent acceleration here.
		// Impulse and velocity changes have dedicated capabilities, so accepting
		// their spellings would silently change the fixed-step command semantics.
		if (applicationMode == "Force")
			return TerraEngine::Physics::PhysicsCommand::MakeContinuousForce(target, value, std::move(metadata));
		if (applicationMode == "Acceleration")
			return TerraEngine::Physics::PhysicsCommand::MakeAcceleration(target, value, std::move(metadata));
		return std::nullopt;
	}

	std::vector<AutomationCapabilityRegistration> PhysicsAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> capabilities;
		capabilities.reserve(25);
		const Json snapshotResult = SnapshotResultSchema();
		const std::vector<std::string> worldStates{"playing", "paused"};

		// All reads are served from immutable physics publication packets.  They
		// deliberately cannot acquire or expose a provider-world pointer.
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.state", "physics", "Read the published physics world lifecycle, generation, tick, and bounded counts.", ObjectSchema({}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"PhysicsSnapshot", "PhysicsDiagnostics"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.settings.get", "physics", "Read resolved engine-owned physics settings without provider-private values.", ObjectSchema({}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"PhysicsSettings", "ProjectState"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.layers.list", "physics", "List stable collision layers and bounded reference-impact summaries.", ObjectSchema({}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"PhysicsSettings", "PhysicsReferenceIndex"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.material.inspect", "physics", "Inspect one project-owned physics material and its safe reference summary.", ObjectSchema({{"materialId", IdentifierSchema()}} , {"materialId"}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"AssetDatabaseSnapshot", "PhysicsMaterialIndex"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.body.inspect", "physics", "Inspect authored identity and immutable runtime body state using exact generations.", ObjectSchema({{"target", TargetSchema()}} , {"target"}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, true, worldStates, "immutable_physics_snapshot", {"PhysicsBodySnapshot", "ObjectStorage"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.character.inspect", "physics", "Inspect immutable character grounding, support, and contact state using exact generations.", ObjectSchema({{"target", TargetSchema()}} , {"target"}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, true, worldStates, "immutable_physics_snapshot", {"PhysicsCharacterSnapshot", "ObjectStorage"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.contacts.list", "physics", "List ordered published contacts with an explicit bounded target, layer, and tick range.", ObjectSchema({{"worldGeneration", UIntSchema()}, {"target", TargetSchema()}, {"collisionLayer", {{"type", "integer"}, {"minimum", 0}, {"maximum", 31}}}, {"maximumContacts", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumQueryHits}}}, {"fromTick", UIntSchema()}, {"toTick", UIntSchema()}} , {"worldGeneration", "maximumContacts"}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, true, worldStates, "immutable_physics_snapshot", {"PhysicsEventSnapshot"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.diagnostics.get", "physics", "Read bounded authoring, preparation, extraction, state-store, publication, health, and scheduler diagnostics without renderer access.", ObjectSchema({{"includeFailures", {{"type", "boolean"}}}}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"PhysicsDiagnostics"}));

		capabilities.push_back(MakeRegistration(resolveHandler, "physics.raycast", "physics_query", "Run a bounded ray query against a published/protected physics boundary.", QuerySchema("ray"), MutationReceiptSchema(true), MutationMode::ReadOnly, AutomationApprovalPolicy::None, true, worldStates, "immutable_physics_snapshot", {"PhysicsQuerySnapshot", "PhysicsSettings"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.shape_cast", "physics_query", "Run a bounded shape cast against a published/protected physics boundary.", QuerySchema("shape_cast"), MutationReceiptSchema(true), MutationMode::ReadOnly, AutomationApprovalPolicy::None, true, worldStates, "immutable_physics_snapshot", {"PhysicsQuerySnapshot", "PhysicsSettings"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.overlap", "physics_query", "Run a bounded overlap query against a published/protected physics boundary.", QuerySchema("overlap"), MutationReceiptSchema(true), MutationMode::ReadOnly, AutomationApprovalPolicy::None, true, worldStates, "immutable_physics_snapshot", {"PhysicsQuerySnapshot", "PhysicsSettings"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.query_job.get", "physics_query", "Read a durable bounded physics query or cook job receipt.", ObjectSchema({{"jobId", IdentifierSchema()}} , {"jobId"}), snapshotResult, MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"EditorAutomationJobs"}));

		const Json settingsPatch = ObjectSchema({
			{"gravity", Vector3Schema()},
			{"fixedTimeStep", FiniteNumberSchema(0.0001, 1.0)},
			{"maxSubsteps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
			{"debugVisualizationEnabled", {{"type", "boolean"}}},
			{"retainedDebugTicks", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}}},
			{"maxDebugPrimitives", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}}}
		});
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.settings.preflight", "physics_authoring", "Preflight a settings patch and return its reference-impact fingerprint before approval.", ObjectSchema({{"settings", settingsPatch}, {"projectGeneration", UIntSchema()}, {"sceneGeneration", UIntSchema()}} , {"settings", "projectGeneration"}), PreflightSchema(), MutationMode::ReadOnly, AutomationApprovalPolicy::None, false, {}, "immutable_physics_snapshot", {"PhysicsSettings", "PhysicsReferenceIndex"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.settings.set", "physics_authoring", "Atomically queue a fingerprint-approved settings update for the protected physics settings boundary.", CommandEnvelope({{"settings", settingsPatch}, {"approval", ApprovalSchema()}, {"projectGeneration", UIntSchema()}}, {"settings", "approval", "projectGeneration"}), MutationReceiptSchema(), MutationMode::ProjectMutation, AutomationApprovalPolicy::HighImpact, false, {}, "post_editor_automation_flush", {"PhysicsSettings", "PhysicsReferenceIndex", "ProjectState"}, {"PhysicsSettingsUpdateQueue", "EditorAutomationSession"}));

		const Json layerPatch = ObjectSchema({{"id", {{"type", "integer"}, {"minimum", 0}, {"maximum", 31}}}, {"name", IdentifierSchema()}, {"collisionMask", UIntSchema()}, {"broadPhaseGroup", UIntSchema()}, {"enabled", {{"type", "boolean"}}}});
		// Layer slots are project-wide collision policy.  Creation and replacement
		// therefore share the same explicit approval contract; omitting it here
		// would make the schema disagree with the handler's stale-impact gate.
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.layer.create", "physics_authoring", "Create a collision layer after bounded reference-impact preflight.", CommandEnvelope({{"layer", layerPatch}, {"approval", ApprovalSchema()}, {"projectGeneration", UIntSchema()}}, {"layer", "approval", "projectGeneration"}), MutationReceiptSchema(), MutationMode::ProjectMutation, AutomationApprovalPolicy::HighImpact, false, {}, "post_editor_automation_flush", {"PhysicsSettings", "PhysicsReferenceIndex"}, {"PhysicsSettingsUpdateQueue", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.layer.update", "physics_authoring", "Update a collision layer only when its fingerprint and affected generations still match.", CommandEnvelope({{"layer", layerPatch}, {"approval", ApprovalSchema()}, {"projectGeneration", UIntSchema()}}, {"layer", "approval", "projectGeneration"}), MutationReceiptSchema(), MutationMode::ProjectMutation, AutomationApprovalPolicy::HighImpact, false, {}, "post_editor_automation_flush", {"PhysicsSettings", "PhysicsReferenceIndex"}, {"PhysicsSettingsUpdateQueue", "EditorAutomationSession"}));

		const Json materialPatch = ObjectSchema({{"materialId", IdentifierSchema()}, {"staticFriction", FiniteNumberSchema(0.0, 100.0)}, {"dynamicFriction", FiniteNumberSchema(0.0, 100.0)}, {"restitution", FiniteNumberSchema(0.0, 1.0)}, {"density", FiniteNumberSchema(0.0001, 100000.0)}});
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.material.create", "physics_authoring", "Create a bounded project material through atomic asset publication.", CommandEnvelope({{"material", materialPatch}, {"projectGeneration", UIntSchema()}}, {"material", "projectGeneration"}), MutationReceiptSchema(), MutationMode::AssetMutation, AutomationApprovalPolicy::None, false, {}, "post_editor_automation_flush", {"AssetDatabaseSnapshot", "ProjectState"}, {"PhysicsMaterialAssetStore", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.material.update", "physics_authoring", "Update a material only after revalidating its impact fingerprint and target generation.", CommandEnvelope({{"material", materialPatch}, {"approval", ApprovalSchema()}, {"projectGeneration", UIntSchema()}}, {"material", "approval", "projectGeneration"}), MutationReceiptSchema(), MutationMode::AssetMutation, AutomationApprovalPolicy::HighImpact, false, {}, "post_editor_automation_flush", {"AssetDatabaseSnapshot", "PhysicsReferenceIndex"}, {"PhysicsMaterialAssetStore", "EditorAutomationSession"}));
		// Cooking can replace a referenced asset.  Require the bridge-issued
		// approval envelope in the public schema as well as at execution time.
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.collider.cook", "physics_authoring", "Queue a cancellable durable collider cook; cancellation before publication leaves no partial asset.", CommandEnvelope({{"sourceAsset", IdentifierSchema()}, {"recipe", ObjectSchema({{"shape", IdentifierSchema()}, {"maxVertices", {{"type", "integer"}, {"minimum", 4}, {"maximum", 1000000}}}} , {"shape"})}, {"approval", ApprovalSchema()}, {"projectGeneration", UIntSchema()}}, {"sourceAsset", "recipe", "approval", "projectGeneration"}), MutationReceiptSchema(true), MutationMode::AssetMutation, AutomationApprovalPolicy::HighImpact, false, {}, "post_editor_automation_flush", {"AssetDatabaseSnapshot", "PhysicsCookSettings", "PhysicsReferenceIndex"}, {"PhysicsCookJobQueue", "EditorAutomationJobs", "EditorAutomationSession"}));

		const Json bodyCommand = CommandEnvelope({{"target", TargetSchema()}, {"vector", Vector3Schema()}, {"applyOnNextStep", {{"type", "boolean"}}}}, {"target", "vector"});
		const Json forceCommand = CommandEnvelope({{"target", TargetSchema()}, {"vector", Vector3Schema()}, {"applicationMode", {{"type", "string"}, {"enum", {"Force", "Acceleration"}}, {"default", "Force"}}}, {"applyOnNextStep", {{"type", "boolean"}}}}, {"target", "vector"});
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.body.apply_force", "physics_runtime", "Enqueue a typed fixed-step Force (default) or mass-independent Acceleration command; duplicate keys return the original receipt.", forceCommand, MutationReceiptSchema(), MutationMode::SceneMutation, AutomationApprovalPolicy::None, true, worldStates, "physics_command_commit", {"PhysicsSnapshot", "ObjectStorage"}, {"PhysicsCommandQueue", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.body.apply_impulse", "physics_runtime", "Enqueue a typed fixed-step impulse command; it never mutates a live body directly.", bodyCommand, MutationReceiptSchema(), MutationMode::SceneMutation, AutomationApprovalPolicy::None, true, worldStates, "physics_command_commit", {"PhysicsSnapshot", "ObjectStorage"}, {"PhysicsCommandQueue", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.body.set_velocity", "physics_runtime", "Enqueue a typed velocity command guarded by object, component, and world generations.", bodyCommand, MutationReceiptSchema(), MutationMode::SceneMutation, AutomationApprovalPolicy::None, true, worldStates, "physics_command_commit", {"PhysicsSnapshot", "ObjectStorage"}, {"PhysicsCommandQueue", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.body.teleport", "physics_runtime", "Enqueue a typed teleport command at the declared protected physics boundary.", CommandEnvelope({{"target", TargetSchema()}, {"position", Vector3Schema()}, {"applyOnNextStep", {{"type", "boolean"}}}}, {"target", "position"}), MutationReceiptSchema(), MutationMode::SceneMutation, AutomationApprovalPolicy::None, true, worldStates, "physics_command_commit", {"PhysicsSnapshot", "ObjectStorage"}, {"PhysicsCommandQueue", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.character.move", "physics_runtime", "Enqueue bounded desired character movement with exact target and world generations.", CommandEnvelope({{"target", TargetSchema()}, {"desiredVelocity", Vector3Schema()}, {"applyOnNextStep", {{"type", "boolean"}}}}, {"target", "desiredVelocity"}), MutationReceiptSchema(), MutationMode::SceneMutation, AutomationApprovalPolicy::None, true, worldStates, "physics_command_commit", {"PhysicsSnapshot", "ObjectStorage"}, {"PhysicsCommandQueue", "EditorAutomationSession"}));
		capabilities.push_back(MakeRegistration(resolveHandler, "physics.character.jump", "physics_runtime", "Enqueue one typed character jump request with idempotent fixed-step ordering.", CommandEnvelope({{"target", TargetSchema()}, {"applyOnNextStep", {{"type", "boolean"}}}}, {"target"}), MutationReceiptSchema(), MutationMode::SceneMutation, AutomationApprovalPolicy::None, true, worldStates, "physics_command_commit", {"PhysicsSnapshot", "ObjectStorage"}, {"PhysicsCommandQueue", "EditorAutomationSession"}));

		return capabilities;
	}
}
