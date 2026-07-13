// Bounded provider integration checks through the engine-owned backend API.
// Jolt types and IDs intentionally do not appear in this test.
#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsCommands.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() { ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(); }
			~ThreadPoolSession() { ThreadPool::instance().shutdown(); }
		};

		struct Fixture {
			ThreadPoolSession pool;
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{1U}, 1U};
			PhysicsShapeHandle shape{{1U}, 1U, 0U, 1U};
			PhysicsBodyHandle body{{1U}, 1U, 0U, 1U};
			PhysicsBodyHandle noGravityBody{{1U}, 1U, 1U, 1U};
			PhysicsBodyHandle heavyNoGravityBody{{1U}, 1U, 2U, 1U};
			TerraEngine::UUID bodyObjectId{};
			TerraEngine::UUID noGravityObjectId{};
			TerraEngine::UUID heavyObjectId{};

			Fixture() {
				if (!backend.Initialize().Succeeded()) return;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				backend.CreateWorld(configuration);
				PhysicsBackendStagingToken staging;
				PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = {1.0F, 1.0F, 1.0F};
				backend.CreateShape(world, shape, recipe, staging);
				PhysicsBodyCreateRequest request;
				request.handle = body;
				request.objectId = TerraEngine::GenerateUUID();
				bodyObjectId = request.objectId;
				request.objectGeneration = 1U;
				request.componentGeneration = 1U;
				request.shape = shape;
				request.motionType = PhysicsMotionType::Dynamic;
				request.pose.position.y = 10.0F;
				backend.CreateBody(request, staging);
				request.handle = noGravityBody;
				request.objectId = TerraEngine::GenerateUUID();
				noGravityObjectId = request.objectId;
				request.useGravity = false;
				backend.CreateBody(request, staging);
				request.handle = heavyNoGravityBody;
				request.objectId = TerraEngine::GenerateUUID();
				heavyObjectId = request.objectId;
				request.pose.position.x = 4.0F;
				request.mass = 10.0F;
				backend.CreateBody(request, staging);
				backend.CommitStaging(staging);
			}
			~Fixture() { backend.DestroyWorld(world); backend.Shutdown(); }

			PhysicsBodyHandle CreateBodyForTest(
				std::uint32_t index,
				TerraEngine::UUID& objectId,
				PhysicsMotionType motionType,
				PhysicsVector3 position,
				bool sensor = false,
				std::uint8_t collisionLayer = 0U,
				std::uint32_t collisionMask = 0xFFFFFFFFU,
				PhysicsShapeHandle shapeOverride = {},
				PhysicsMotionQuality motionQuality = PhysicsMotionQuality::Discrete,
				PhysicsVector3 linearVelocity = {},
				float friction = 0.8F,
				float restitution = 0.25F)
			{
				PhysicsBodyHandle handle{{1U}, 1U, index, 1U};
				PhysicsBodyCreateRequest request;
				request.handle = handle;
				request.objectId = TerraEngine::GenerateUUID();
				request.objectGeneration = 1U;
				request.componentGeneration = 1U;
				request.shape = shapeOverride.IsValid() ? shapeOverride : shape;
				request.motionType = motionType;
				request.motionQuality = motionQuality;
				request.pose.position = position;
				request.linearVelocity = linearVelocity;
				request.collisionLayer = collisionLayer;
				request.collisionMask = collisionMask;
				request.sensor = sensor;
				request.friction = friction;
				request.restitution = restitution;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateBody(request, staging).Succeeded() || !backend.CommitStaging(staging).Succeeded()) {
					return {};
				}
				objectId = request.objectId;
				return handle;
			}
		};
	}

	void RegisterPhysicsRigidBodySimulationTests(TestRegistry& registry)
	{
		registry.Add("Physics.RigidBodySimulation.GravityToggleAndMassIndependentAcceleration", [](TestContext& context) {
			Fixture fixture;
			PhysicsForcePayload accelerationPayload;
			accelerationPayload.value = {0.0F, 3.0F, 0.0F};
			accelerationPayload.mode = PhysicsForceMode::Acceleration;
			PhysicsBackendStepRequest step{fixture.world, 1U, 1.0F / 60.0F, 1U};
			context.Expect(fixture.backend.Step(step).Succeeded(), "A valid dynamic fixture must step.");
			PhysicsBackendExtraction before;
			before.world = fixture.world;
			fixture.backend.Extract(before);
			context.Expect(!before.bodies.empty(), "Extraction must publish the created body.");
			const auto gravityBody = std::find_if(before.bodies.begin(), before.bodies.end(), [&](const auto& body) { return body.handle == fixture.body; });
			const auto noGravity = std::find_if(before.bodies.begin(), before.bodies.end(), [&](const auto& body) { return body.handle == fixture.noGravityBody; });
			context.Expect(gravityBody != before.bodies.end() && noGravity != before.bodies.end(), "Both gravity modes must be extractable.");
			context.Expect(gravityBody->linearVelocity.y < noGravity->linearVelocity.y,
				"Built-in gravity must affect only the useGravity body.");
			context.Expect(std::isfinite(gravityBody->linearVelocity.y) && std::isfinite(noGravity->linearVelocity.y), "Gravity outputs must remain finite.");
			PhysicsCommand lightAcceleration = PhysicsCommand::MakeApplyForce(fixture.noGravityObjectId, 1U, 1U);
			lightAcceleration.type = PhysicsCommandType::ApplyAcceleration;
			lightAcceleration.payload = accelerationPayload;
			PhysicsCommand heavyAcceleration = PhysicsCommand::MakeApplyForce(fixture.heavyObjectId, 1U, 1U);
			heavyAcceleration.type = PhysicsCommandType::ApplyAcceleration;
			heavyAcceleration.payload = accelerationPayload;
			context.Expect(fixture.backend.ApplyCommand(lightAcceleration).Succeeded()
				&& fixture.backend.ApplyCommand(heavyAcceleration).Succeeded(),
				"Mass-independent acceleration commands must be accepted for both masses.");
			step.tick = 2U;
			context.Expect(fixture.backend.Step(step).Succeeded(), "Acceleration fixture must complete a second fixed step.");
			PhysicsBackendExtraction after;
			after.world = fixture.world;
			fixture.backend.Extract(after);
			const auto lightAfter = std::find_if(after.bodies.begin(), after.bodies.end(), [&](const auto& value) {
				return value.handle == fixture.noGravityBody;
			});
			const auto heavyAfter = std::find_if(after.bodies.begin(), after.bodies.end(), [&](const auto& value) {
				return value.handle == fixture.heavyNoGravityBody;
			});
			context.Expect(lightAfter != after.bodies.end() && heavyAfter != after.bodies.end(),
				"Both acceleration bodies must remain extractable.");
			context.Expect(std::fabs(lightAfter->linearVelocity.y - heavyAfter->linearVelocity.y) < 1.0e-4F,
				"Acceleration mode must produce the same velocity change for one and ten kilogram bodies.");
		});

		registry.Add("Physics.RigidBodySimulation.StaticDynamicKinematicAndSleepWake", [](TestContext& context) {
			Fixture fixture;
			TerraEngine::UUID staticId{};
			TerraEngine::UUID kinematicId{};
			PhysicsShapeHandle floorShape{{1U}, 1U, 1U, 1U};
			PhysicsShapeRecipe floorRecipe;
			floorRecipe.kind = PhysicsShapeKind::Box;
			floorRecipe.dimensions = {20.0F, 1.0F, 20.0F};
			PhysicsBackendStagingToken floorStaging;
			context.Expect(fixture.backend.CreateShape(fixture.world, floorShape, floorRecipe, floorStaging).Succeeded()
				&& fixture.backend.CommitStaging(floorStaging).Succeeded(),
				"The static support must publish a broad collision shape for the soak fixture.");
			const PhysicsBodyHandle staticFloor = fixture.CreateBodyForTest(
				3U, staticId, PhysicsMotionType::Static, {0.0F, -1.0F, 0.0F}, false, 0U, 0xFFFFFFFFU, floorShape);
			const PhysicsBodyHandle kinematic = fixture.CreateBodyForTest(
				4U, kinematicId, PhysicsMotionType::Kinematic, {-3.0F, 2.0F, 0.0F});
			context.Expect(staticFloor.IsValid() && kinematic.IsValid(),
				"The static floor and kinematic fixture bodies must stage and publish.");

			PhysicsForcePayload forcePayload;
			// Use a vertical force so the one-metre shared box fixture does not walk
			// sideways off the deliberately compact static support during the soak.
			forcePayload.value = {0.0F, 1.0F, 0.0F};
			PhysicsCommand staticForce = PhysicsCommand::MakeApplyForce(staticId, 1U, 1U);
			staticForce.payload = forcePayload;
			context.Expect(!fixture.backend.ApplyCommand(staticForce).Succeeded(),
				"Static bodies must reject force commands instead of silently becoming dynamic.");
			PhysicsCommand dynamicForce = PhysicsCommand::MakeApplyForce(fixture.bodyObjectId, 1U, 1U);
			dynamicForce.payload = forcePayload;
			context.Expect(fixture.backend.ApplyCommand(dynamicForce).Succeeded(),
				"Dynamic bodies must accept typed force commands.");

			PhysicsCommand kinematicTarget;
			kinematicTarget.type = PhysicsCommandType::SetKinematicTarget;
			kinematicTarget.worldId = fixture.world.worldId;
			kinematicTarget.worldGeneration = fixture.world.worldGeneration;
			kinematicTarget.targetObjectId = kinematicId;
			kinematicTarget.componentGeneration = 1U;
			PhysicsMotionPayload target;
			target.pose.position = {3.0F, 2.0F, 0.0F};
			target.duration = 1.0F / 60.0F;
			kinematicTarget.payload = target;
			context.Expect(fixture.backend.ApplyCommand(kinematicTarget).Succeeded(),
				"A kinematic body must accept a duration-qualified kinematic target.");

			context.Expect(fixture.backend.Step({fixture.world, 3U, 1.0F / 60.0F, 1U}).Succeeded(),
				"The kinematic target must advance on its requested fixed boundary.");
			PhysicsBackendExtraction targetExtraction;
			targetExtraction.world = fixture.world;
			context.Expect(fixture.backend.Extract(targetExtraction).Succeeded(),
				"The target boundary must publish kinematic state.");
			const auto targetState = std::find_if(targetExtraction.bodies.begin(), targetExtraction.bodies.end(), [&](const auto& state) {
				return state.handle == kinematic;
			});
			context.Expect(targetState != targetExtraction.bodies.end()
				&& std::fabs(targetState->pose.position.x - 3.0F) < 0.05F,
				"Kinematic targets must reach their requested pose on the fixed boundary.");
			// Reissuing the reached target gives the kinematic body zero commanded
			// velocity for the longer dynamic/static settling soak below.
			context.Expect(fixture.backend.ApplyCommand(kinematicTarget).Succeeded(),
				"A reached kinematic target must accept a stationary follow-up target.");
			for(PhysicsTick tick = 4U; tick < 183U; ++tick) {
				context.Expect(fixture.backend.Step({fixture.world, tick, 1.0F / 60.0F, 1U}).Succeeded(),
					"Static/dynamic/kinematic fixture steps must remain healthy.");
			}
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			context.Expect(fixture.backend.Extract(extraction).Succeeded(), "The mixed-motion fixture must extract body states.");
			const auto dynamicState = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& state) {
				return state.handle == fixture.body;
			});
			const auto kinematicState = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& state) {
				return state.handle == kinematic;
			});
			context.Expect(dynamicState != extraction.bodies.end() && kinematicState != extraction.bodies.end(),
				"Mixed-motion extraction must retain both dynamic and kinematic state.");
			context.Expect(dynamicState->pose.position.y > -0.25F,
				"A falling dynamic body must remain supported by the published static collider; y="
					+ std::to_string(dynamicState->pose.position.y));

			PhysicsCommand sleep = PhysicsCommand::MakeWake(fixture.bodyObjectId, 1U, 1U);
			sleep.type = PhysicsCommandType::SleepBody;
			context.Expect(fixture.backend.ApplyCommand(sleep).Succeeded(), "A dynamic body must accept an explicit sleep request.");
			context.Expect(fixture.backend.Extract(extraction).Succeeded(), "Sleep state must remain extractable.");
			const auto sleepingState = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& state) {
				return state.handle == fixture.body;
			});
			context.Expect(sleepingState != extraction.bodies.end() && !sleepingState->awake,
				"Sleep must suppress the dynamic body's active state until it is woken.");
			context.Expect(fixture.backend.ApplyCommand(PhysicsCommand::MakeWake(fixture.bodyObjectId, 1U, 1U)).Succeeded(),
				"A sleeping dynamic body must accept a wake command.");
			context.Expect(fixture.backend.Extract(extraction).Succeeded(), "Wake state must remain extractable.");
			const auto awakeState = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& state) {
				return state.handle == fixture.body;
			});
			context.Expect(awakeState != extraction.bodies.end() && awakeState->awake,
				"Wake must reactivate the dynamic body without rebuilding it.");
			context.Expect(fixture.backend.IsHealthy(), "Backend must remain healthy after body lifecycle operations.");
		});

		registry.Add("Physics.RigidBodySimulation.LayersAndTriggersFilterQueries", [](TestContext& context) {
			Fixture fixture;
			TerraEngine::UUID triggerId{};
			TerraEngine::UUID solidId{};
			const PhysicsBodyHandle trigger = fixture.CreateBodyForTest(
				3U, triggerId, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F}, true, 1U, 0xFFFFFFFFU);
			const PhysicsBodyHandle solid = fixture.CreateBodyForTest(
				4U, solidId, PhysicsMotionType::Static, {4.0F, 0.0F, 0.0F}, false, 2U, 0xFFFFFFFFU);
			context.Expect(trigger.IsValid() && solid.IsValid(), "Layer and trigger bodies must publish before queries execute.");

			PhysicsBackendQueryRequest query;
			query.world = fixture.world;
			query.pose.position = {-3.0F, 0.0F, 0.0F};
			query.direction = {1.0F, 0.0F, 0.0F};
			query.distance = 10.0F;
			query.maximumHits = 1U;
			query.filter.collisionMask = (1U << 1U);
			query.filter.includeSensors = false;
			const PhysicsBackendQueryResult hiddenTrigger = fixture.backend.Query(query);
			context.Expect(hiddenTrigger.result.Succeeded() && hiddenTrigger.hits.empty(),
				"A trigger on the selected layer must be omitted when sensor queries are disabled.");

			query.filter.includeSensors = true;
			const PhysicsBackendQueryResult visibleTrigger = fixture.backend.Query(query);
			context.Expect(visibleTrigger.result.Succeeded() && !visibleTrigger.hits.empty()
				&& visibleTrigger.hits.front().body == trigger && visibleTrigger.hits.front().sensor,
				"The same selected layer must report its trigger when sensor queries are enabled.");

			query.filter.collisionMask = (1U << 2U);
			query.filter.includeSensors = false;
			const PhysicsBackendQueryResult visibleSolid = fixture.backend.Query(query);
			context.Expect(visibleSolid.result.Succeeded() && !visibleSolid.hits.empty()
				&& visibleSolid.hits.front().body == solid && !visibleSolid.hits.front().sensor,
				"Layer masks must exclude unrelated trigger layers and select the solid collider.");
		});

		registry.Add("Physics.RigidBodySimulation.LinearCastPreventsFastThinGeometryTunnelling", [](TestContext& context) {
			Fixture fixture;
			PhysicsShapeHandle floorShape{{1U}, 1U, 1U, 1U};
			PhysicsShapeRecipe floorRecipe;
			floorRecipe.kind = PhysicsShapeKind::Box;
			floorRecipe.dimensions = {30.0F, 0.1F, 30.0F};
			PhysicsBackendStagingToken floorStaging;
			context.Expect(fixture.backend.CreateShape(fixture.world, floorShape, floorRecipe, floorStaging).Succeeded()
				&& fixture.backend.CommitStaging(floorStaging).Succeeded(),
				"The CCD fixture must publish thin static geometry.");
			TerraEngine::UUID floorId{};
			TerraEngine::UUID discreteId{};
			TerraEngine::UUID linearCastId{};
			const PhysicsBodyHandle floor = fixture.CreateBodyForTest(
				3U, floorId, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F}, false, 0U, 0xFFFFFFFFU, floorShape);
			const PhysicsBodyHandle discrete = fixture.CreateBodyForTest(
				4U, discreteId, PhysicsMotionType::Dynamic, {-4.0F, 5.0F, 0.0F}, false, 0U, 0xFFFFFFFFU, {},
				PhysicsMotionQuality::Discrete, {0.0F, -500.0F, 0.0F});
			const PhysicsBodyHandle linearCast = fixture.CreateBodyForTest(
				5U, linearCastId, PhysicsMotionType::Dynamic, {4.0F, 5.0F, 0.0F}, false, 0U, 0xFFFFFFFFU, {},
				PhysicsMotionQuality::LinearCast, {0.0F, -500.0F, 0.0F});
			context.Expect(floor.IsValid() && discrete.IsValid() && linearCast.IsValid(),
				"Discrete and LinearCast bodies must publish against the same thin collider.");
			context.Expect(fixture.backend.Step({fixture.world, 3U, 1.0F / 60.0F, 1U}).Succeeded(),
				"The fast-body CCD fixture must complete one fixed step.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			context.Expect(fixture.backend.Extract(extraction).Succeeded(), "CCD body states must be extractable.");
			const auto discreteState = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& state) {
				return state.handle == discrete;
			});
			const auto linearCastState = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& state) {
				return state.handle == linearCast;
			});
			context.Expect(discreteState != extraction.bodies.end() && linearCastState != extraction.bodies.end(),
				"Both motion-quality results must be published.");
			context.Expect(linearCastState->pose.position.y > -0.55F,
				"LinearCast must keep a fast body above the thin geometry instead of tunnelling.");
			context.Expect(linearCastState->pose.position.y > discreteState->pose.position.y + 0.5F,
				"The calibrated fixture must demonstrate a materially different CCD outcome from Discrete mode.");
		});

		registry.Add("Physics.RigidBodySimulation.MaterialRestitutionChangesContactResponse", [](TestContext& context) {
			Fixture fixture;
			PhysicsShapeHandle floorShape{{1U}, 1U, 1U, 1U};
			PhysicsShapeRecipe floorRecipe;
			floorRecipe.kind = PhysicsShapeKind::Box;
			floorRecipe.dimensions = {30.0F, 1.0F, 30.0F};
			PhysicsBackendStagingToken floorStaging;
			context.Expect(fixture.backend.CreateShape(fixture.world, floorShape, floorRecipe, floorStaging).Succeeded()
				&& fixture.backend.CommitStaging(floorStaging).Succeeded(),
				"The material fixture must publish a broad static support shape.");
			TerraEngine::UUID floorId{};
			TerraEngine::UUID deadId{};
			TerraEngine::UUID bouncyId{};
			const PhysicsBodyHandle floor = fixture.CreateBodyForTest(
				3U, floorId, PhysicsMotionType::Static, {0.0F, -0.5F, 0.0F}, false, 0U, 0xFFFFFFFFU,
				floorShape, PhysicsMotionQuality::Discrete, {}, 0.8F, 0.0F);
			const PhysicsBodyHandle dead = fixture.CreateBodyForTest(
				4U, deadId, PhysicsMotionType::Dynamic, {-8.0F, 4.0F, 0.0F}, false, 0U, 0xFFFFFFFFU,
				{}, PhysicsMotionQuality::Discrete, {}, 0.8F, 0.0F);
			const PhysicsBodyHandle bouncy = fixture.CreateBodyForTest(
				5U, bouncyId, PhysicsMotionType::Dynamic, {8.0F, 4.0F, 0.0F}, false, 0U, 0xFFFFFFFFU,
				{}, PhysicsMotionQuality::Discrete, {}, 0.8F, 1.0F);
			context.Expect(floor.IsValid() && dead.IsValid() && bouncy.IsValid(),
				"Both engine material values must publish before contact simulation.");

			float deadPeakUpwardVelocity = 0.0F;
			float bouncyPeakUpwardVelocity = 0.0F;
			for(PhysicsTick tick = 3U; tick < 123U; ++tick) {
				context.Expect(fixture.backend.Step({fixture.world, tick, 1.0F / 60.0F, 1U}).Succeeded(),
					"Material response simulation must remain healthy.");
				PhysicsBackendExtraction extraction;
				extraction.world = fixture.world;
				context.Expect(fixture.backend.Extract(extraction).Succeeded(), "Material response states must be extractable.");
				for(const PhysicsBackendBodyState& state : extraction.bodies) {
					if(state.handle == dead) deadPeakUpwardVelocity = std::max(deadPeakUpwardVelocity, state.linearVelocity.y);
					if(state.handle == bouncy) bouncyPeakUpwardVelocity = std::max(bouncyPeakUpwardVelocity, state.linearVelocity.y);
				}
			}
			context.Expect(bouncyPeakUpwardVelocity > deadPeakUpwardVelocity + 2.0F,
				"High restitution must create a measurably stronger rebound than a zero-restitution material.");
		});
	}
}
