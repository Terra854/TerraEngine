// EditorMcpPhysicsSafetyTests.cpp - Approval, generation, and retry contracts.

#include "../TestHarness.h"

#include "EditorAutomationIdempotencyStore.h"
#include "EditorAutomationJobService.h"
#include "EditorAutomationLogStore.h"
#include "EditorAutomationRegistry.h"
#include "EditorAutomationService.h"
#include "Providers/PhysicsAutomationProvider.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsWorld.h"
#include "ThreadPool/ThreadPool.h"

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

		class ThreadPoolSession final
		{
		public:
			ThreadPoolSession()
			{
				// The protected immediate-query and command paths use the shared pool.
				// Isolating this fixture prevents a prior editor test from leaking workers.
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession()
			{
				ThreadPool::instance().shutdown();
			}
		};

		TerraEngine::Physics::PhysicsEvent MakeRetainedContact(
			TerraEngine::Physics::PhysicsWorld& world,
			TerraEngine::Physics::PhysicsTick tick)
		{
			using namespace TerraEngine::Physics;
			PhysicsEvent event;
			event.type = PhysicsEventType::Contact;
			event.world = world.Handle();
			event.tick = tick;
			event.first.objectId = TerraEngine::GenerateUUID();
			event.first.objectGeneration = 1U;
			event.first.componentGeneration = 1U;
			event.first.colliderRecordId = TerraEngine::GenerateUUID();
			event.first.recordGeneration = 1U;
			event.first.body = world.AllocateBody();
			event.first.collisionLayer = 2U;
			event.second.objectId = TerraEngine::GenerateUUID();
			event.second.objectGeneration = 1U;
			event.second.componentGeneration = 1U;
			event.second.colliderRecordId = TerraEngine::GenerateUUID();
			event.second.recordGeneration = 1U;
			event.second.body = world.AllocateBody();
			event.second.collisionLayer = 4U;
			return event;
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
				context.Expect(capability.descriptor.availability.allowedPlayStates == std::vector<std::string>{"playing", "paused"}, "Runtime command discovery must name the only Play Mode states where the handler can apply its explicit-step safety gate.");
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

		registry.Add("Editor.MCP.Physics.PausedCommandsRequireExplicitNextStepConsent", [](TestContext& context) {
			ThreadPoolSession pool;
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The paused-command safety fixture needs a fresh generation-safe physics world.");
			context.Expect(physics.SetPaused(true), "The safety fixture must enter the shared paused physics state.");

			EditorAutomationService service;
			AutomationRequestContext request;
			request.requestId = "paused-command-request";
			request.clientId = "physics-test";
			request.correlationId = "paused-command-correlation";
			request.idempotencyKey = "paused-command-key";
			request.grantedPermission = AutomationPermission::ProjectWrite;
			request.state.projectAvailable = true;
			request.state.sceneAvailable = true;
			request.state.playState = "paused";
			request.arguments = {
				{"worldGeneration", physics.WorldHandle().worldGeneration},
				{"idempotencyKey", request.idempotencyKey},
				{"vector", {0.0F, 1.0F, 0.0F}},
				{"target", {{"objectId", "00000000000000000000000000000001"}, {"componentGeneration", 1U}, {"worldGeneration", physics.WorldHandle().worldGeneration}}}
			};

			const AutomationDispatchResult blocked = service.GetRegistry().Dispatch("physics.body.apply_impulse", 1, request);
			context.Expect(!blocked.ok && blocked.error == ErrorCategory::Conflict,
				"A paused command without explicit Next-step consent must be rejected before object lookup or queue mutation.");

			request.arguments["applyOnNextStep"] = true;
			const AutomationDispatchResult admittedPastPauseGate = service.GetRegistry().Dispatch("physics.body.apply_impulse", 1, request);
			context.Expect(!admittedPastPauseGate.ok && admittedPastPauseGate.error == ErrorCategory::MissingTarget,
				"Explicit Next-step consent should pass the pause gate, then reject the intentionally stale target without enqueuing a command.");

			context.Expect(physics.StopWorld(), "The paused-command fixture must drain the provider world before test teardown.");
			physics.Shutdown();
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

		registry.Add("Editor.MCP.Physics.ForceApplicationModeBuildsTypedForceOrAccelerationCommands", [](TestContext& context) {
			const TerraEngine::Physics::PhysicsBodyCommandTarget target{
				{TerraEngine::Physics::kPrimaryPhysicsWorldId, 7U}, TerraEngine::GenerateUUID(), 3U, 5U};
			TerraEngine::Physics::PhysicsCommandMetadata metadata;
			metadata.producerId = 99U;
			metadata.producerSequence = 1U;

			const auto force = PhysicsAutomationProvider::TryCreateApplyForceCommand("Force", target, {1.0F, 2.0F, 3.0F}, metadata);
			const auto acceleration = PhysicsAutomationProvider::TryCreateApplyForceCommand("Acceleration", target, {4.0F, 5.0F, 6.0F}, metadata);
			const auto invalid = PhysicsAutomationProvider::TryCreateApplyForceCommand("Impulse", target, {0.0F, 0.0F, 0.0F}, metadata);
			const auto forcePayload = force ? std::get_if<TerraEngine::Physics::PhysicsForcePayload>(&force->payload) : nullptr;
			const auto accelerationPayload = acceleration ? std::get_if<TerraEngine::Physics::PhysicsForcePayload>(&acceleration->payload) : nullptr;
			context.Expect(force && force->type == TerraEngine::Physics::PhysicsCommandType::ApplyForce
				&& forcePayload != nullptr && forcePayload->mode == TerraEngine::Physics::PhysicsForceMode::Force,
				"A missing applicationMode defaults to the typed ordinary force command.");
			context.Expect(acceleration && acceleration->type == TerraEngine::Physics::PhysicsCommandType::ApplyAcceleration
				&& accelerationPayload != nullptr && accelerationPayload->mode == TerraEngine::Physics::PhysicsForceMode::Acceleration,
				"Acceleration must route to the mass-independent typed acceleration command.");
			context.Expect(!invalid.has_value(), "Unsupported force application modes must be rejected before queue admission.");
		});

		registry.Add("Editor.MCP.Physics.RetainedContactsSurviveAQuietLatestPacket", [](TestContext& context) {
			TerraEngine::Physics::PhysicsWorld world;
			context.Expect(world.Create(TerraEngine::Physics::PhysicsSettings::Defaults()), "The retained-contact fixture must create an isolated physics world.");
			const TerraEngine::Physics::PhysicsEvent earlierContact = MakeRetainedContact(world, 1U);
			const auto firstPacket = world.PublishEvents(1U, {&earlierContact, 1U});
			const auto quietLatestPacket = world.PublishEvents(2U, {});
			context.Expect(firstPacket && firstPacket->events && firstPacket->events->empty(),
				"With zero gameplay subscribers, the gameplay event packet must remain empty.");
			context.Expect(quietLatestPacket && quietLatestPacket->events && quietLatestPacket->events->empty(),
				"The latest packet intentionally contains no contact events.");

			const auto retainedEarlierPacket = world.AcquireDiagnosticEventPacket(1U, 2U);
			context.Expect(retainedEarlierPacket && retainedEarlierPacket->events && retainedEarlierPacket->events->size() == 1U
				&& retainedEarlierPacket->events->front().type == TerraEngine::Physics::PhysicsEventType::Contact,
				"A bounded requested history range must retain the earlier contact even when the latest packet is quiet.");
			context.Expect(world.BeginStopping(), "The retained-contact fixture must stop new work before teardown.");
			(void)world.BeginDraining();
			context.Expect(world.CompleteTeardown(), "The retained-contact fixture must release its immutable history cleanly.");
		});
	}
}
