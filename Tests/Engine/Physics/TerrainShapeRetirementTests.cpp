// Actual protected terrain replacement + Jolt ownership, without RSS or fake receipts.
#include "TestHarness.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsReviewTestAccess.h"
#include "Physics/Terrain/TerrainCollisionSeamPreparation.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Physics;
		struct TerrainOwnershipFixture
		{
			PhysicsSystem& system = PhysicsSystem::Instance();
			std::size_t previousWorkers = ThreadPool::instance().getThreadCount();
			void Initialize(TestContext& context)
			{
				system.Shutdown(); ObjectManager::instance().clear(); TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
				ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(2u);
				ComponentRegistry::RegisterAllComponents(); system.SetEditorLifecycleControlled(true); system.Initialize();
				context.Expect(system.StartWorld(), "The ownership fixture must start the real primary Jolt world.");
			}
			TerrainShapeOwnershipSnapshot Observe(TestContext& context)
			{
				const auto snapshot = PhysicsReviewTestAccess::ReadTerrainShapeOwnership(system);
				context.Expect(snapshot.has_value(), "Read-only ownership must come from the actual active Jolt backend and typed world.");
				return *snapshot;
			}
			TerrainHeightFieldRecipe Recipe(TestContext& context)
			{
				TerrainHeightFieldRecipe recipe{};
				recipe.terrainAssetId = TerraEngine::GenerateUUID(); recipe.authoredGeneration = recipe.authoredHash = 1u;
				recipe.heightMillimetres.assign(recipe.kSampleCount, 0); recipe.holeBits.assign(recipe.kHoleByteCount, 0u);
				context.Expect(TerraEngine::Physics::Terrain::CookTerrainCollisionRecipe(recipe), "The immutable terrain must be cooked by the real provider before publication.");
				return recipe;
			}
			TerrainCollisionReplacementReceipt Commit(const TerrainHeightFieldRecipe& recipe, std::uint64_t generation,
				bool cancelled = false)
			{
				// This fixture is the declared protected lifecycle owner; it submits the
				// actual typed terrain packet, never a test backend/receipt callback.
				TerrainCollisionReplacementPacket packet{};
				packet.id = "terrain-ownership-" + std::to_string(generation);
				packet.recipe = recipe; packet.sourceRootHash = {generation + 101u, 211u};
				packet.sourcePublicationGeneration = generation; packet.cancelled = cancelled;
				return system.CommitTerrainCollisionReplacement(std::move(packet));
			}
			~TerrainOwnershipFixture()
			{
				(void)system.StopWorld(); system.Shutdown(); system.SetEditorLifecycleControlled(false);
				TerraEngine::Scene::SceneMutationQueue::Instance().Clear(); ObjectManager::instance().clear();
				ThreadPool::instance().shutdown(); if(previousWorkers != 0u) ThreadPool::instance().initialize(previousWorkers);
			}
		};
		bool SameLiveOwnership(const TerrainShapeOwnershipSnapshot& first, const TerrainShapeOwnershipSnapshot& second)
		{
			// Capacity may reserve a temporary fresh slot before a rejected candidate
			// rolls back; the live/staged ownership must remain exactly unchanged.
			return first.world == second.world && first.typedLiveShapes == second.typedLiveShapes
				&& first.typedLiveBodies == second.typedLiveBodies && first.providerShapeRecords == second.providerShapeRecords
				&& first.providerBodyRecords == second.providerBodyRecords
				&& first.stagedShapeRecords == second.stagedShapeRecords && first.stagedBodyRecords == second.stagedBodyRecords
				&& first.providerStrongCacheEntries == second.providerStrongCacheEntries
				&& first.activeTerrainPublications == second.activeTerrainPublications;
		}
	}

	void RegisterTerrainShapeRetirementTests(TestRegistry& registry)
	{
		registry.Add("Physics.TerrainShapeRetirement.RepeatedReplacementRetiresNativeAndTypedOwners", [](TestContext& context) {
			TerrainOwnershipFixture fixture; fixture.Initialize(context);
			const auto unrelated = fixture.Recipe(context), target = fixture.Recipe(context);
			context.Expect(fixture.Commit(unrelated, 1u).committed, "An unrelated real terrain owner must exist before replacement pressure.");
			const auto baseline = fixture.Observe(context);
			std::vector<TerrainShapeOwnershipSnapshot> retainedCounts;
			TerrainCollisionReplacementReceipt previous{};
			for(std::uint64_t generation = 1u; generation <= 32u; ++generation) {
				const auto receipt = fixture.Commit(target, generation);
				context.Expect(receipt.committed, "Every bounded replacement must commit its actual Jolt body/shape.");
				if(generation > 1u) context.Expect(receipt.activeBody != previous.activeBody && receipt.activeShape != previous.activeShape,
					"Each replacement must actually allocate a fresh generation-qualified body and shape before retiring the old ones.");
				previous = receipt;
				const auto publications = fixture.system.CopyTerrainCollisionPublications();
				const auto current = std::find_if(publications.begin(), publications.end(), [&](const auto& item) { return item.terrainAssetId == target.terrainAssetId; });
				context.Expect(publications.size() == baseline.activeTerrainPublications + 1u && current != publications.end()
					&& current->body == receipt.activeBody && current->shape == receipt.activeShape,
					"Actual provider commits must retain exactly one current publication for the replaced area.");
				retainedCounts.push_back(fixture.Observe(context));
			}
			context.Expect(std::all_of(retainedCounts.begin(), retainedCounts.end(), [&](const auto& count) {
				return count.typedLiveShapes == baseline.typedLiveShapes + 1u && count.typedLiveBodies == baseline.typedLiveBodies + 1u
					&& count.providerShapeRecords == baseline.providerShapeRecords + 1u && count.providerBodyRecords == baseline.providerBodyRecords + 1u
					&& count.stagedShapeRecords == 0u && count.stagedBodyRecords == 0u
					&& count.providerStrongCacheEntries == baseline.providerStrongCacheEntries
					&& count.typedShapeCapacity <= baseline.typedShapeCapacity + 2u;
			}), "After each actual old-body retirement, only one current target shape may remain. finalTypedShapes="
				+ std::to_string(retainedCounts.back().typedLiveShapes) + " finalProviderShapes=" + std::to_string(retainedCounts.back().providerShapeRecords));
			context.Expect(fixture.system.RemoveTerrainCollisions(target.terrainAssetId) && SameLiveOwnership(baseline, fixture.Observe(context)),
				"Removing the final target must retire its remaining owner without touching unrelated terrain.");
		});
		registry.Add("Physics.TerrainShapeRetirement.RejectedCandidatesPreserveLastKnownGoodOwners", [](TestContext& context) {
			TerrainOwnershipFixture fixture; fixture.Initialize(context);
			const auto unrelated = fixture.Recipe(context), target = fixture.Recipe(context);
			context.Expect(fixture.Commit(unrelated, 1u).committed, "The unrelated native control must commit.");
			const auto unrelatedBaseline = fixture.Observe(context);
			const auto good = fixture.Commit(target, 1u);
			context.Expect(good.committed, "The last-known-good target must be a real committed native owner.");
			const auto baseline = fixture.Observe(context);
			const auto cancelled = fixture.Commit(target, 2u, true);
			context.Expect(!cancelled.committed && cancelled.retainedLastKnownGood && SameLiveOwnership(baseline, fixture.Observe(context)),
				"Cancellation before provider staging must retain both current ownership and unrelated native bodies.");
			auto malformed = target; malformed.holeBits.clear();
			const auto rejected = fixture.Commit(malformed, 3u);
			const auto publications = fixture.system.CopyTerrainCollisionPublications();
			context.Expect(!rejected.committed && rejected.retainedLastKnownGood && SameLiveOwnership(baseline, fixture.Observe(context))
				&& std::any_of(publications.begin(), publications.end(), [&](const auto& value) {
					return value.terrainAssetId == target.terrainAssetId && value.body == good.activeBody && value.shape == good.activeShape;
				}), "Invalid detached geometry must leave the actual current body/shape publication and native counts unchanged.");
			context.Expect(fixture.system.RemoveTerrainCollisions(target.terrainAssetId) && SameLiveOwnership(unrelatedBaseline, fixture.Observe(context)),
				"Target removal after rejection must preserve the unrelated owner exactly.");
			context.Expect(fixture.Commit(target, 4u).committed, "Later valid publication must still succeed after rejected candidates.");
		});
		registry.Add("Physics.TerrainShapeRetirement.WorldRestartClearsAllNativeAndTypedTerrainOwners", [](TestContext& context) {
			TerrainOwnershipFixture fixture; fixture.Initialize(context);
			const auto target = fixture.Recipe(context);
			context.Expect(fixture.Commit(target, 1u).committed && fixture.Commit(target, 2u).committed,
				"The lifecycle control must own actual replaced native terrain shapes before teardown.");
			const auto oldWorld = fixture.system.WorldHandle();
			context.Expect(fixture.system.StopWorld() && fixture.system.StartWorld(), "The actual primary provider world must stop and restart normally.");
			const auto clean = fixture.Observe(context);
			context.Expect(clean.world != oldWorld && clean.typedLiveShapes == 0u && clean.typedLiveBodies == 0u
				&& clean.providerShapeRecords == 0u && clean.providerBodyRecords == 0u && clean.providerStrongCacheEntries == 0u
				&& clean.activeTerrainPublications == 0u && clean.stagedShapeRecords == 0u && clean.stagedBodyRecords == 0u,
				"World retirement must clear old ownership and reserve a distinct world generation without relying on process RSS.");
		});
	}
}
