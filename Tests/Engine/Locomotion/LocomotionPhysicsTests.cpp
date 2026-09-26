// LocomotionPhysicsTests.cpp - fixed-step character movement contract tests.
//
// A provider fixture keeps render submissions separate from physical ticks so
// the tests can distinguish copied intent from actual collision-limited motion.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		constexpr float kFixedDeltaTime = 1.0F / 60.0F;

		struct VelocityFixture final {
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{73U}, 1U};
			PhysicsShapeHandle floorShape{{73U}, 1U, 90U, 1U};
			PhysicsBodyHandle floorBody{{73U}, 1U, 90U, 1U};

			VelocityFixture() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				if(!backend.Initialize().Succeeded()) throw std::runtime_error("Velocity provider initialization failed.");
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.frame = {kPrimaryPhysicsFrameId, 1U};
				if(!backend.CreateWorld(configuration).Succeeded()) throw std::runtime_error("Velocity world creation failed.");
				PhysicsBackendStagingToken staging;
				PhysicsShapeRecipe floorRecipe;
				floorRecipe.kind = PhysicsShapeKind::Box;
				floorRecipe.dimensions = {40.0F, 1.0F, 40.0F};
				PhysicsBodyCreateRequest floor;
				floor.handle = floorBody;
				floor.objectId = TerraEngine::GenerateUUID();
				floor.objectGeneration = 1U;
				floor.componentGeneration = 1U;
				floor.shape = floorShape;
				floor.motionType = PhysicsMotionType::Static;
				floor.pose.position.y = -0.5F;
				if(!backend.CreateShape(world, floorShape, floorRecipe, staging).Succeeded()
					|| !backend.CreateBody(floor, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("Velocity fixture floor creation failed.");
			}

			~VelocityFixture() {
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
				ThreadPool::instance().shutdown();
			}

			PhysicsCharacterCommandTarget CreateCharacter(
				PhysicsCharacterRotationPolicy rotationPolicy = PhysicsCharacterRotationPolicy::AlignToUp,
				PhysicsVector3 localCenter = {}, PhysicsVector3 up = {0.0F, 1.0F, 0.0F},
				bool gravity = false, float initialX = 0.0F) {
				PhysicsCharacterCommandTarget controller;
				controller.world = world;
				controller.character = {{73U}, 1U, 8U, 1U};
				controller.objectId = TerraEngine::GenerateUUID();
				controller.objectGeneration = 1U;
				controller.componentGeneration = 1U;
				PhysicsCharacterCreateRequest request;
				request.handle = controller.character;
				request.objectId = controller.objectId;
				request.objectGeneration = controller.objectGeneration;
				request.componentGeneration = controller.componentGeneration;
				request.pose.position = {initialX, gravity ? 4.0F : 1.1F, 0.0F};
				request.shapeKind = PhysicsShapeKind::Capsule;
				request.radius = 0.5F;
				request.height = 2.0F;
				request.localCenter = localCenter;
				request.upDirection = up;
				request.rotationPolicy = rotationPolicy;
				request.useGravity = gravity;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateCharacter(request, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("Velocity fixture character creation failed.");
				return controller;
			}

			CharacterMotionLease Acquire(const PhysicsCharacterCommandTarget& controller) {
				CharacterMotionLease unleased;
				unleased.controller = controller;
				CharacterMotionLease owned = unleased;
				owned.owner.sceneLifetime = 1U;
				owned.owner.objectId = controller.objectId;
				owned.owner.objectGeneration = controller.objectGeneration;
				owned.owner.attachmentGeneration = 1U;
				owned.owner.slotIndex = 1U;
				owned.owner.slotGeneration = 1U;
				owned.authorityEpoch = 1U;
				owned.motionEpoch = 1U;
				owned.publicationEpoch = 1U;
				owned.state = CharacterMotionLeaseState::LocomotionOwned;
				const std::array changes{CharacterMotionLeaseChange{unleased, owned}};
				if(!backend.PublishCharacterMotionLeases(changes).Succeeded())
					throw std::runtime_error("Velocity fixture lease acquisition failed.");
				return owned;
			}

			CharacterMotionTarget Target(const CharacterMotionLease& lease, PhysicsVector3 velocity,
				std::uint64_t updateEpoch) const {
				CharacterMotionTarget target;
				target.controller = lease.controller;
				target.owner = lease.owner;
				target.authorityEpoch = lease.authorityEpoch;
				target.motionEpoch = lease.motionEpoch;
				target.locomotionUpdateEpoch = updateEpoch;
				target.mode = CharacterMotionMode::Velocity;
				target.frame = {kPrimaryPhysicsFrameId, 1U};
				target.planarVelocity = velocity;
				target.acceleration = 4.0F;
				target.braking = 8.0F;
				return target;
			}

			PhysicsBackendCharacterState Step(const PhysicsCharacterCommandTarget& controller, PhysicsTick tick) {
				if(!backend.AdvanceCharacters(world, tick, kFixedDeltaTime).Succeeded()
					|| !backend.Step({world, tick, kFixedDeltaTime, 1U}).Succeeded())
					throw std::runtime_error("Velocity fixture fixed step failed.");
				PhysicsBackendExtraction extraction;
				extraction.world = world;
				if(!backend.Extract(extraction).Succeeded()) throw std::runtime_error("Velocity fixture extraction failed.");
				const auto found = std::find_if(extraction.characters.begin(), extraction.characters.end(),
					[&controller](const PhysicsBackendCharacterState& state) { return state.handle == controller.character; });
				if(found == extraction.characters.end()) throw std::runtime_error("Velocity fixture character disappeared.");
				return *found;
			}

			void AddStaticBox(PhysicsVector3 dimensions, PhysicsVector3 position,
				PhysicsQuaternion rotation = {}) {
				PhysicsShapeHandle shape{{73U}, 1U, 91U, 1U};
				PhysicsBodyHandle body{{73U}, 1U, 91U, 1U};
				PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = dimensions;
				PhysicsBodyCreateRequest request;
				request.handle = body;
				request.objectId = TerraEngine::GenerateUUID();
				request.objectGeneration = 1U;
				request.componentGeneration = 1U;
				request.shape = shape;
				request.motionType = PhysicsMotionType::Static;
				request.pose.position = position;
				request.pose.rotation = rotation;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateShape(world, shape, recipe, staging).Succeeded()
					|| !backend.CreateBody(request, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("Velocity fixture obstacle creation failed.");
			}

			void AddWall() {
				AddStaticBox({1.0F, 2.0F, 3.0F}, {2.0F, 1.0F, 0.0F});
			}
		};
	}

	void RegisterLocomotionPhysicsTests(TestRegistry& registry) {
		registry.Add("Locomotion.Physics.FixedStepAccelerationAt30_60_120Updates", [](TestContext& context) {
			std::array<float, 3U> distances{};
			const std::array renderRates{30U, 60U, 120U};
			for(std::size_t caseIndex = 0U; caseIndex < renderRates.size(); ++caseIndex) {
				VelocityFixture fixture;
				const auto controller = fixture.CreateCharacter();
				const auto lease = fixture.Acquire(controller);
				std::uint64_t updateEpoch = 0U;
				for(PhysicsTick tick = 1U; tick <= 120U; ++tick) {
					// Render cadence changes only how often a copied target arrives;
					// the same 60 Hz physical duration must shape each step.
					const std::uint32_t submissions = renderRates[caseIndex] == 120U ? 2U
						: renderRates[caseIndex] == 60U || (tick & 1U) != 0U ? 1U : 0U;
					for(std::uint32_t i = 0U; i < submissions; ++i) {
						context.Expect(fixture.backend.PublishCharacterMotionTarget(
							fixture.Target(lease, {4.0F, 0.0F, 0.0F}, ++updateEpoch)).Succeeded(),
							"Every render-rate target should be accepted before its physical step.");
					}
					distances[caseIndex] = fixture.Step(controller, tick).pose.position.x;
				}
				context.Expect(std::fabs(distances[caseIndex] - 6.0F) < 0.2F,
					"Acceleration over fixed physical duration should travel about six metres in two seconds.");
			}
			context.Expect(std::fabs(distances[0] - distances[1]) < 0.05F
				&& std::fabs(distances[1] - distances[2]) < 0.05F,
				"30, 60 and 120 render updates must produce the same fixed-step displacement.");
		});

		registry.Add("Locomotion.Physics.ReversalBrakesThroughZero", [](TestContext& context) {
			VelocityFixture fixture;
			const auto controller = fixture.CreateCharacter();
			const auto lease = fixture.Acquire(controller);
			context.Expect(fixture.backend.PublishCharacterMotionTarget(
				fixture.Target(lease, {4.0F, 0.0F, 0.0F}, 1U)).Succeeded(), "Forward target must be accepted.");
			PhysicsBackendCharacterState state;
			for(PhysicsTick tick = 1U; tick <= 60U; ++tick) state = fixture.Step(controller, tick);
			const float reverseStart = state.pose.position.x;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(
				fixture.Target(lease, {-4.0F, 0.0F, 0.0F}, 2U)).Succeeded(), "Reverse target must be accepted.");
			const float firstReverseX = fixture.Step(controller, 61U).pose.position.x;
			for(PhysicsTick tick = 62U; tick <= 90U; ++tick) state = fixture.Step(controller, tick);
			const float zeroCrossingX = state.pose.position.x;
			for(PhysicsTick tick = 91U; tick <= 120U; ++tick) state = fixture.Step(controller, tick);
			context.Expect(firstReverseX >= reverseStart && zeroCrossingX > reverseStart + 0.8F,
				"An exact reversal must brake forward speed through zero before moving backward.");
			context.Expect(state.pose.position.x < zeroCrossingX - 0.5F,
				"After crossing zero, the controller should accelerate toward the new reverse target.");
		});

		registry.Add("Locomotion.Physics.FixedTickYawAndHold", [](TestContext& context) {
			VelocityFixture fixture;
			const auto controller = fixture.CreateCharacter();
			const auto lease = fixture.Acquire(controller);
			auto facing = fixture.Target(lease, {}, 1U);
			facing.hasFacing = true;
			facing.desiredFacing = {1.0F, 0.0F, 0.0F};
			facing.turnRate = 1.0F;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(facing).Succeeded(),
				"An upright centered capsule should accept a physical facing target.");
			PhysicsBackendCharacterState state;
			for(PhysicsTick tick = 1U; tick <= 15U; ++tick) state = fixture.Step(controller, tick);
			const float firstYaw = 2.0F * std::atan2(std::fabs(state.pose.rotation.y),
				std::fabs(state.pose.rotation.w));
			context.Expect(firstYaw > 0.18F && firstYaw < 0.32F,
				"A quarter-second at one radian per second should turn only about a quarter radian.");
			facing.locomotionUpdateEpoch = 2U;
			facing.hasFacing = false;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(facing).Succeeded(),
				"Hold facing should accept a target without requested yaw.");
			for(PhysicsTick tick = 16U; tick <= 30U; ++tick) state = fixture.Step(controller, tick);
			const float heldYaw = 2.0F * std::atan2(std::fabs(state.pose.rotation.y),
				std::fabs(state.pose.rotation.w));
			context.Expect(std::fabs(heldYaw - firstYaw) < 0.01F,
				"Hold facing must preserve the existing physical orientation.");
		});

		registry.Add("Locomotion.Physics.LockedAndUnsafeFacingReject", [](TestContext& context) {
			{
				VelocityFixture fixture;
				const auto controller = fixture.CreateCharacter(PhysicsCharacterRotationPolicy::Locked);
				const auto lease = fixture.Acquire(controller);
				auto facing = fixture.Target(lease, {}, 1U);
				facing.hasFacing = true;
				facing.desiredFacing = {1.0F, 0.0F, 0.0F};
				facing.turnRate = 1.0F;
				context.Expect(!fixture.backend.PublishCharacterMotionTarget(facing).Succeeded(),
					"Locked character rotation must reject a requested locomotion yaw.");
			}
			{
				VelocityFixture fixture;
				const auto controller = fixture.CreateCharacter(PhysicsCharacterRotationPolicy::AlignToUp,
					{0.2F, 0.0F, 0.0F});
				const auto lease = fixture.Acquire(controller);
				auto facing = fixture.Target(lease, {}, 1U);
				facing.hasFacing = true;
				facing.desiredFacing = {1.0F, 0.0F, 0.0F};
				facing.turnRate = 1.0F;
				context.Expect(!fixture.backend.PublishCharacterMotionTarget(facing).Succeeded(),
					"An off-axis capsule center must reject collision-unsafe facing rotation.");
			}
			{
				VelocityFixture fixture;
				const auto controller = fixture.CreateCharacter(PhysicsCharacterRotationPolicy::AlignToUp,
					{}, {0.2F, 0.98F, 0.0F});
				const auto lease = fixture.Acquire(controller);
				auto facing = fixture.Target(lease, {}, 1U);
				facing.hasFacing = true;
				facing.desiredFacing = {1.0F, 0.0F, 0.0F};
				facing.turnRate = 1.0F;
				context.Expect(!fixture.backend.PublishCharacterMotionTarget(facing).Succeeded(),
					"A tilted symmetry axis must reject collision-unsafe facing rotation.");
			}
		});

		registry.Add("Locomotion.Physics.WallLimitsRealizedNotRequestedMotion", [](TestContext& context) {
			VelocityFixture fixture;
			fixture.AddWall();
			const auto controller = fixture.CreateCharacter();
			const auto lease = fixture.Acquire(controller);
			context.Expect(fixture.backend.PublishCharacterMotionTarget(
				fixture.Target(lease, {6.0F, 0.0F, 0.0F}, 1U)).Succeeded(), "A wall approach target must be accepted.");
			PhysicsBackendCharacterState state;
			for(PhysicsTick tick = 1U; tick <= 120U; ++tick) state = fixture.Step(controller, tick);
			context.Expect(state.pose.position.x < 1.0F,
				"The physical controller must stop before the solid wall, regardless of requested speed.");
			context.Expect(state.requestedPlanarVelocity.x > 3.0F
				&& std::fabs(state.realizedDisplacement.x) < 0.01F
				&& state.completedStepDuration > 0.0F && !state.motionDiscontinuity,
				"Collision feedback must distinguish active requested speed from near-zero realized movement.");
		});

		registry.Add("Locomotion.Physics.StepAndSlopeUseControllerCollision", [](TestContext& context) {
			{
				VelocityFixture fixture;
				fixture.AddStaticBox({1.0F, 0.3F, 3.0F}, {2.0F, 0.15F, 0.0F});
				const auto controller = fixture.CreateCharacter();
				const auto lease = fixture.Acquire(controller);
				context.Expect(fixture.backend.PublishCharacterMotionTarget(
					fixture.Target(lease, {6.0F, 0.0F, 0.0F}, 1U)).Succeeded(),
					"A step approach target must be admitted.");
				float highestY = 0.0F;
				PhysicsBackendCharacterState state;
				for(PhysicsTick tick = 1U; tick <= 120U; ++tick) {
					state = fixture.Step(controller, tick);
					highestY = std::max(highestY, state.pose.position.y);
				}
				context.Expect(state.pose.position.x > 2.5F && highestY > 1.2F,
					"A supported low step should be traversed by CharacterVirtual, with a measurable rise.");
			}
			{
				VelocityFixture fixture;
				// A shallow up-ramp starts close to the floor; yaw-only facing may
				// turn on it, but the movement itself remains Jolt collision-owned.
				fixture.AddStaticBox({4.0F, 0.2F, 3.0F}, {3.0F, 0.6F, 0.0F},
					{0.0F, 0.0F, 0.130526F, 0.991445F});
				const auto controller = fixture.CreateCharacter();
				const auto lease = fixture.Acquire(controller);
				context.Expect(fixture.backend.PublishCharacterMotionTarget(
					fixture.Target(lease, {6.0F, 0.0F, 0.0F}, 1U)).Succeeded(),
					"A slope approach target must be admitted.");
				float highestY = 0.0F;
				PhysicsBackendCharacterState state;
				for(PhysicsTick tick = 1U; tick <= 120U; ++tick) {
					state = fixture.Step(controller, tick);
					highestY = std::max(highestY, state.pose.position.y);
				}
				context.Expect(state.pose.position.x > 4.0F && highestY > 1.35F,
					"An allowed shallow slope should preserve collision-owned ascent and forward travel.");
			}
		});

		registry.Add("Locomotion.Physics.GravityRemainsOutsidePlanarAccelerationLimit", [](TestContext& context) {
			VelocityFixture fixture;
			const auto controller = fixture.CreateCharacter(PhysicsCharacterRotationPolicy::AlignToUp,
				{}, {0.0F, 1.0F, 0.0F}, true);
			const auto lease = fixture.Acquire(controller);
			context.Expect(fixture.backend.PublishCharacterMotionTarget(
				fixture.Target(lease, {4.0F, 0.0F, 0.0F}, 1U)).Succeeded(),
				"A falling character should still accept planar locomotion intent.");
			PhysicsBackendCharacterState state;
			for(PhysicsTick tick = 1U; tick <= 20U; ++tick) state = fixture.Step(controller, tick);
			// From four metres, 20 steps at 60 Hz under 9.81 m/s² fall only
			// about 0.55 m; demanding 0.6 m would exceed physical gravity.
			context.Expect(state.pose.position.y < 3.7F && state.linearVelocity.y < -2.0F,
				"Gravity must retain its vertical acceleration, independent of planar locomotion limits.");
			context.Expect(std::fabs(state.requestedPlanarVelocity.y) < 0.01F,
				"The copied locomotion request must remain planar even while external gravity acts.");
		});

		registry.Add("Locomotion.Physics.CompletedTickAndTeleportResetDerivative", [](TestContext& context) {
			VelocityFixture fixture;
			const auto controller = fixture.CreateCharacter();
			const auto owned = fixture.Acquire(controller);
			context.Expect(fixture.backend.PublishCharacterMotionTarget(
				fixture.Target(owned, {4.0F, 0.0F, 0.0F}, 1U)).Succeeded(), "The observation target must be accepted.");
			const auto moved = fixture.Step(controller, 1U);
			context.Expect(std::fabs(moved.completedStepDuration - kFixedDeltaTime) < 0.0001F
				&& moved.requestedPlanarVelocity.x > 0.0F
				&& moved.realizedDisplacement.x > 0.0F && !moved.motionDiscontinuity,
				"A completed tick must carry valid requested and realized movement over its exact duration.");
			CharacterMotionLease released;
			released.controller = controller;
			released.authorityEpoch = 2U;
			released.motionEpoch = 2U;
			released.publicationEpoch = 2U;
			const std::array release{CharacterMotionLeaseChange{owned, released}};
			context.Expect(fixture.backend.PublishCharacterMotionLeases(release).Succeeded(), "Observation reset must release the lease.");
			PhysicsCharacterUpdateRequest teleport;
			teleport.handle = controller.character;
			teleport.objectId = controller.objectId;
			teleport.objectGeneration = controller.objectGeneration;
			teleport.componentGeneration = controller.componentGeneration;
			teleport.characterMotionEpoch = released.motionEpoch;
			teleport.teleportRequested = true;
			teleport.pose.position = {30.0F, 1.1F, 0.0F};
			context.Expect(fixture.backend.UpdateCharacter(teleport).Succeeded(), "The unleased teleport must be admitted.");
			const auto jumped = fixture.Step(controller, 2U);
			context.Expect(jumped.pose.position.x > 29.0F && jumped.motionDiscontinuity
				&& std::fabs(jumped.realizedDisplacement.x) < 0.01F,
				"Teleport must not become a large measured speed on the completed tick.");
		});
	}
}
