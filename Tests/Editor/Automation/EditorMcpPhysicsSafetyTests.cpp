// EditorMcpPhysicsSafetyTests.cpp - Approval, generation, and retry contracts.

#include "../TestHarness.h"

#include "EditorAutomationIdempotencyStore.h"
#include "EditorAutomationJobService.h"
#include "EditorAutomationLogStore.h"
#include "EditorAutomationRegistry.h"
#include "Providers/PhysicsAutomationProvider.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		std::shared_ptr<PhysicsAutomationProvider> MakeProvider()
		{
			return std::make_shared<PhysicsAutomationProvider>([](std::string_view name) {
				return [operation = std::string(name)](const AutomationRequestContext&) {
					return AutomationDispatchResult{true, ErrorCategory::Internal, {}, {{"operation", operation}}};
				};
			});
		}

		AutomationCapabilitySnapshot RequireCapability(
			TestContext& context,
			const EditorAutomationRegistry& capabilityRegistry,
			std::string_view name)
		{
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.playState = "playing";
			const auto capability = capabilityRegistry.Find(name, 1, state);
			context.Expect(capability.has_value(), std::string(name) + " must be registered.");
			if (!capability.has_value())
				throw TestFailure("Missing mandatory physics capability.");
			// Find returns an optional value snapshot. Return a value here so callers
			// never keep a reference to that temporary after this helper returns.
			return *capability;
		}

		bool Requires(const nlohmann::json& schema, std::string_view property)
		{
			if (!schema.contains("required") || !schema["required"].is_array())
				return false;
			for (const nlohmann::json& required : schema["required"])
			{
				if (required.is_string() && required.get<std::string>() == property)
					return true;
			}
			return false;
		}
	}

	void RegisterEditorMcpPhysicsSafetyTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Physics.RuntimeCommandsRequireIdempotencyAndExactGenerations", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			context.Expect(capabilityRegistry.RegisterProvider(MakeProvider()).registered, "Physics provider registration should succeed.");

			for (const std::string_view name : {"physics.body.apply_force", "physics.body.apply_impulse", "physics.body.teleport", "physics.body.set_velocity", "physics.character.move", "physics.character.jump"})
			{
				const AutomationCapabilitySnapshot& capability = RequireCapability(context, capabilityRegistry, name);
				const nlohmann::json& schema = capability.descriptor.inputSchema;
				const nlohmann::json& properties = schema["properties"];
				context.Expect(properties.contains("idempotencyKey"), "Every runtime mutation needs an idempotency key before work is queued.");
				context.Expect(properties.contains("worldGeneration"), "Every runtime mutation must name its expected world generation.");
				context.Expect(Requires(schema, "idempotencyKey"), "Idempotency keys must be mandatory, not advisory.");
				context.ExpectEqual(std::string("physics_command_commit"), capability.descriptor.execution.visibilityBoundary, "Runtime commands must use the typed physics commit boundary.");
				context.Expect(capability.descriptor.execution.commitWrites == std::vector<std::string>{"PhysicsCommandQueue", "EditorAutomationSession"}, "Runtime commands must never write ObjectManager or provider state directly.");
				if (properties.contains("target"))
				{
					const nlohmann::json& target = properties["target"];
					context.Expect(Requires(target, "objectId"), "Targets must keep the authored object UUID.");
					context.Expect(Requires(target, "componentGeneration"), "Targets must reject stale component recreation.");
					context.Expect(Requires(target, "worldGeneration"), "Targets must reject stale physics worlds.");
				}
			}
		});

		registry.Add("Editor.MCP.Physics.AuthoringPreflightBindsApprovalAndReferenceImpact", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			context.Expect(capabilityRegistry.RegisterProvider(MakeProvider()).registered, "Physics provider registration should succeed.");
			const AutomationCapabilitySnapshot& preflight = RequireCapability(context, capabilityRegistry, "physics.settings.preflight");
			const AutomationCapabilitySnapshot& set = RequireCapability(context, capabilityRegistry, "physics.settings.set");

			const nlohmann::json& preflightProperties = preflight.descriptor.resultSchema["properties"];
			context.Expect(preflightProperties.contains("impactFingerprint"), "A settings preflight must produce a stable impact fingerprint.");
			context.Expect(preflightProperties.contains("referenceCount"), "Reference-impact preflight must report affected references.");
			context.Expect(preflightProperties.contains("projectGeneration"), "Preflight output must bind the approval to a project generation.");
			context.Expect(set.descriptor.approvalPolicy == AutomationApprovalPolicy::HighImpact, "Settings publication must require high-impact approval.");
			const nlohmann::json& approval = set.descriptor.inputSchema["properties"]["approval"];
			context.Expect(Requires(approval, "approvalId"), "Publication must carry the approval token.");
			context.Expect(Requires(approval, "impactFingerprint"), "Publication must revalidate the preflight fingerprint.");
			context.Expect(Requires(approval, "projectGeneration"), "Publication must revalidate the project generation.");
			context.ExpectEqual(std::string("post_editor_automation_flush"), set.descriptor.execution.visibilityBoundary, "Authoring changes must publish only after the protected editor flush.");

			for (const std::string_view name : {"physics.layer.create", "physics.layer.update", "physics.collider.cook"})
			{
				const AutomationCapabilitySnapshot& capability = RequireCapability(context, capabilityRegistry, name);
				const nlohmann::json& schema = capability.descriptor.inputSchema;
				context.Expect(schema["properties"].contains("approval"), "Every reference-impact mutation must expose an approval envelope.");
				context.Expect(Requires(schema, "approval"), "Approval must be mandatory in the public mutation schema, not merely injected by the bridge.");
			}
		});

		registry.Add("Editor.MCP.Physics.CookingAndJobsAreDurableAndCancellableByContract", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			context.Expect(capabilityRegistry.RegisterProvider(MakeProvider()).registered, "Physics provider registration should succeed.");
			const AutomationCapabilitySnapshot& cook = RequireCapability(context, capabilityRegistry, "physics.collider.cook");
			const AutomationCapabilitySnapshot& job = RequireCapability(context, capabilityRegistry, "physics.query_job.get");

			context.Expect(cook.descriptor.inputSchema["properties"].contains("idempotencyKey"), "Cooking retries need an idempotency key.");
			context.Expect(cook.descriptor.resultSchema["properties"].contains("jobId"), "Cooking must return a reconnectable durable job receipt.");
			context.Expect(cook.descriptor.execution.commitWrites == std::vector<std::string>{"PhysicsCookJobQueue", "EditorAutomationJobs", "EditorAutomationSession"}, "Cook work must enter the durable job queue rather than publish an asset from a worker.");
			context.ExpectEqual(std::string("immutable_physics_snapshot"), job.descriptor.execution.visibilityBoundary, "Job polling must use immutable retained receipts.");
			context.Expect(cook.descriptor.permission == AutomationPermission::ProjectWrite, "Cooking must not run for read-only automation clients.");
		});

		registry.Add("Editor.MCP.Physics.ReadOnlyClientsCannotSubmitProjectMutations", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			context.Expect(capabilityRegistry.RegisterProvider(MakeProvider()).registered, "Physics provider registration should succeed.");
			AutomationRequestContext request;
			request.state.projectAvailable = true;
			request.state.sceneAvailable = true;
			request.state.playState = "playing";
			request.grantedPermission = AutomationPermission::Read;
			const AutomationDispatchResult result = capabilityRegistry.Dispatch("physics.settings.set", 1, request);
			context.Expect(!result.ok, "A read-only caller must not reach the settings mutation handler.");
			context.Expect(result.error == ErrorCategory::Permission, "Permission denial should be structured rather than a handler failure.");
		});

		registry.Add("Editor.MCP.Physics.ImpulseRetriesAndDurableCookReceiptsRemainCorrelated", [](TestContext& context) {
			EditorAutomationIdempotencyStore idempotency;
			const IdempotencyRequest impulse{
				"physics-client",
				"physics.body.apply_impulse",
				"impulse-7",
				"world=12|object=crate|component=4|payload=up",
				{{"accepted", true}, {"correlationId", "physics-correlation"}}};
			context.Expect(idempotency.Begin(impulse).disposition == IdempotencyDisposition::NewRequest, "The first typed impulse should reserve its idempotency key.");
			context.Expect(idempotency.PublishResult("physics-client", "physics.body.apply_impulse", "impulse-7", {{"commandId", "command-7"}}, true), "The protected command receipt should become terminal only after acceptance.");
			const IdempotencyDecision replay = idempotency.Begin(impulse);
			context.Expect(replay.disposition == IdempotencyDisposition::Replay && replay.terminal, "An identical impulse retry must replay its retained receipt.");
			context.Expect(idempotency.Begin({"physics-client", "physics.body.apply_impulse", "impulse-7", "world=13|object=crate|component=4|payload=up", {}}).disposition == IdempotencyDisposition::Conflict,
				"Changing a world generation with the same key must be rejected as a conflict.");

			EditorAutomationJobService jobs;
			const AutomationJobSnapshot cook = jobs.Create({"physics_cook", "physics.collider.cook", "cook-request", "physics-correlation", "cook-7", true});
			context.Expect(!cook.jobId.empty(), "A collider cook must allocate a durable job identity.");
			context.Expect(jobs.RequestCancellation(cook.jobId), "A queued cook should support cooperative cancellation.");
			context.Expect(jobs.Get(cook.jobId).has_value(), "A reconnect should retrieve the same retained cook receipt.");

			EditorAutomationLogStore logs;
			StructuredLogRecord log;
			log.category = "physics";
			log.source = "PhysicsAutomationProvider";
			log.correlationId = "physics-correlation";
			log.jobId = cook.jobId;
			log.message = "Cook cancellation requested.";
			(void)logs.Append(std::move(log));
			StructuredLogQuery query;
			query.correlationId = "physics-correlation";
			const StructuredLogPage page = logs.Query(query);
			context.Expect(page.valid && page.records.size() == 1, "Physics receipts and diagnostics must remain correlated for reconnect and troubleshooting.");
		});
	}
}
