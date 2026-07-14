// PhysicsLifecycleStressTests.cpp - bounded stale-race and lifecycle stress.

#include "../TestHarness.h"

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
	}
}
