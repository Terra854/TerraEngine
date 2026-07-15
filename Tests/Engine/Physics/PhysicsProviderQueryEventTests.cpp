// PhysicsProviderQueryEventTests.cpp - direct Jolt 5.6 provider coverage.
//
// These tests deliberately use only the engine-owned backend interface. They
// prove that Jolt's real narrow-phase queries and callback bridge preserve the
// provider-neutral contracts without exposing Jolt IDs or callback objects.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsCommands.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		// The Jolt adapter consumes jobs from the engine-wide pool. Keeping the
		// lifetime local prevents a provider job from escaping this test fixture.
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

		struct BackendFixture final {
			ThreadPoolSession pool;
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{73U}, 1U};
			std::uint32_t nextShapeSlot = 1U;
			std::uint32_t nextBodySlot = 1U;

			bool Initialize() {
				if(!backend.Initialize().Succeeded()) return false;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				return backend.CreateWorld(configuration).Succeeded();
			}

			~BackendFixture() {
				// Shutdown is best-effort because individual assertions report setup
				// failures before this cleanup path executes.
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
			}

			PhysicsShapeHandle PublishBox(PhysicsVector3 dimensions, bool sensor = false) {
				const PhysicsShapeHandle shape{{world.worldId}, world.worldGeneration, nextShapeSlot++, 1U};
				PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = dimensions;
				recipe.sensor = sensor;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateShape(world, shape, recipe, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded()) {
					return {};
				}
				return shape;
			}

			PhysicsBodyHandle PublishBody(
				PhysicsShapeHandle shape,
				PhysicsMotionType motionType,
				PhysicsVector3 position,
				TerraEngine::UUID& objectId,
				std::uint8_t collisionLayer = 0U,
				bool sensor = false,
				bool useGravity = true) {
				const PhysicsBodyHandle body{{world.worldId}, world.worldGeneration, nextBodySlot++, 1U};
				PhysicsBodyCreateRequest request;
				request.handle = body;
				request.objectId = TerraEngine::GenerateUUID();
				request.objectGeneration = 1U;
				request.componentGeneration = 1U;
				request.shape = shape;
				request.motionType = motionType;
				request.pose.position = position;
				request.collisionLayer = collisionLayer;
				request.sensor = sensor;
				request.useGravity = useGravity;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateBody(request, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded()) {
					return {};
				}
				objectId = request.objectId;
				return body;
			}

			PhysicsBackendExtraction StepAndExtract(PhysicsTick tick, bool& stepped) {
				PhysicsBackendExtraction extraction;
				extraction.world = world;
				stepped = backend.Step({world, tick, 1.0F / 60.0F, 1U}).Succeeded()
					&& backend.Extract(extraction).Succeeded();
				return extraction;
			}
		};

		bool HasEvent(const std::vector<PhysicsEvent>& events, PhysicsEventType type) {
			return std::any_of(events.begin(), events.end(), [type](const PhysicsEvent& event) {
				return event.type == type;
			});
		}

		void AppendEvents(std::vector<PhysicsEvent>& destination, PhysicsBackendExtraction&& extraction) {
			destination.insert(destination.end(),
			std::make_move_iterator(extraction.events.begin()),
			std::make_move_iterator(extraction.events.end()));
		}

		PhysicsBackendQueryRequest MakeRayQuery(const PhysicsWorldHandle world) {
			PhysicsBackendQueryRequest request;
			request.world = world;
			request.kind = PhysicsQueryKind::RayCast;
			request.pose.position = {-8.0F, 0.0F, 0.0F};
			request.direction = {1.0F, 0.0F, 0.0F};
			request.distance = 24.0F;
			request.maximumHits = 1U;
			request.filter.bodyTypes = PhysicsQueryBodyTypeMask::Static;
			return request;
		}
	}

	void RegisterPhysicsProviderQueryEventTests(TestRegistry& registry) {
		registry.Add("Physics.Provider.Jolt56.AllQueryKindsFiltersAndBounds", [](TestContext& context) {
			BackendFixture fixture;
			context.Expect(fixture.Initialize(), "The direct Jolt 5.6 provider fixture must initialize.");
			const PhysicsShapeHandle solidShape = fixture.PublishBox({1.0F, 1.0F, 1.0F});
			const PhysicsShapeHandle sensorShape = fixture.PublishBox({1.0F, 1.0F, 1.0F}, true);
			context.Expect(solidShape.IsValid() && sensorShape.IsValid(), "The query fixture must publish its bounded box shapes.");

			TerraEngine::UUID nearestId{};
			TerraEngine::UUID middleId{};
			TerraEngine::UUID farId{};
			TerraEngine::UUID sensorId{};
			const PhysicsBodyHandle nearest = fixture.PublishBody(solidShape, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F}, nearestId, 1U);
			const PhysicsBodyHandle middle = fixture.PublishBody(solidShape, PhysicsMotionType::Static, {3.0F, 0.0F, 0.0F}, middleId, 1U);
			const PhysicsBodyHandle far = fixture.PublishBody(solidShape, PhysicsMotionType::Static, {6.0F, 0.0F, 0.0F}, farId, 2U);
			const PhysicsBodyHandle sensor = fixture.PublishBody(sensorShape, PhysicsMotionType::Static, {9.0F, 0.0F, 0.0F}, sensorId, 3U, true);
			context.Expect(nearest.IsValid() && middle.IsValid() && far.IsValid() && sensor.IsValid(),
				"The query fixture must publish all solid and sensor bodies before querying.");

			PhysicsBackendQueryRequest request = MakeRayQuery(fixture.world);
			request.hitMode = PhysicsQueryHitMode::Any;
			const PhysicsBackendQueryResult any = fixture.backend.Query(request);
			context.Expect(any.result.Succeeded() && any.hits.size() == 1U,
				"Jolt Any ray casts must return one bounded provider hit.");

			request.hitMode = PhysicsQueryHitMode::Closest;
			const PhysicsBackendQueryResult closest = fixture.backend.Query(request);
			context.Expect(closest.result.Succeeded() && closest.hits.size() == 1U && closest.hits.front().body == nearest,
				"Jolt Closest ray casts must retain the nearest engine body identity.");

			request.hitMode = PhysicsQueryHitMode::All;
			request.maximumHits = 2U;
			const PhysicsBackendQueryResult all = fixture.backend.Query(request);
			context.Expect(all.result.Succeeded() && all.hits.size() == 2U && all.truncated,
				"Jolt All ray casts must retain only the calibrated caller bound and diagnose truncation.");
			context.Expect(all.hits[0].body == nearest && all.hits[1].body == middle
				&& all.hits[0].distance <= all.hits[1].distance,
				"Bounded All hits must have stable nearest-first engine ordering independent of Jolt callback arrival.");

			request.maximumHits = 1U;
			request.filter.collisionMask = (1U << 2U);
			const PhysicsBackendQueryResult layerFiltered = fixture.backend.Query(request);
			context.Expect(layerFiltered.result.Succeeded() && layerFiltered.hits.size() == 1U
				&& layerFiltered.hits.front().body == far,
				"The provider body filter must honor engine collision-layer masks.");
			request.filter.collisionMask = (1U << 1U);
			request.filter.excludedBody = nearest;
			const PhysicsBackendQueryResult excluded = fixture.backend.Query(request);
			context.Expect(excluded.result.Succeeded() && excluded.hits.size() == 1U && excluded.hits.front().body == middle,
				"The provider body filter must exclude an exact engine body handle.");
			request.filter = {};
			request.filter.collisionMask = (1U << 3U);
			request.filter.bodyTypes = PhysicsQueryBodyTypeMask::Static;
			const PhysicsBackendQueryResult hiddenSensor = fixture.backend.Query(request);
			context.Expect(hiddenSensor.result.Succeeded() && hiddenSensor.hits.empty(),
				"Sensor bodies must remain hidden unless an engine query explicitly includes sensors.");
			request.filter.includeSensors = true;
			const PhysicsBackendQueryResult visibleSensor = fixture.backend.Query(request);
			context.Expect(visibleSensor.result.Succeeded() && visibleSensor.hits.size() == 1U
				&& visibleSensor.hits.front().body == sensor && visibleSensor.hits.front().sensor,
				"Sensor inclusion must preserve the provider hit's engine-owned sensor flag.");

			PhysicsBackendQueryRequest shapeCast = MakeRayQuery(fixture.world);
			shapeCast.kind = PhysicsQueryKind::ShapeCast;
			shapeCast.hitMode = PhysicsQueryHitMode::Closest;
			shapeCast.shapeExtent = {0.25F, 0.25F, 0.25F};
			const PhysicsBackendQueryResult cast = fixture.backend.Query(shapeCast);
			context.Expect(cast.result.Succeeded() && cast.hits.size() == 1U && cast.hits.front().body == nearest,
				"Jolt shape casts must return the closest collision with provider-neutral identity and position.");

			PhysicsBackendQueryRequest overlap = MakeRayQuery(fixture.world);
			overlap.kind = PhysicsQueryKind::Overlap;
			overlap.hitMode = PhysicsQueryHitMode::All;
			overlap.pose.position = {0.0F, 0.0F, 0.0F};
			overlap.shapeExtent = {0.25F, 0.25F, 0.25F};
			overlap.maximumHits = 2U;
			const PhysicsBackendQueryResult overlapping = fixture.backend.Query(overlap);
			context.Expect(overlapping.result.Succeeded() && !overlapping.hits.empty()
				&& overlapping.hits.front().body == nearest,
				"Jolt overlaps must report bodies that intersect the engine-owned query shape.");

			PhysicsBackendQueryRequest point = MakeRayQuery(fixture.world);
			point.kind = PhysicsQueryKind::Point;
			point.hitMode = PhysicsQueryHitMode::All;
			point.pose.position = {3.0F, 0.0F, 0.0F};
			point.maximumHits = 2U;
			const PhysicsBackendQueryResult contained = fixture.backend.Query(point);
			context.Expect(contained.result.Succeeded() && !contained.hits.empty()
				&& contained.hits.front().body == middle,
				"Jolt point queries must identify an engine body containing the requested point.");
		});

		registry.Add("Physics.Provider.Jolt56.CallbackBridgePublishesCollisionTriggerAndActivationEvents", [](TestContext& context) {
			BackendFixture fixture;
			context.Expect(fixture.Initialize(), "The direct Jolt 5.6 callback fixture must initialize.");
			const PhysicsShapeHandle floorShape = fixture.PublishBox({20.0F, 1.0F, 20.0F});
			const PhysicsShapeHandle bodyShape = fixture.PublishBox({1.0F, 1.0F, 1.0F});
			const PhysicsShapeHandle triggerShape = fixture.PublishBox({4.0F, 4.0F, 4.0F}, true);
			context.Expect(floorShape.IsValid() && bodyShape.IsValid() && triggerShape.IsValid(),
				"The callback fixture must publish physical and sensor shapes.");

			TerraEngine::UUID floorId{};
			TerraEngine::UUID fallingId{};
			TerraEngine::UUID triggerId{};
			TerraEngine::UUID triggerBodyId{};
			const PhysicsBodyHandle floor = fixture.PublishBody(floorShape, PhysicsMotionType::Static, {0.0F, -0.5F, 0.0F}, floorId);
			const PhysicsBodyHandle falling = fixture.PublishBody(bodyShape, PhysicsMotionType::Dynamic, {0.0F, 3.0F, 0.0F}, fallingId);
			const PhysicsBodyHandle trigger = fixture.PublishBody(triggerShape, PhysicsMotionType::Static, {10.0F, 0.0F, 0.0F}, triggerId, 0U, true);
			const PhysicsBodyHandle triggerBody = fixture.PublishBody(bodyShape, PhysicsMotionType::Dynamic, {10.0F, 0.0F, 0.0F}, triggerBodyId, 0U, false, false);
			context.Expect(floor.IsValid() && falling.IsValid() && trigger.IsValid() && triggerBody.IsValid(),
				"The callback fixture must create real Jolt physical and trigger pairs.");

			std::vector<PhysicsEvent> events;
			for(PhysicsTick tick = 1U; tick <= 2U; ++tick) {
				bool stepped = false;
				AppendEvents(events, fixture.StepAndExtract(tick, stepped));
				context.Expect(stepped, "The initial trigger overlap must execute through Jolt Step and Extract.");
			}
			context.Expect(HasEvent(events, PhysicsEventType::TriggerEnter) && HasEvent(events, PhysicsEventType::TriggerStay),
				"A persistent Jolt sensor overlap must publish deterministic trigger enter and stay records.");

			PhysicsBodyCommandTarget triggerTarget{fixture.world, triggerBodyId, 1U, 1U};
			PhysicsPose outsideTrigger;
			outsideTrigger.position = {20.0F, 0.0F, 0.0F};
			context.Expect(fixture.backend.ApplyCommand(PhysicsCommand::MakeTeleport(triggerTarget, outsideTrigger)).Succeeded(),
				"A typed teleport must move the trigger body out through the protected backend boundary.");
			bool stepped = false;
			AppendEvents(events, fixture.StepAndExtract(3U, stepped));
			context.Expect(stepped && HasEvent(events, PhysicsEventType::TriggerExit),
				"Leaving the real Jolt sensor must publish a trigger exit record after the next extraction.");

			for(PhysicsTick tick = 4U; tick <= 150U; ++tick) {
				AppendEvents(events, fixture.StepAndExtract(tick, stepped));
				context.Expect(stepped, "The falling physical body must remain step/extract healthy while reaching its support.");
			}
			context.Expect(HasEvent(events, PhysicsEventType::CollisionEnter) && HasEvent(events, PhysicsEventType::CollisionStay)
				&& HasEvent(events, PhysicsEventType::Contact),
				"Jolt physical contacts must bridge collision enter/stay and contact-detail packets.");

			PhysicsBodyCommandTarget fallingTarget{fixture.world, fallingId, 1U, 1U};
			PhysicsBodyCommandTarget staleObjectTarget = fallingTarget;
			staleObjectTarget.objectGeneration = 2U;
			context.Expect(!fixture.backend.ApplyCommand(PhysicsCommand::MakeWake(staleObjectTarget)).Succeeded(),
				"A delayed command with a stale object generation must not reach the recycled provider body.");
			context.Expect(fixture.backend.ApplyCommand(PhysicsCommand::MakeWake(fallingTarget)).Succeeded(),
				"An explicit wake command must be accepted for the settled dynamic Jolt body.");
			AppendEvents(events, fixture.StepAndExtract(151U, stepped));
			context.Expect(stepped && HasEvent(events, PhysicsEventType::Activated),
				"Jolt activation callbacks must become an engine Activated event at the extraction boundary.");

			context.Expect(fixture.backend.ApplyCommand(PhysicsCommand::MakeSleep(fallingTarget)).Succeeded(),
				"An explicit sleep command must be accepted for the dynamic Jolt body.");
			AppendEvents(events, fixture.StepAndExtract(152U, stepped));
			context.Expect(stepped && HasEvent(events, PhysicsEventType::Slept),
				"Jolt deactivation callbacks must become an engine Slept event at the extraction boundary.");

			PhysicsPose aboveFloor;
			aboveFloor.position = {0.0F, 10.0F, 0.0F};
			context.Expect(fixture.backend.ApplyCommand(PhysicsCommand::MakeTeleport(fallingTarget, aboveFloor)).Succeeded(),
				"A physical body must accept a typed teleport away from its support.");
			AppendEvents(events, fixture.StepAndExtract(153U, stepped));
			context.Expect(stepped && HasEvent(events, PhysicsEventType::CollisionExit),
				"Removing a real physical overlap must publish collision exit after Jolt callbacks join.");
		});

		registry.Add("Physics.Provider.Jolt56.QueryResultsRemainOwnedAfterProviderMutation", [](TestContext& context) {
			BackendFixture fixture;
			context.Expect(fixture.Initialize(), "The copied-query fixture must initialize its isolated Jolt world.");
			const PhysicsShapeHandle shape = fixture.PublishBox({1.0F, 1.0F, 1.0F});
			TerraEngine::UUID objectId{};
			const PhysicsBodyHandle body = fixture.PublishBody(shape, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F}, objectId);
			const PhysicsBackendQueryResult copiedResult = fixture.backend.Query(MakeRayQuery(fixture.world));
			context.Expect(copiedResult.result.Succeeded() && copiedResult.hits.size() == 1U,
				"The provider must produce one engine-owned hit before its body is removed.");
			context.Expect(fixture.backend.DestroyBody(body).Succeeded(),
				"The provider fixture must remove the queried body through its protected ownership boundary.");
			context.Expect(copiedResult.hits.front().body == body && copiedResult.hits.front().objectId == objectId,
				"A completed query must keep copied body and object identities after the provider has released the source body.");
			const PhysicsBackendQueryResult afterRemoval = fixture.backend.Query(MakeRayQuery(fixture.world));
			context.Expect(afterRemoval.result.Succeeded() && afterRemoval.hits.empty(),
				"A later provider query must observe removal without mutating an already returned immutable query result.");
		});
	}
}
