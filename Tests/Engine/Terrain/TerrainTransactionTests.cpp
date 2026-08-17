// TerrainTransactionTests.cpp -- Atomic terrain root publication regression tests.

#include "TestHarness.h"
#include "Terrain/TerrainPublication.h"

namespace TerraEngine::Tests
{
namespace
{
	std::shared_ptr<const Terrain::TerrainAssetRoot> MakeRoot(Terrain::TerrainAreaDescriptor& outDescriptor)
	{
		auto root = std::make_shared<Terrain::TerrainAssetRoot>(); root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1u; root->persistedRootGeneration = 1u; root->rootContentHash = {1u, 2u}; root->authoredBounds = {-2, -2, 2, 2}; root->layerStackGeneration = 1u;
		outDescriptor.coordinate = {0, 0}; outDescriptor.editGeneration = 1u; outDescriptor.authoredContentHash = {3u, 4u}; Terrain::TerrainDirectoryShard shard{}; shard.areaBounds = {-2, -2, 2, 2}; shard.descriptors.emplace(outDescriptor.coordinate, outDescriptor); root->directoryShards.push_back(std::move(shard)); return root;
	}

	Terrain::TerrainEditTransaction MakeTransaction(const Terrain::TerrainAssetRoot& root, const Terrain::TerrainAreaDescriptor& before)
	{
		Terrain::TerrainEditTransaction transaction{}; transaction.transactionId = TerraEngine::GenerateUUID(); transaction.targetAssetUuid = root.assetUuid; transaction.inputRootHash = root.rootContentHash; transaction.expectedLayerStackGeneration = root.layerStackGeneration; transaction.affectedBounds = {0, 0, 1, 1}; transaction.readSet.push_back({before.coordinate, before.editGeneration, before.authoredContentHash}); Terrain::TerrainPatch patch{}; patch.replacement = before; patch.replacement.editGeneration = 2u; patch.replacement.authoredContentHash = {5u, 6u}; transaction.patches.push_back(patch); return transaction;
	}
}

	void RegisterTerrainTransactionTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Transaction.PublishesEveryAreaAtOneRootBoundary", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor before{}; Terrain::TerrainAssetStore store(MakeRoot(before)); Terrain::TerrainPublication publication(store); const auto receipt = publication.Publish(MakeTransaction(*store.CaptureRoot().root, before)); const auto root = store.CaptureRoot();
			context.Expect(receipt.outcome == Terrain::TerrainEditOutcome::Committed && root.publicationGeneration == 2u, "A valid typed terrain transaction must publish exactly one new immutable root.");
			context.Expect(root.root->directoryShards.front().descriptors.at(Terrain::TerrainAreaCoordinate{0, 0}).editGeneration == 2u, "The changed area becomes visible only with the new root.");
		});

		registry.Add("Terrain.Transaction.StaleAndInjectedFailuresLeaveRootUntouched", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor before{}; Terrain::TerrainAssetStore store(MakeRoot(before)); Terrain::TerrainPublication publication(store); auto stale = MakeTransaction(*store.CaptureRoot().root, before); stale.readSet.front().editGeneration = 99u; const auto staleReceipt = publication.Publish(stale); const auto generationAfterStale = store.CaptureRoot().publicationGeneration;
			publication.SetInjectFailureForTests(true); const auto failureReceipt = publication.Publish(MakeTransaction(*store.CaptureRoot().root, before));
			context.Expect(staleReceipt.outcome == Terrain::TerrainEditOutcome::Rejected && failureReceipt.outcome == Terrain::TerrainEditOutcome::Rejected, "Conflicting and failed terrain edits should reject.");
			context.Expect(generationAfterStale == 1u && store.CaptureRoot().publicationGeneration == 1u, "Rejected terrain edits must leave zero partial root publication.");
		});
	}
}
