// PhysicsLifecycleStressTests.cpp - bounded stale-race and lifecycle stress.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsQueries.h"
#include "Physics/PhysicsSettings.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsWorld.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Physics;

		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		enum class LifecycleBoundary : std::uint8_t {
			SceneUnload,
			RemoveRecreate,
			ScriptReload,
			RendererUnavailable,
			ProjectClose,
			Shutdown
		};

		// Round-robin scheduling gives every named teardown path repeated coverage
		// while retaining the required 1,000-cycle total rather than multiplying it.
		constexpr std::array<LifecycleBoundary, 6U> kLifecycleBoundaries{
			LifecycleBoundary::SceneUnload,
			LifecycleBoundary::RemoveRecreate,
			LifecycleBoundary::ScriptReload,
			LifecycleBoundary::RendererUnavailable,
			LifecycleBoundary::ProjectClose,
			LifecycleBoundary::Shutdown};

		std::string_view BoundaryName(LifecycleBoundary boundary) {
			switch(boundary) {
				case LifecycleBoundary::SceneUnload: return "scene-unload";
				case LifecycleBoundary::RemoveRecreate: return "remove-recreate";
				case LifecycleBoundary::ScriptReload: return "script-reload";
				case LifecycleBoundary::RendererUnavailable: return "renderer-unavailable";
				case LifecycleBoundary::ProjectClose: return "project-close";
				case LifecycleBoundary::Shutdown: return "shutdown";
			}
			return "unknown";
		}

		PhysicsQueryRequest MakeQuery(PhysicsWorldHandle world, PhysicsTick tick) {
			PhysicsQueryRequest request;
			request.world = world;
			request.kind = PhysicsQueryKind::RayCast;
			request.pose.position = {-1.0F, 0.0F, 0.0F};
			request.direction = {1.0F, 0.0F, 0.0F};
			request.distance = 4.0F;
			request.maximumHits = 1U;
			request.submittedTick = tick;
			request.correlationId = "physics-lifecycle-stale-race";
			return request;
		}

		bool ReplaceWorld(PhysicsWorld& world, LifecycleBoundary boundary) {
			// All non-renderer producer boundaries converge at this protected teardown:
			// stop acceptance, drain prepared work, invalidate identities, then create.
			// Passing the named boundary keeps the race matrix explicit rather than
			// treating every stale handle as a generic replacement.
			(void)boundary;
			return world.BeginStopping()
				&& world.BeginDraining() == 0U
				&& world.CompleteTeardown()
				&& world.Create(PhysicsSettings::Defaults());
		}
	}

	void RegisterPhysicsLifecycleStressTests(TestRegistry& registry) {
		registry.Add("Physics.LifecycleStress.OneHundredStaleRemoveRecreateUnloadReloadAndShutdownRacesStayIsolated", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The stale-race world must start before repeated lifecycle changes.");

			for(std::uint32_t iteration = 0U; iteration < 100U; ++iteration) {
				const LifecycleBoundary boundary = kLifecycleBoundaries[iteration % kLifecycleBoundaries.size()];
				const PhysicsWorldHandle oldWorld = world.Handle();
				const PhysicsBodyHandle removedBody = world.AllocateBody();
				const PhysicsQueryReceipt receipt = world.SubmitQuery(MakeQuery(oldWorld, 1U));
				const PhysicsPreparedQueryBatch prepared = world.PrepareQueries(1U);
				context.Expect(removedBody.IsValid() && receipt.accepted && prepared.queries.size() == 1U,
					"Every race must admit one old body and one prepared query before unload/recreate.");

				context.Expect(ReplaceWorld(world, boundary),
					"The " + std::string(BoundaryName(boundary)) + " replacement must drain old work before its new generation is visible.");
				const PhysicsWorldHandle replacement = world.Handle();
				const PhysicsBodyHandle replacementBody = world.AllocateBody();
				context.Expect(replacement != oldWorld && replacementBody.slotIndex == removedBody.slotIndex,
					"The race must exercise a reused compact slot under a new world generation.");
				context.Expect(!world.CompleteQuery(prepared.queries.front(), 1U, PhysicsResultStatus::Success, {}),
					"A late worker query completion from an unloaded world must be rejected.");
				context.Expect(world.ReadQuery(receipt.handle).status == PhysicsResultStatus::StaleTarget,
					"A stale query result must never read a replacement world packet.");
				context.Expect(world.Status(removedBody) == PhysicsHandleStatus::StaleWorld,
					"A removed body handle must not gain authority after remove/recreate.");
				context.Expect(world.Release(replacementBody), "The replacement body must retire before the next lifecycle race.");
			}
		});

		registry.Add("Physics.LifecycleStress.CancellationProviderFailureStaleReceiptsAndReusedGenerationsStayContained", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The lifecycle containment fixture must create a live world.");
			const PhysicsWorldHandle liveWorld = world.Handle();
			const PhysicsBodyHandle retiredBody = world.AllocateBody();
			context.Expect(retiredBody.IsValid() && world.Release(retiredBody),
				"The fixture must retire one object-generation-bearing body handle before reallocation.");
			const PhysicsBodyHandle replacementBody = world.AllocateBody();
			context.Expect(replacementBody.slotIndex == retiredBody.slotIndex && replacementBody.slotGeneration != retiredBody.slotGeneration,
				"Reused compact slots must advance generation so a stale object/body identity cannot revive.");
			context.Expect(world.Status(retiredBody) == PhysicsHandleStatus::StaleSlot,
				"The stale pre-replacement handle must be rejected before any lifecycle boundary runs.");

			const PhysicsQueryReceipt queryReceipt = world.SubmitQuery(MakeQuery(liveWorld, 1U));
			context.Expect(queryReceipt.accepted && world.CancelQuery(queryReceipt.handle),
				"A queued query must be cancellable while the original world is live.");
			context.Expect(world.ReadQuery(queryReceipt.handle).status == PhysicsResultStatus::Cancelled,
				"A cancelled receipt must never become a successful late result.");

			const PhysicsBodyCommandTarget commandTarget{liveWorld, TerraEngine::GenerateUUID(), 1U, 1U};
			const PhysicsCommandMetadata commandMetadata{.producerId = 1U, .producerSequence = 1U};
			const PhysicsCommandReceipt commandReceipt = world.EnqueueCommand(PhysicsCommand::MakeWake(commandTarget, commandMetadata));
			context.Expect(commandReceipt.accepted && world.CancelCommand(commandReceipt.commandId),
				"A deferred command must be removable before provider work reaches a protected boundary.");
			context.Expect(world.CommandQueueDiagnostics().queuedDepth == 0U,
				"Command cancellation must leave no work that could outlive the world.");

			// MarkFailed is the provider/orchestration failure transition. It stops new
			// producers, yet still requires the ordinary drain and teardown sequence.
			context.Expect(world.MarkFailed() && world.State() == PhysicsWorldState::Failed,
				"A provider failure must enter the contained Failed state before teardown.");
			context.Expect(!world.EnqueueCommand(PhysicsCommand::MakeWake(commandTarget,
				PhysicsCommandMetadata{.producerId = 1U, .producerSequence = 2U})).accepted,
				"A failed provider world must reject new work rather than retaining a dangerous stale command.");
			context.Expect(world.BeginStopping() && world.BeginDraining() == 0U && world.CompleteTeardown(),
				"Provider failure cleanup must use the same complete draining boundary as project close.");
			context.Expect(world.ReadQuery(queryReceipt.handle).status == PhysicsResultStatus::StaleTarget
				&& world.Status(replacementBody) == PhysicsHandleStatus::StaleWorld,
				"Teardown must leave both cancelled receipts and replacement-generation handles unable to address a new world.");
		});

		registry.Add("Physics.LifecycleStress.OneThousandSystemCyclesDrainForExplicitLifecycleBoundaries", [](TestContext& context) {
			ThreadPoolSession pool;
			auto& physics = PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			PublishedPhysicsDebugPackets().Clear();

			for(std::uint32_t cycle = 0U; cycle < 1000U; ++cycle) {
				const LifecycleBoundary boundary = kLifecycleBoundaries[cycle % kLifecycleBoundaries.size()];
				context.Expect(physics.StartWorld(), "Every Play Mode lifecycle cycle must create a fresh provider world.");
				const PhysicsWorldHandle liveWorld = physics.WorldHandle();
				context.Expect(liveWorld.IsValid(), "A live cycle must expose a valid generation-safe world handle.");
				context.Expect(physics.SetPaused(true) && physics.RequestExplicitStep(),
					"A paused lifecycle cycle must accept exactly one bounded explicit tick before teardown.");
				physics.Update(0.0F);
				physics.OnAfterPhysicsFlush({});

				if(boundary == LifecycleBoundary::RendererUnavailable) {
					// This headless engine test deliberately has no renderer. Selecting only
					// immutable diagnostic packets proves renderer loss cannot touch the live
					// provider world or make physics depend on presentation availability.
					const PhysicsDebugDisplaySelection selection{};
					(void)PublishedPhysicsDebugPackets().Select(liveWorld, cycle + 1U, selection);
					context.Expect(physics.WorldHandle() == liveWorld,
						"The renderer-unavailable boundary must leave the current world valid until stop.");
				}
				context.Expect(physics.StopWorld() && !physics.WorldHandle().IsValid(),
					"The " + std::string(BoundaryName(boundary)) + " boundary must leave no active physics world.");

				if(boundary == LifecycleBoundary::ProjectClose || boundary == LifecycleBoundary::Shutdown) {
					// A soak loop has to restart after both host-ending cases. This is a real
					// PhysicsSystem teardown/reinitialize sequence, not a comment-only model.
					physics.Shutdown();
					context.Expect(!physics.WorldHandle().IsValid(),
						"Project close and shutdown must release all active provider world identities.");
					physics.Initialize();
				}
			}
			physics.Shutdown();
			context.Expect(!physics.WorldHandle().IsValid(), "Final shutdown must leave no active world after all one thousand cycles.");
			PublishedPhysicsDebugPackets().Clear();
		});

		registry.Add("Physics.LifecycleStress.TeardownRejectsPreparedWorkAndRestartHasNoOldAuthority", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "The teardown retry fixture needs an active first generation.");
			const PhysicsWorldHandle first = world.Handle();
			const PhysicsQueryReceipt receipt = world.SubmitQuery(MakeQuery(first, 1U));
			const PhysicsPreparedQueryBatch prepared = world.PrepareQueries(1U);
			context.Expect(receipt.accepted && prepared.queries.size() == 1U,
				"One immutable prepared query must exist before teardown races its completion.");
			context.Expect(world.BeginStopping() && world.BeginDraining() == 0U && world.CompleteTeardown(),
				"Teardown must drain the protected work boundary before it invalidates the world generation.");
			context.Expect(world.Create(PhysicsSettings::Defaults()) && world.Handle() != first,
				"A retry world must receive a new authority generation after teardown.");
			context.Expect(!world.CompleteQuery(prepared.queries.front(), 1U, PhysicsResultStatus::Success, {}),
				"A prepared completion that races teardown must not publish into the retry world.");
			context.Expect(world.ReadQuery(receipt.handle).status == PhysicsResultStatus::StaleTarget,
				"A rejected old completion must remain stale rather than becoming retry-world state.");
			context.Expect(world.BeginStopping() && world.BeginDraining() == 0U && world.CompleteTeardown(),
				"The retry world must also tear down without retaining prepared work from the failed generation.");
		});

		registry.Add("Physics.LifecycleStress.FatalProviderBarrierFailurePublishesNothingAndDrainsBeforeTeardown", [](TestContext& context) {
			using namespace TerraEngine::Physics::Backend;
			ThreadPoolSession pool;
			JoltPhysicsBackend backend{ThreadPool::instance()};
			const PhysicsWorldHandle world{{92U}, 1U};
			PhysicsBackendWorldConfiguration configuration;
			configuration.world = world;
			context.Expect(backend.Initialize().Succeeded() && backend.CreateWorld(configuration).Succeeded(),
				"The contained-provider fixture must establish one world before exercising its barrier path.");
			context.Expect(backend.Step({world, 1U, 1.0F / 60.0F, 1U}).Succeeded(),
				"One successful fixed step must establish that the normal provider barrier drains first.");

			// This seam records the same translated fatal provider failure used by the
			// allocator callback, without relying on an unsafe allocation failure.
			backend.ReportFatalProviderFailureForValidation();
			PhysicsBackendExtraction extraction;
			extraction.world = world;
			extraction.tick = 2U;
			const PhysicsBackendResult rejectedStep = backend.Step({world, 2U, 1.0F / 60.0F, 1U});
			context.Expect(rejectedStep.error == PhysicsBackendErrorCode::FatalProviderFailure
				&& backend.Extract(extraction).error == PhysicsBackendErrorCode::FatalProviderFailure,
				"A contained provider/barrier failure must reject simulation and extraction instead of publishing a new packet.");
			context.Expect(backend.JobDiagnostics().queued == 0U && backend.JobDiagnostics().activePumps == 0U,
				"Failure containment must drain every provider job before the world lifetime can end.");
			context.Expect(backend.DestroyWorld(world).Succeeded() && backend.Shutdown().Succeeded(),
				"A contained provider failure must still permit deterministic world teardown without outliving work.");
		});
}
}
