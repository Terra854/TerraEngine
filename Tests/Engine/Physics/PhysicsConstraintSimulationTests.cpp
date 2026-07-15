// Direct Jolt-backend constraint regressions. Registration is intentionally
// separate so the orchestrator can wire these focused tests in its shared CMake slice.
#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		class ConstraintFixture final {
		public:
			ConstraintFixture() : backend(ThreadPool::instance()) {
				ThreadPool::instance().shutdown(); ThreadPool::instance().initialize();
				backend.Initialize(); PhysicsBackendWorldConfiguration configuration; configuration.world = world; backend.CreateWorld(configuration);
				PhysicsBackendStagingToken staging; PhysicsShapeRecipe recipe; recipe.kind = PhysicsShapeKind::Box; recipe.dimensions = {1.0F, 1.0F, 1.0F};
				backend.CreateShape(world, shape, recipe, staging);
				CreateBody(staging, bodyA, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F});
				CreateBody(staging, bodyB, PhysicsMotionType::Dynamic, {0.0F, -2.0F, 0.0F});
				CreateBody(staging, bodyC, PhysicsMotionType::Dynamic, {2.0F, -2.0F, 0.0F});
				backend.CommitStaging(staging);
			}

			~ConstraintFixture() { backend.DestroyWorld(world); backend.Shutdown(); ThreadPool::instance().shutdown(); }

			PhysicsConstraintCreateRequest Request(PhysicsConstraintHandle handle, PhysicsConstraintKind kind) const {
				PhysicsConstraintCreateRequest request; request.handle = handle; request.kind = kind; request.bodyA = bodyA; request.bodyB = bodyB;
				request.ownerObjectId = TerraEngine::GenerateUUID(); request.ownerObjectGeneration = 1U; request.componentGeneration = 1U;
				request.recordId = TerraEngine::GenerateUUID(); request.recordGeneration = 1U;
				request.axisA = {0.0F, 1.0F, 0.0F}; request.axisB = request.axisA; request.normalAxisA = {1.0F, 0.0F, 0.0F}; request.normalAxisB = request.normalAxisA;
				return request;
			}

			void CreateBody(PhysicsBackendStagingToken& staging, PhysicsBodyHandle handle, PhysicsMotionType type, PhysicsVector3 position) {
				PhysicsBodyCreateRequest request; request.handle = handle; request.objectId = TerraEngine::GenerateUUID(); request.objectGeneration = 1U; request.componentGeneration = 1U;
				request.shape = shape; request.motionType = type; request.pose.position = position; backend.CreateBody(request, staging);
			}

			JoltPhysicsBackend backend;
			PhysicsWorldHandle world{{41U}, 1U};
			PhysicsShapeHandle shape{{41U}, 1U, 0U, 1U};
			PhysicsBodyHandle bodyA{{41U}, 1U, 0U, 1U};
			PhysicsBodyHandle bodyB{{41U}, 1U, 1U, 1U};
			PhysicsBodyHandle bodyC{{41U}, 1U, 2U, 1U};
		};
	}

	void RegisterPhysicsConstraintSimulationTests(TestRegistry& registry) {
		registry.Add("Physics.ConstraintSimulation.AllFamiliesStageExtractAndRollback", [](TestContext& context) {
			ConstraintFixture fixture;
			const std::array<PhysicsConstraintKind, 8U> families{
				PhysicsConstraintKind::Fixed, PhysicsConstraintKind::Point, PhysicsConstraintKind::Distance, PhysicsConstraintKind::Hinge,
				PhysicsConstraintKind::Slider, PhysicsConstraintKind::Cone, PhysicsConstraintKind::SwingTwist, PhysicsConstraintKind::SixDegreesOfFreedom};
			PhysicsBackendStagingToken staging;
			for(std::size_t index = 0; index < families.size(); ++index) {
				PhysicsConstraintCreateRequest request = fixture.Request({{41U}, 1U, static_cast<PhysicsSlotIndex>(index), 1U}, families[index]);
				request.axes[0].limitEnabled = true; request.axes[0].minimum = -0.25F; request.axes[0].maximum = 0.25F;
				request.axes[0].springEnabled = true; request.axes[0].springUsesFrequency = true; request.axes[0].springFrequency = 4.0F; request.axes[0].springDamping = 0.8F;
				request.axes[0].motorMode = PhysicsBackendMotorMode::PositionAndVelocity; request.axes[0].motorMaximumForce = 100.0F;
				for(std::size_t axis = 0; axis < request.axes.size(); ++axis) { request.axes[axis].limitEnabled = true; request.axes[axis].minimum = -0.2F; request.axes[axis].maximum = 0.2F; }
				context.Expect(fixture.backend.CreateConstraint(request, staging).Succeeded(), "Every supported Jolt constraint family must stage.");
			}
			context.Expect(fixture.backend.CommitStaging(staging).Succeeded(), "A complete constraint batch must publish atomically.");
			context.Expect(fixture.backend.Step({fixture.world, 1U, 1.0F / 60.0F, 1U}).Succeeded(), "Constraint fixture must step.");
			PhysicsBackendExtraction extraction; extraction.world = fixture.world; fixture.backend.Extract(extraction);
			context.Expect(extraction.constraints.size() == families.size(), "Every committed constraint must publish a provider-neutral state.");
			for(const PhysicsBackendConstraintState& state : extraction.constraints) {
				context.Expect(std::isfinite(state.linearImpulseMagnitude) && std::isfinite(state.angularImpulseMagnitude), "Jolt solver lambdas must extract as finite impulses.");
				context.Expect(state.handle.IsValid() && state.ownerObjectId != TerraEngine::EMPTY_UUID
					&& state.recordId != TerraEngine::EMPTY_UUID && state.ownerObjectGeneration == 1U
					&& state.componentGeneration == 1U && state.recordGeneration == 1U,
					"Constraint extraction must preserve complete generation-bearing ownership metadata instead of provider-local references.");
			}

			PhysicsBackendStagingToken rollback; PhysicsConstraintCreateRequest stale = fixture.Request({{41U}, 1U, 20U, 1U}, PhysicsConstraintKind::Fixed);
			context.Expect(fixture.backend.CreateConstraint(stale, rollback).Succeeded() && fixture.backend.RollbackStaging(rollback).Succeeded(), "A staged constraint must roll back without live solver state.");
			context.Expect(!fixture.backend.DestroyConstraint({{41U}, 1U, 20U, 2U}).Succeeded(), "A stale generation must never destroy a reused constraint slot.");
		});

		registry.Add("Physics.ConstraintSimulation.GearUsesHingeReferences", [](TestContext& context) {
			ConstraintFixture fixture; PhysicsBackendStagingToken staging;
			PhysicsConstraintCreateRequest first = fixture.Request({{41U}, 1U, 30U, 1U}, PhysicsConstraintKind::Hinge);
			PhysicsConstraintCreateRequest second = fixture.Request({{41U}, 1U, 31U, 1U}, PhysicsConstraintKind::Hinge);
			// Jolt gears require two rotating bodies. Attach each hinge to the provider
			// world endpoint, then use the two dynamic hinge bodies for the gear itself.
			first.bodyA = {}; first.bodyB = fixture.bodyB;
			second.bodyA = {}; second.bodyB = fixture.bodyC;
			context.Expect(fixture.backend.CreateConstraint(first, staging).Succeeded() && fixture.backend.CreateConstraint(second, staging).Succeeded(), "Referenced hinges must stage before their gear.");
			PhysicsConstraintCreateRequest gear = fixture.Request({{41U}, 1U, 32U, 1U}, PhysicsConstraintKind::Gear); gear.bodyA = fixture.bodyB; gear.bodyB = fixture.bodyC; gear.gearConstraintA = first.handle; gear.gearConstraintB = second.handle; gear.gearRatio = 2.0F;
			context.Expect(fixture.backend.CreateConstraint(gear, staging).Succeeded() && fixture.backend.CommitStaging(staging).Succeeded(), "Jolt gear support must accept two staged hinge references.");
			fixture.backend.Step({fixture.world, 1U, 1.0F / 60.0F, 1U}); PhysicsBackendExtraction extraction; extraction.world = fixture.world; fixture.backend.Extract(extraction);
			const auto found = std::find_if(extraction.constraints.begin(), extraction.constraints.end(), [&](const auto& state) { return state.handle == gear.handle; });
			context.Expect(found != extraction.constraints.end() && std::isfinite(found->angularImpulseMagnitude), "Gear extraction must expose its finite total angular lambda.");

			PhysicsConstraintCreateRequest unrelated = fixture.Request({{41U}, 1U, 33U, 1U}, PhysicsConstraintKind::Hinge);
			PhysicsBackendStagingToken unrelatedStaging;
			context.Expect(fixture.backend.CreateConstraint(unrelated, unrelatedStaging).Succeeded()
				&& fixture.backend.CommitStaging(unrelatedStaging).Succeeded(), "An unrelated hinge must publish while a gear is live.");
			context.Expect(fixture.backend.UpdateConstraint(unrelated).Succeeded(), "A gear must not block updates to an unrelated hinge.");
			context.Expect(!fixture.backend.UpdateConstraint(first).Succeeded()
				&& !fixture.backend.DestroyConstraint(first.handle).Succeeded(), "A gear must protect only the exact hinges it references.");
			context.Expect(fixture.backend.DestroyConstraint(unrelated.handle).Succeeded(), "An unrelated hinge must remain independently destroyable.");
			context.Expect(fixture.backend.DestroyConstraint(gear.handle).Succeeded()
				&& fixture.backend.DestroyConstraint(first.handle).Succeeded()
				&& fixture.backend.DestroyConstraint(second.handle).Succeeded(), "Gear-first teardown must release both dependent hinges safely.");
		});
	}
}
