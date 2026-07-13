// PhysicsQueryTests.cpp - contract tests for bounded provider-neutral queries.

#include "../TestHarness.h"

#include "Physics/PhysicsQueries.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Physics;

		constexpr PhysicsWorldHandle kWorld{{77U}, 3U};

		PhysicsQueryRequest MakeRayRequest(PhysicsTick tick = 10U) {
			PhysicsQueryRequest request;
			request.world = kWorld;
			request.kind = PhysicsQueryKind::RayCast;
			request.pose.position = {-10.0F, 0.0F, 0.0F};
			request.direction = {1.0F, 0.0F, 0.0F};
			request.distance = 100.0F;
			request.maximumHits = 8U;
			request.submittedTick = tick;
			request.correlationId = "physics-query-test";
			return request;
		}

		PhysicsQueryHit MakeHit(
			PhysicsSlotIndex slot,
			float distance,
			std::uint8_t layer = 0U,
			PhysicsMotionType motion = PhysicsMotionType::Static,
			bool sensor = false) {
			PhysicsQueryHit hit;
			hit.body = PhysicsBodyHandle{kWorld.worldId, kWorld.worldGeneration, slot, 1U};
			hit.objectId[0] = static_cast<std::uint8_t>(slot + 1U);
			hit.objectGeneration = 1U;
			hit.componentGeneration = 1U;
			hit.position = {distance, 0.0F, 0.0F};
			hit.normal = {-1.0F, 0.0F, 0.0F};
			hit.distance = distance;
			hit.fraction = distance / 100.0F;
			hit.subShapeKey = slot % 2U;
			hit.collisionLayer = layer;
			hit.motionType = motion;
			hit.sensor = sensor;
			return hit;
		}

		PhysicsPreparedQuery PrepareOne(TestContext& context, PhysicsQueryManager& manager, const PhysicsQueryRequest& request, PhysicsTick tick) {
			const PhysicsQueryReceipt receipt = manager.Submit(request, kWorld, tick);
			context.Expect(receipt.accepted, "A calibrated query must receive a generation-safe handle.");
			const PhysicsPreparedQueryBatch batch = manager.Prepare(kWorld, tick);
			context.ExpectEqual(static_cast<std::size_t>(1U), batch.queries.size(), "One submitted query must prepare once.");
			return batch.queries.front();
		}
	}

	void RegisterPhysicsQueryTests(TestRegistry& registry) {
		registry.Add("Physics.Queries.CanonicalHitsRemainStableAcrossOneHundredIdenticalRuns", [](TestContext& context) {
			std::vector<PhysicsSlotIndex> expectedOrder;
			for(std::uint32_t run = 0U; run < 100U; ++run) {
				PhysicsQueryManager manager;
				const PhysicsPreparedQuery prepared = PrepareOne(context, manager, MakeRayRequest(), 10U);
				// Deliberately unordered candidates model differing provider broad-phase
				// traversal order. A duplicate target keeps only its closest hit.
				std::vector<PhysicsQueryHit> candidates{
					MakeHit(5U, 9.0F), MakeHit(2U, 3.0F), MakeHit(5U, 7.0F), MakeHit(1U, 3.0F)};
				context.Expect(manager.Complete(prepared, kWorld, 10U, PhysicsResultStatus::Success, candidates),
					"Prepared queries must publish canonical immutable results.");
				const PhysicsQueryResult result = manager.Read(prepared.handle, kWorld, 10U);
				context.Expect(result.status == PhysicsResultStatus::Success && result.hits != nullptr,
					"A completed query must be readable within its retention window.");
				std::vector<PhysicsSlotIndex> observed;
				for(const PhysicsQueryHit& hit : *result.hits) observed.push_back(hit.body.slotIndex);
				if(run == 0U) expectedOrder = observed;
				context.Expect(observed == expectedOrder && observed == std::vector<PhysicsSlotIndex>{1U, 2U, 5U},
					"One hundred identical states must publish the same distance-and-identity hit order.");
			}
		});

		registry.Add("Physics.Queries.SupportRayShapeOverlapPointFiltersAndCalibratedBounds", [](TestContext& context) {
			PhysicsQueryManager manager;
			PhysicsQueryRequest ray = MakeRayRequest();
			ray.filter.collisionMask = 1U << 2U;
			ray.filter.bodyTypes = PhysicsQueryBodyTypeMask::Dynamic;
			ray.filter.includeSensors = false;
			const PhysicsPreparedQuery prepared = PrepareOne(context, manager, ray, 10U);
			const PhysicsQueryHit excludedLayer = MakeHit(1U, 1.0F, 1U, PhysicsMotionType::Dynamic);
			const PhysicsQueryHit excludedSensor = MakeHit(2U, 2.0F, 2U, PhysicsMotionType::Dynamic, true);
			const PhysicsQueryHit excludedType = MakeHit(3U, 3.0F, 2U, PhysicsMotionType::Static);
			const PhysicsQueryHit included = MakeHit(4U, 4.0F, 2U, PhysicsMotionType::Dynamic);
			const std::vector<PhysicsQueryHit> candidates{excludedLayer, excludedSensor, excludedType, included};
			context.Expect(manager.Complete(prepared, kWorld, 10U, PhysicsResultStatus::Success, candidates),
				"A filtered ray must complete.");
			const PhysicsQueryResult filtered = manager.Read(prepared.handle, kWorld, 10U);
			context.ExpectEqual(static_cast<std::size_t>(1U), filtered.hits->size(), "Layer, trigger, and body-type filters must remove unrelated hits.");
			context.Expect(filtered.hits->front().body == included.body, "The one matching hit must survive canonical filtering.");

			PhysicsQueryRequest shape = MakeRayRequest();
			shape.kind = PhysicsQueryKind::ShapeCast;
			shape.shapeExtent = {1.0F, 2.0F, 3.0F};
			context.Expect(manager.Submit(shape, kWorld, 10U).accepted, "A finite bounded shape cast must be accepted.");
			PhysicsQueryRequest overlap = shape;
			overlap.kind = PhysicsQueryKind::Overlap;
			overlap.distance = 0.0F;
			context.Expect(manager.Submit(overlap, kWorld, 10U).accepted, "A finite bounded overlap must be accepted.");
			PhysicsQueryRequest point = overlap;
			point.kind = PhysicsQueryKind::Point;
			point.shapeExtent = {};
			context.Expect(manager.Submit(point, kWorld, 10U).accepted, "A point query must not require a synthetic provider shape.");

			PhysicsQueryRequest tooFar = MakeRayRequest();
			tooFar.distance = kPhysicsMaximumCastDistance + 1.0F;
			context.Expect(!manager.Submit(tooFar, kWorld, 10U).accepted, "Calibrated cast-distance bounds must reject oversized rays.");
			PhysicsQueryRequest oversized = shape;
			oversized.shapeExtent.x = kPhysicsMaximumShapeExtent + 1.0F;
			context.Expect(!manager.Submit(oversized, kWorld, 10U).accepted, "Calibrated shape-extent bounds must reject oversized casts.");
		});

		registry.Add("Physics.Queries.CancellationExpiryAndGenerationReuseFailSafely", [](TestContext& context) {
			PhysicsQueryManager manager;
			const PhysicsQueryReceipt cancelled = manager.Submit(MakeRayRequest(20U), kWorld, 20U);
			context.Expect(cancelled.accepted && manager.Cancel(cancelled.handle, kWorld), "Queued query cancellation must succeed before backend execution.");
			context.Expect(manager.Read(cancelled.handle, kWorld, 20U).status == PhysicsResultStatus::Cancelled,
				"Cancelled requests must not be mistaken for empty successful queries.");

			PhysicsQueryRequest expiring = MakeRayRequest(20U);
			expiring.expiryTick = 21U;
			const PhysicsQueryReceipt expiry = manager.Submit(expiring, kWorld, 20U);
			context.Expect(expiry.accepted, "A future expiry must be accepted before its deadline.");
			const PhysicsPreparedQueryBatch expiredBatch = manager.Prepare(kWorld, 21U);
			context.Expect(expiredBatch.queries.empty() && !expiredBatch.rejected.empty(), "An expired queued query must never reach a backend.");
			context.Expect(manager.Read(expiry.handle, kWorld, 21U).status == PhysicsResultStatus::Expired,
				"Expiry must publish an explicit safe terminal result.");
			manager.Expire(kWorld, 21U);
			const PhysicsQueryReceipt replacement = manager.Submit(MakeRayRequest(21U), kWorld, 21U);
			context.Expect(replacement.accepted, "A retired slot must be reusable under bounded storage.");
			context.Expect(manager.Read(expiry.handle, kWorld, 21U).status == PhysicsResultStatus::StaleTarget,
				"An old slot generation must not observe a replacement query result.");
			const PhysicsWorldHandle replacementWorld{kWorld.worldId, kWorld.worldGeneration + 1U};
			context.Expect(!manager.Submit(MakeRayRequest(21U), replacementWorld, 21U).accepted,
				"A request with the old world identity must be rejected before it can affect a recreated world.");
		});

		registry.Add("Physics.Queries.PreparesTenThousandRequestsWithFixedBounds", [](TestContext& context) {
			PhysicsQueryManager manager;
			std::vector<PhysicsQueryRequest> requests(kPhysicsMaximumRequestsPerBatch, MakeRayRequest(100U));
			for(std::size_t index = 0U; index < requests.size(); ++index) {
				requests[index].correlationId = "batch-" + std::to_string(index);
			}
			const PhysicsQueryBatchSubmission submitted = manager.SubmitBatch(requests, kWorld, 100U);
			context.Expect(submitted.accepted && submitted.receipts.size() == kPhysicsMaximumRequestsPerBatch,
				"Exactly ten thousand calibrated requests must be accepted as one bounded batch.");
			const PhysicsPreparedQueryBatch prepared = manager.Prepare(kWorld, 100U);
			context.Expect(prepared.queries.size() == kPhysicsMaximumRequestsPerBatch,
				"The prepared batch must retain every accepted request exactly once.");
			context.Expect(prepared.reservedResultHitCapacity <= static_cast<std::size_t>(kPhysicsMaximumRequestsPerBatch) * kPhysicsMaximumHitsPerQuery,
				"Prepared-result capacity must remain bounded by the calibrated batch and hit limits.");
			const PhysicsQueryDiagnostics diagnostics = manager.GetDiagnostics();
			context.Expect(diagnostics.peakLiveQueryCount <= diagnostics.maximumLiveQueryCount,
				"The manager must report a fixed live-query storage ceiling.");

			requests.push_back(MakeRayRequest(100U));
			const PhysicsQueryBatchSubmission overflow = manager.SubmitBatch(requests, kWorld, 100U);
			context.Expect(!overflow.accepted && overflow.rejectionReason == PhysicsQueryRejectionReason::BatchLimit,
				"A batch larger than ten thousand must fail atomically without per-request allocation.");
		});
	}
}
