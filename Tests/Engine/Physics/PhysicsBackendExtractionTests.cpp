// PhysicsBackendExtractionTests.cpp - provider-neutral bounded extraction checks.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsCommands.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		class ExtractionPoolSession final {
		public:
			ExtractionPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize(4U);
			}

			~ExtractionPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		struct ExtractionFixture final {
			ExtractionPoolSession pool;
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{91U}, 3U};
			PhysicsShapeHandle shape{{91U}, 3U, 0U, 1U};
			std::vector<TerraEngine::UUID> objectIds;

			ExtractionFixture() {
				if(!backend.Initialize().Succeeded()) return;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.maximumBodies = 256U;
				if(!backend.CreateWorld(configuration).Succeeded()) return;
				PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = {1.0F, 1.0F, 1.0F};
				PhysicsBackendStagingToken staging;
				if(!backend.CreateShape(world, shape, recipe, staging).Succeeded()) return;
				(void)backend.CommitStaging(staging);
			}

			~ExtractionFixture() {
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
			}

			PhysicsBodyHandle AddBody(
				std::uint32_t slot,
				PhysicsMotionType motion,
				bool startAwake = true) {
				PhysicsBodyCreateRequest request;
				request.handle = {world.worldId, world.worldGeneration, slot, 1U};
				request.objectId = TerraEngine::GenerateUUID();
				request.objectGeneration = 7U;
				request.componentGeneration = 11U;
				request.shape = shape;
				request.motionType = motion;
				request.useGravity = false;
				request.startAwake = startAwake;
				request.pose.position.x = static_cast<float>(slot);
				PhysicsBackendStagingToken staging;
				if(!backend.CreateBody(request, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded()) return {};
				objectIds.push_back(request.objectId);
				return request.handle;
			}

			PhysicsBackendExtraction Extract(PhysicsBackendExtractionRequest request) {
				PhysicsBackendExtraction output;
				(void)backend.Extract(request, output);
				return output;
			}
		};

		PhysicsBackendExtractionRequest FullRequest(PhysicsWorldHandle world, PhysicsTick tick) {
			PhysicsBackendExtractionRequest request;
			request.world = world;
			request.tick = tick;
			request.selection = PhysicsBackendExtractionSelection::FullState;
			request.fields = PhysicsBodyStateFieldMask::All;
			request.fullState = true;
			return request;
		}
	}

	void RegisterPhysicsBackendExtractionTests(TestRegistry& registry) {
		registry.Add("Physics.BackendExtraction.StableOrderOwnershipAndSelectiveFields", [](TestContext& context) {
			ExtractionFixture fixture;
			const PhysicsBodyHandle fifth = fixture.AddBody(5U, PhysicsMotionType::Static);
			const PhysicsBodyHandle first = fixture.AddBody(1U, PhysicsMotionType::Dynamic);
			const PhysicsBodyHandle third = fixture.AddBody(3U, PhysicsMotionType::Kinematic);
			PhysicsBackendExtractionRequest request = FullRequest(fixture.world, 1U);
			request.fields = PhysicsBodyStateFieldMask::Pose | PhysicsBodyStateFieldMask::Awake;
			PhysicsBackendExtraction output;
			context.Expect(fixture.backend.Extract(request, output).Succeeded(),
				"A valid selective full-state request must succeed.");
			context.Expect(output.bodies.size() == 3U && output.bodies[0].handle == first
				&& output.bodies[1].handle == third && output.bodies[2].handle == fifth,
				"Provider membership order must be deterministic rather than unordered-map iteration order.");
			for(std::uint32_t index = 0U; index < output.bodies.size(); ++index) {
				const PhysicsBackendBodyState& state = output.bodies[index];
				context.Expect(state.stableIndex == index && state.objectId != TerraEngine::EMPTY_UUID
					&& state.objectGeneration == 7U && state.componentGeneration == 11U,
					"Each provider record must carry a direct generation-safe engine ownership index.");
				context.Expect(state.fields == request.fields
					&& state.linearVelocity == PhysicsVector3{} && state.mass == 0.0F,
					"Selective extraction must not claim or populate fields outside the requested mask.");
			}
			const std::uint64_t originalOrder = output.providerOrderGeneration;
			PhysicsBodyUpdateRequest update;
			update.handle = first;
			update.pose.position = {20.0F, 0.0F, 0.0F};
			context.Expect(fixture.backend.UpdateBody(update).Succeeded(), "A body-only update must succeed.");
			output = fixture.Extract(FullRequest(fixture.world, 2U));
			context.Expect(output.providerOrderGeneration == originalOrder,
				"Non-membership changes must retain the provider order generation.");
			context.Expect(fixture.backend.DestroyBody(fifth).Succeeded(), "A committed body must be removable.");
			PhysicsBackendExtraction afterRemoval = fixture.Extract(FullRequest(fixture.world, 3U));
			context.Expect(afterRemoval.providerOrderGeneration > originalOrder
				&& afterRemoval.bodies.size() == 2U && afterRemoval.bodies[0].stableIndex == 0U
				&& afterRemoval.bodies[1].stableIndex == 1U,
				"Membership changes alone must rebuild and densely reindex the deterministic provider order.");
		});

		registry.Add("Physics.BackendExtraction.BoundedRangesFullStateAndStaticRetention", [](TestContext& context) {
			ExtractionFixture fixture;
			fixture.AddBody(8U, PhysicsMotionType::Static);
			fixture.AddBody(2U, PhysicsMotionType::Static);
			fixture.AddBody(5U, PhysicsMotionType::Dynamic, false);
			PhysicsBackendExtractionRequest publication;
			publication.world = fixture.world;
			publication.tick = 1U;
			publication.selection = PhysicsBackendExtractionSelection::PublicationRelevant;
			publication.fields = PhysicsBodyStateFieldMask::Pose | PhysicsBodyStateFieldMask::Awake;
			PhysicsBackendExtraction first = fixture.Extract(publication);
			context.Expect(first.bodies.size() == 3U,
				"New static and sleeping bodies must enter the first incremental publication.");
			publication.tick = 2U;
			PhysicsBackendExtraction unchanged = fixture.Extract(publication);
			context.Expect(unchanged.bodies.empty(),
				"Unchanged static and sleeping records must remain retained without recurring provider reads.");

			PhysicsBackendExtractionRequest range;
			range.world = fixture.world;
			range.tick = 2U;
			range.selection = PhysicsBackendExtractionSelection::StableRanges;
			range.fields = PhysicsBodyStateFieldMask::Pose;
			range.stableRanges = {{1U, 1U}};
			range.batchSize = 1U;
			PhysicsBackendExtraction bounded = fixture.Extract(range);
			context.Expect(bounded.bodies.size() == 1U && bounded.bodies.front().stableIndex == 1U
				&& bounded.extractionRangeCount == 1U,
				"Stable ranges must return only their bounded deterministic provider-order interval.");

			PhysicsBackendExtraction complete = fixture.Extract(FullRequest(fixture.world, 3U));
			context.Expect(complete.fullState && complete.bodies.size() == complete.totalBodyCount
				&& complete.bodies.size() == 3U,
				"Explicit full-state mode must report every retained live record exactly once.");
		});

		registry.Add("Physics.BackendExtraction.SleepWakeAndExclusiveEventDrain", [](TestContext& context) {
			ExtractionFixture fixture;
			const PhysicsBodyHandle body = fixture.AddBody(1U, PhysicsMotionType::Dynamic);
			const TerraEngine::UUID objectId = fixture.objectIds.front();
			const PhysicsBodyCommandTarget target{fixture.world, objectId, 7U, 11U};
			PhysicsCommand sleep = PhysicsCommand::MakeSleep(target);
			context.Expect(fixture.backend.ApplyCommand(sleep).Succeeded(), "The fixture body must accept sleep.");
			PhysicsBackendExtractionRequest explicitRequest;
			explicitRequest.world = fixture.world;
			explicitRequest.tick = 4U;
			explicitRequest.selection = PhysicsBackendExtractionSelection::ExplicitHandles;
			explicitRequest.explicitHandles = {body};
			explicitRequest.fields = PhysicsBodyStateFieldMask::Awake | PhysicsBodyStateFieldMask::Events;
			PhysicsBackendExtraction sleeping = fixture.Extract(explicitRequest);
			context.Expect(sleeping.bodies.size() == 1U && !sleeping.bodies.front().awake
				&& sleeping.eventDrainPerformed,
				"An explicit sleeping state stays queryable and owns the first event drain for its tick.");
			PhysicsBackendExtraction duplicateDrain = fixture.Extract(explicitRequest);
			context.Expect(!duplicateDrain.eventDrainPerformed && duplicateDrain.events.empty(),
				"A second extraction cannot drain provider events again for the same world tick.");
			context.Expect(fixture.backend.ApplyCommand(PhysicsCommand::MakeWake(target)).Succeeded(),
				"The sleeping fixture body must accept wake.");
			explicitRequest.tick = 5U;
			PhysicsBackendExtraction awake = fixture.Extract(explicitRequest);
			context.Expect(awake.bodies.size() == 1U && awake.bodies.front().awake,
				"An awakened body must immediately re-enter explicit extraction.");
		});

		registry.Add("Physics.BackendExtraction.SafeParallelReadsAndSerialFallback", [](TestContext& context) {
			ExtractionFixture fixture;
			for(std::uint32_t slot = 0U; slot < 32U; ++slot) fixture.AddBody(slot + 10U, PhysicsMotionType::Static);
			PhysicsBackendExtractionRequest request = FullRequest(fixture.world, 1U);
			request.fields = PhysicsBodyStateFieldMask::Pose;
			request.batchSize = 2U;
			request.parallelThreshold = 1U;
			PhysicsBackendExtraction parallel = fixture.Extract(request);
			context.Expect(fixture.backend.ExtractionCapabilities().independentBodyReadsSafe
				&& parallel.usedParallelReads && parallel.extractionRangeCount == 16U,
				"Advertised-safe independent reads must use bounded shared-pool lanes for large work.");
			request.tick = 2U;
			request.allowParallelReads = false;
			PhysicsBackendExtraction serial = fixture.Extract(request);
			context.Expect(serial.usedSerialFallback && !serial.usedParallelReads
				&& serial.bodies.size() == parallel.bodies.size(),
				"Disabling safe parallel reads must retain a complete deterministic serial fallback.");
			context.Expect(std::equal(parallel.bodies.begin(), parallel.bodies.end(), serial.bodies.begin(),
				[](const auto& left, const auto& right) { return left.handle == right.handle; }),
				"Worker-lane selection must never change provider publication order.");
		});
	}
}
