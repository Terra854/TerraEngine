// IncrementalBodyStateStoreTests.cpp - immutable page and same-tick view checks.

#include "../TestHarness.h"

#include "Physics/Backend/IPhysicsBackend.h"
#include "Physics/IncrementalBodyStateStore.h"
#include "Physics/PhysicsWorld.h"

#include <memory>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		constexpr PhysicsWorldHandle kStoreWorld{{44U}, 2U};
		constexpr PhysicsBodyStateFieldMask kStoredFields = PhysicsBodyStateFieldMask::Pose
			| PhysicsBodyStateFieldMask::LinearVelocity
			| PhysicsBodyStateFieldMask::AngularVelocity
			| PhysicsBodyStateFieldMask::Awake;

		PhysicsBodyState MakeState(std::uint32_t slot, float x, bool awake = true) {
			PhysicsBodyState state;
			state.handle = {kStoreWorld.worldId, kStoreWorld.worldGeneration, slot, 1U};
			state.objectId = TerraEngine::GenerateUUID();
			state.objectGeneration = 3U;
			state.componentGeneration = 5U;
			state.worldPose.position.x = x;
			state.awake = awake;
			return state;
		}

		IncrementalBodyStateUpdateBatch FullBatch(PhysicsTick tick, std::vector<PhysicsBodyState> states) {
			IncrementalBodyStateUpdateBatch batch;
			batch.world = kStoreWorld;
			batch.tick = tick;
			batch.providerOrderGeneration = 1U;
			batch.expectedCompleteBodyCount = states.size();
			batch.fullState = true;
			for(PhysicsBodyState& state : states) {
				batch.updates.push_back({std::move(state), {}, kStoredFields,
					PhysicsBodyStateChangeMask::Created, false});
			}
			return batch;
		}
	}

	void RegisterIncrementalBodyStateStoreTests(TestRegistry& registry) {
		registry.Add("Physics.IncrementalBodyStateStore.ImmutablePageReuseAndSnapshotLifetime", [](TestContext& context) {
			IncrementalBodyStateStore store({2U, 32U, 1U * 1024U * 1024U});
			auto first = store.Apply(FullBatch(1U, {
				MakeState(0U, 0.0F), MakeState(1U, 1.0F), MakeState(2U, 2.0F), MakeState(3U, 3.0F)}));
			context.Expect(first.has_value() && first->complete->pages.size() == 2U,
				"The initial complete state must seal two bounded immutable pages.");
			const auto oldSnapshot = first->complete;
			const auto untouchedPage = oldSnapshot->pages[1];
			IncrementalBodyStateUpdateBatch update;
			update.world = kStoreWorld;
			update.tick = 2U;
			update.providerOrderGeneration = 1U;
			PhysicsBodyState changed = *oldSnapshot->Find({kStoreWorld.worldId, kStoreWorld.worldGeneration, 0U, 1U});
			changed.worldPose.position.x = 20.0F;
			update.updates.push_back({changed, {}, PhysicsBodyStateFieldMask::Pose,
				PhysicsBodyStateChangeMask::Updated, false});
			auto second = store.Apply(update);
			context.Expect(second.has_value() && second->pagesCopied == 1U && second->pagesReused == 1U
				&& second->complete->pages[1] == untouchedPage,
				"Only the touched page may be copied; an unrelated immutable page must be reused by identity.");
			context.Expect(oldSnapshot->Find(changed.handle)->worldPose.position.x == 0.0F
				&& second->complete->Find(changed.handle)->worldPose.position.x == 20.0F,
				"An older snapshot lease must retain its original page after later publication.");
		});

		registry.Add("Physics.IncrementalBodyStateStore.ChangedPacketAndCompleteViewShareTick", [](TestContext& context) {
			IncrementalBodyStateStore store({2U, 32U, 1U * 1024U * 1024U});
			auto first = store.Apply(FullBatch(8U, {MakeState(0U, 0.0F, false), MakeState(1U, 1.0F)}));
			IncrementalBodyStateUpdateBatch update;
			update.world = kStoreWorld;
			update.tick = 9U;
			update.providerOrderGeneration = 1U;
			PhysicsBodyState awake = *first->complete->Find({kStoreWorld.worldId, kStoreWorld.worldGeneration, 0U, 1U});
			awake.awake = true;
			update.updates.push_back({awake, {}, PhysicsBodyStateFieldMask::Awake,
				PhysicsBodyStateChangeMask::Awakened, false});
			auto publication = store.Apply(update);
			context.Expect(publication.has_value() && publication->complete->tick == 9U
				&& publication->changed->tick == 9U
				&& publication->complete->storeGeneration == publication->changed->storeGeneration,
				"Complete queries and changed-body consumers must observe one atomic world/tick identity.");
			context.Expect(publication->complete->bodyCount == 2U
				&& publication->changed->changes->size() == 1U
				&& ((*publication->changed->changes)[0].change & PhysicsBodyStateChangeMask::Awakened)
					== PhysicsBodyStateChangeMask::Awakened,
				"Incremental publication must keep complete state while bounding its changed packet.");
		});

		registry.Add("Physics.IncrementalBodyStateStore.RemovalBoundsAndRejectedBatchAtomicity", [](TestContext& context) {
			IncrementalBodyStateStore store({2U, 4U, 1U * 1024U * 1024U});
			auto first = store.Apply(FullBatch(1U, {MakeState(0U, 0.0F), MakeState(1U, 1.0F)}));
			IncrementalBodyStateUpdateBatch invalid;
			invalid.world = kStoreWorld;
			invalid.tick = 2U;
			invalid.providerOrderGeneration = 1U;
			invalid.updates.push_back({MakeState(5U, 5.0F), {}, kStoredFields,
				PhysicsBodyStateChangeMask::Created, false});
			context.Expect(!store.Apply(invalid).has_value() && store.AcquireComplete() == first->complete,
				"An out-of-bound slot must reject atomically without replacing the last complete snapshot.");
			IncrementalBodyStateUpdateBatch removal;
			removal.world = kStoreWorld;
			removal.tick = 2U;
			removal.providerOrderGeneration = 2U;
			removal.updates.push_back({{}, {kStoreWorld.worldId, kStoreWorld.worldGeneration, 1U, 1U},
				PhysicsBodyStateFieldMask::None, PhysicsBodyStateChangeMask::Removed, true});
			auto removed = store.Apply(removal);
			context.Expect(removed.has_value() && removed->complete->bodyCount == 1U
				&& removed->changed->changes->front().removed,
				"A valid removal must update the complete page and changed packet together.");
			context.Expect(store.Statistics().retainedBytes <= 1U * 1024U * 1024U,
				"The retained latest-page storage must remain within the configured hard bound.");
		});

		registry.Add("Physics.IncrementalBodyStateStore.ProviderConversionValidatesEngineOwnership", [](TestContext& context) {
			PhysicsBackendExtraction extraction;
			extraction.world = kStoreWorld;
			extraction.tick = 7U;
			extraction.providerOrderGeneration = 3U;
			extraction.totalBodyCount = 1U;
			extraction.fullState = true;
			PhysicsBackendBodyState provider;
			provider.handle = {kStoreWorld.worldId, kStoreWorld.worldGeneration, 0U, 1U};
			provider.objectId = TerraEngine::GenerateUUID();
			provider.objectGeneration = 2U;
			provider.componentGeneration = 4U;
			provider.fields = kStoredFields;
			provider.pose.position.x = 12.0F;
			extraction.bodies.push_back(provider);
			PhysicsBodyStateValidationScratch scratch;
			auto prepared = PhysicsWorld::ConvertExtractedBodyStates(extraction, scratch);
			context.Expect(prepared.has_value() && prepared->fullState
				&& prepared->updates.size() == 1U
				&& prepared->updates.front().state.worldPose.position.x == 12.0F,
				"Provider records must become validated engine-owned state before protected publication.");
			const std::size_t ownershipCapacity = scratch.ownershipKeys.capacity();
			const std::size_t stableIndexCapacity = scratch.stableIndices.capacity();
			extraction.tick = 8U;
			context.Expect(PhysicsWorld::ConvertExtractedBodyStates(extraction, scratch).has_value()
				&& scratch.ownershipKeys.capacity() == ownershipCapacity
				&& scratch.stableIndices.capacity() == stableIndexCapacity,
				"Repeated provider conversion must reuse contiguous validation storage after warm-up.");
			extraction.bodies.front().componentGeneration = 0U;
			context.Expect(!PhysicsWorld::ConvertExtractedBodyStates(extraction).has_value(),
				"Missing engine ownership generations must reject before any live-state boundary is reached.");
		});
	}
}
