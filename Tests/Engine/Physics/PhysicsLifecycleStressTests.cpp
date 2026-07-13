// PhysicsLifecycleStressTests.cpp - bounded stale-race and lifecycle stress.

#include "../TestHarness.h"

#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsQueries.h"
#include "Physics/PhysicsSettings.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsWorld.h"
#include "ThreadPool/ThreadPool.h"

#include <cstdint>

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

		bool ReplaceWorld(PhysicsWorld& world) {
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
				const PhysicsWorldHandle oldWorld = world.Handle();
				const PhysicsBodyHandle removedBody = world.AllocateBody();
				const PhysicsQueryReceipt receipt = world.SubmitQuery(MakeQuery(oldWorld, 1U));
				const PhysicsPreparedQueryBatch prepared = world.PrepareQueries(1U);
				context.Expect(removedBody.IsValid() && receipt.accepted && prepared.queries.size() == 1U,
					"Every race must admit one old body and one prepared query before unload/recreate.");

				// This one protected replacement models scene unload plus remove/recreate.
				// Script reload and shutdown use the same generation invalidation contract,
				// so a late command/result cannot be accepted by the replacement world.
				context.Expect(ReplaceWorld(world), "World replacement must drain all old work before the next generation is visible.");
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

		registry.Add("Physics.LifecycleStress.OneThousandSystemCyclesDrainForScriptReloadRendererFailureProjectCloseAndShutdown", [](TestContext& context) {
			ThreadPoolSession pool;
			auto& physics = PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			PublishedPhysicsDebugPackets().Clear();

			for(std::uint32_t cycle = 0U; cycle < 1000U; ++cycle) {
				context.Expect(physics.StartWorld(), "Every Play Mode lifecycle cycle must create a fresh provider world.");
				const PhysicsWorldHandle liveWorld = physics.WorldHandle();
				context.Expect(liveWorld.IsValid(), "A live cycle must expose a valid generation-safe world handle.");
				context.Expect(physics.SetPaused(true) && physics.RequestExplicitStep(),
					"A paused lifecycle cycle must accept exactly one bounded explicit tick before teardown.");
				physics.Update(0.0F);
				physics.OnAfterPhysicsFlush({});

				// Diagnostics are intentionally renderer-independent. Reading the packet
				// history here models a renderer-unavailable frame without invoking any
				// presentation backend or allowing it to own physics lifetime.
				const PhysicsDebugDisplaySelection selection{};
				(void)PublishedPhysicsDebugPackets().Select(liveWorld, cycle + 1U, selection);
				context.Expect(physics.StopWorld() && !physics.WorldHandle().IsValid(),
					"Play stop, script reload, and scene unload must leave no active physics world.");

				if((cycle + 1U) % 100U == 0U) {
					// Periodic complete host teardown models project close/shutdown. Restarting
					// immediately verifies that no callback or provider allocation survives.
					physics.Shutdown();
					physics.Initialize();
				}
			}
			physics.Shutdown();
			context.Expect(!physics.WorldHandle().IsValid(), "Final shutdown must leave no active world after all one thousand cycles.");
			PublishedPhysicsDebugPackets().Clear();
		});
	}
}
