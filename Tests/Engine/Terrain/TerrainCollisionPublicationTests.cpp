// TerrainCollisionPublicationTests.cpp -- stale/failure/LKG collision publication coverage.

#include "TestHarness.h"

#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsTransactions.h"
#include "Core/Scheduling/JobGraph.h"
#include "ThreadPool/ThreadPool.h"

#include <atomic>
#include <memory>

namespace TerraEngine::Tests
{
	void RegisterTerrainCollisionPublicationTests(TestRegistry& registry)
	{
		registry.Add("Terrain.CollisionPublication.RejectsStaleAndRetainsLastKnownGood", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 4u; root->rootContentHash = {8u, 8u}; root->authoredBounds = {0, 0, 1, 1};
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = root->assetUuid; recipe.authoredGeneration = 4u; recipe.authoredHash = 99u;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			auto candidate = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(recipe, root->rootContentHash, 4u);
			TerraEngine::Physics::Terrain::TerrainCollisionSnapshot snapshot{};
			const bool current = TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, candidate, store);
			candidate.sourcePublicationGeneration = 5u;
			const bool stale = TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, candidate, store);
			context.Expect(current && !stale && snapshot.currentRecipe.IsComplete() && snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Stale,
				"A stale worker collision candidate must be rejected without discarding the prior current heightfield recipe.");
		});

		registry.Add("Terrain.CollisionPublication.StagesFreshStaticBodyAndRetainsPriorOnRejection", [](TestContext& context) {
			// This exercises the same provider-neutral packet used by PhysicsSystem's
			// Before Physics boundary, including real Jolt heightfield material mapping.
			ThreadPool::instance().shutdown(); ThreadPool::instance().initialize();
			TerraEngine::Physics::Backend::JoltPhysicsBackend backend{ThreadPool::instance()};
			const TerraEngine::Physics::PhysicsWorldHandle world{{712u}, 1u};
			TerraEngine::Physics::Backend::PhysicsBackendWorldConfiguration configuration; configuration.world = world;
			const bool ready = backend.Initialize().Succeeded() && backend.CreateWorld(configuration).Succeeded();
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{}; recipe.terrainAssetId = TerraEngine::GenerateUUID(); recipe.authoredGeneration = 3u; recipe.authoredHash = 77u; recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0); recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u); recipe.materialIndices.assign(256u * 256u, 9u);
			TerraEngine::Physics::TerrainCollisionReplacementPacket packet; packet.id = "terrain-replacement"; packet.sourceRootHash = {12u, 13u}; packet.sourcePublicationGeneration = 3u; packet.expectedWorld = world; packet.recipe = recipe; packet.replacementShape = {world.worldId, world.worldGeneration, 0u, 1u}; packet.replacementBody.handle = {world.worldId, world.worldGeneration, 0u, 1u}; packet.replacementBody.shape = packet.replacementShape; packet.replacementBody.objectId = recipe.terrainAssetId; packet.replacementBody.objectGeneration = 3u; packet.replacementBody.componentGeneration = 1u; packet.replacementBody.motionType = TerraEngine::Physics::PhysicsMotionType::Static;
			const auto committed = ready ? TerraEngine::Physics::CommitTerrainCollisionReplacement(backend, packet) : TerraEngine::Physics::TerrainCollisionReplacementReceipt{};
			packet.replacementBody.motionType = TerraEngine::Physics::PhysicsMotionType::Dynamic;
			const auto rejected = ready ? TerraEngine::Physics::CommitTerrainCollisionReplacement(backend, packet) : TerraEngine::Physics::TerrainCollisionReplacementReceipt{};
			context.Expect(ready && committed.committed && committed.activeBody == packet.replacementBody.handle && !rejected.committed && rejected.retainedLastKnownGood,
				"A terrain collision packet must stage a fresh static material-aware heightfield body, while a rejected replacement leaves the prior committed collision authoritative.");
			if(committed.committed) { (void)backend.DestroyBody(committed.activeBody); (void)backend.DestroyShape(committed.activeShape); }
			if(ready) (void)backend.DestroyWorld(world); (void)backend.Shutdown(); ThreadPool::instance().shutdown();
		});

		registry.Add("Terrain.CollisionPublication.CancelFailureAndBoundaryLeavePriorCollisionVisible", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 6u;
			root->rootContentHash = {18u, 19u};
			root->authoredBounds = {0, 0, 256, 256};
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = root->assetUuid;
			recipe.authoredGeneration = 6u;
			recipe.authoredHash = 101u;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 12);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			const auto current = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(recipe, root->rootContentHash, 6u);
			TerraEngine::Physics::Terrain::TerrainCollisionSnapshot snapshot{};
			context.Expect(TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, current, store),
				"A complete current worker candidate must become visible at the protected before-physics boundary.");

			std::atomic_bool cancelled = true;
			const auto cancelledCandidate = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(recipe, root->rootContentHash, 6u, &cancelled);
			const bool cancelledPublish = TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, cancelledCandidate, store);
			context.Expect(!cancelledPublish && snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Cancelled
				&& snapshot.currentRecipe.authoredHash == recipe.authoredHash,
				"Cancellation before the boundary must retain the complete prior collision recipe.");

			auto incomplete = recipe;
			incomplete.heightMillimetres.pop_back();
			const auto failedCandidate = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(incomplete, root->rootContentHash, 6u);
			const bool failedPublish = TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, failedCandidate, store);
			context.Expect(!failedPublish && snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Failed
				&& snapshot.currentRecipe.authoredHash == recipe.authoredHash,
				"An invalid complete-root candidate must report failure and leave last-known-good collision visible.");

			auto& system = TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance();
			system.Initialize();
			system.SetPreparedCandidate(current);
			const bool boundaryPublished = system.PublishBeforePhysics(store);
			const bool duplicatePublished = system.PublishBeforePhysics(store);
			context.Expect(boundaryPublished && !duplicatePublished && system.Snapshot().status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current,
				"A prepared replacement must publish once at the declared boundary and never leak into visibility twice.");
			system.Shutdown();
		});

		registry.Add("Terrain.CollisionPublication.ConfiguredProtectedWorkloadCommitsProviderReplacement", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 9u;
			root->rootContentHash = {31u, 32u}; root->authoredBounds = {0, 0, 256, 256};
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = root->assetUuid; recipe.authoredGeneration = 9u; recipe.authoredHash = 3132u;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 25);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);

			auto& system = TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance();
			system.Initialize();
			std::size_t commitCount = 0u;
			system.ConfigurePublication(&store, [&](TerraEngine::Physics::TerrainCollisionReplacementPacket packet) {
				++commitCount;
				TerraEngine::Physics::TerrainCollisionReplacementReceipt receipt;
				receipt.committed = packet.recipe.IsComplete();
				return receipt;
			});
			system.SetPreparedCandidate(TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(
				recipe, root->rootContentHash, root->publicationGeneration));
			auto workloads = system.DeclareFrameWorkloads(0.0f);
			TerraEngine::Scheduling::JobGraphContext graphContext;
			if(workloads.size() == 2u && workloads.back().work) workloads.back().work(graphContext);
			const auto snapshot = system.Snapshot();
			context.Expect(commitCount == 1u && snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
				&& snapshot.sourcePublicationGeneration == 9u,
				"The protected collision workload must validate the immutable root and commit exactly one provider replacement before physics visibility.");
			system.ClearPublication(&store);
			system.Shutdown();
		});
	}
}
