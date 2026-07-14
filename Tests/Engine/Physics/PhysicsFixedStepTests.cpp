// Fixed-step policy tests. These exercise the engine-owned accumulator and
// paused-token contract without depending on a particular physics provider.
#include "../TestHarness.h"

#include "Physics/PhysicsWorld.h"
#include "Physics/PhysicsSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <cmath>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	namespace {
		// Keep the shared pool balanced even when an assertion throws, because the
		// Jolt-backed system test below must not leak worker state into later tests.
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
	}

	void RegisterPhysicsFixedStepTests(TestRegistry& registry)
	{
		registry.Add("Physics.FixedStep.ClampsAccumulatorAndReportsDroppedTime", [](TestContext& context) {
			PhysicsWorld world;
			PhysicsSettings settings = PhysicsSettings::Defaults();
			settings.maxSubsteps = 2;
			settings.maxAccumulatedTime = settings.fixedTimeStep * 3.0F;
			context.Expect(world.Create(settings), "World should accept valid fixed-step settings.");
			const auto plan = world.PlanFixedSteps(1.0F);
			context.Expect(plan.clampedTime > 0.0F, "Excess elapsed time must be reported as clamped.");
			context.Expect(plan.droppedTime >= 0.0F, "Dropped time must be observable and nonnegative.");
			context.Expect(plan.requests.size() <= settings.maxSubsteps, "Catch-up must respect maxSubsteps.");
			context.Expect(plan.remainingAccumulator <= settings.maxAccumulatedTime + 1.0e-6F, "Accumulator must remain bounded.");
		});

		registry.Add("Physics.FixedStep.PausedNextConsumesExactlyOneExplicitToken", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "World should create for pause testing.");
			context.Expect(world.SetPaused(true), "World should enter paused state.");
			for (int index = 0; index < 100; ++index) {
				context.Expect(world.RequestExplicitPausedStep(), "Each paused Next must accept one token after the prior token is consumed.");
				const auto plan = world.PlanFixedSteps(0.0F);
				context.Expect(plan.consumedExplicitPausedStep && plan.requests.size() == 1U, "One Next token must produce exactly one fixed request.");
				context.Expect(plan.requests.front().explicitPausedStep, "Paused requests must be marked explicit.");
				context.Expect(std::fabs(plan.requests.front().deltaTime - world.Settings().fixedTimeStep) < 1.0e-6F, "Next uses the configured fixed timestep.");
				context.Expect(world.MarkStepCompleted(plan.requests.front()), "Completed explicit step must advance publication timing.");
				context.Expect(world.IsPaused(), "Completing Next must not resume the world.");
			}
			context.ExpectEqual(static_cast<PhysicsTick>(100), world.Timing().completedTick, "Exactly 100 explicit ticks must complete.");
		});

		registry.Add("Physics.FixedStep.PausedNextIgnoresResidualAndWallClockTime", [](TestContext& context) {
			PhysicsWorld world;
			PhysicsSettings settings = PhysicsSettings::Defaults();
			settings.maxSubsteps = 1U;
			context.Expect(world.Create(settings), "World should create for paused-step isolation testing.");

			// Leave ordinary accumulator time behind. Next must not consume it or
			// turn it into an additional simulation request while the world is paused.
			const auto ordinaryPlan = world.PlanFixedSteps(settings.fixedTimeStep * 3.0F);
			context.ExpectEqual(static_cast<std::size_t>(1), ordinaryPlan.requests.size(), "The bounded ordinary plan should issue one substep.");
			context.Expect(world.MarkStepCompleted(ordinaryPlan.requests.front()),
				"The ordinary fixture step must complete before the following explicit tick can publish in sequence.");
			context.Expect(world.SetPaused(true), "World should pause after ordinary accumulation is established.");
			const float residualBeforeNext = world.Timing().accumulator;
			const float requestedDuration = world.Settings().fixedTimeStep;

			context.Expect(world.RequestExplicitPausedStep(), "Paused Next should accept one explicit token.");
			context.Expect(!world.RequestExplicitPausedStep(), "A second Next must not queue behind an unconsumed explicit token.");
			const auto nextPlan = world.PlanFixedSteps(123.0F);

			context.Expect(nextPlan.consumedExplicitPausedStep, "Paused planning must consume the requested token.");
			context.ExpectEqual(static_cast<std::size_t>(1), nextPlan.requests.size(), "Wall-clock delta must not add extra paused ticks.");
			context.Expect(nextPlan.requests.front().explicitPausedStep, "The only paused request must be explicitly marked.");
			context.Expect(std::fabs(nextPlan.requests.front().deltaTime - requestedDuration) < 1.0e-6F,
				"Paused Next must retain the configured fixed duration instead of using wall-clock time.");
			context.Expect(std::fabs(nextPlan.remainingAccumulator - residualBeforeNext) < 1.0e-6F,
				"Paused Next must preserve residual ordinary accumulator time.");
			context.Expect(world.MarkStepCompleted(nextPlan.requests.front()), "The explicit request should complete once publication succeeds.");
			context.Expect(world.IsPaused(), "The completed Next request must leave the world paused.");
		});

		registry.Add("Physics.FixedStep.SystemPublishesOnePausedStepPerToken", [](TestContext& context) {
			// PhysicsSystem uses the engine-owned pool through Jolt's job adapter;
			// this bounded test owns a fresh session so it can exercise the real
			// publication barrier rather than only PhysicsWorld planning state.
			ThreadPoolSession pool;
			auto& physics = PhysicsSystem::Instance();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "A lifecycle-controlled physics world must start before Next is accepted.");
			context.Expect(physics.SetPaused(true), "The live physics world must enter paused state.");
			const std::uint64_t publicationsBefore = physics.ExplicitStepPublicationCount();
			context.Expect(physics.RequestExplicitStep(), "The live physics system must accept one paused Next token.");
			physics.Update(42.0F);
			physics.OnAfterPhysicsFlush({});
			context.ExpectEqual(publicationsBefore + 1U, physics.ExplicitStepPublicationCount(),
				"One paused token must reach the publication barrier exactly once despite a large frame delta.");
			context.Expect(physics.IsPaused(), "The live system must remain paused after publishing one explicit tick.");
			context.Expect(physics.StopWorld(), "The bounded live world must tear down cleanly after the test.");
			physics.Shutdown();
		});
	}
}
