// TerrainResizeHistoryTests.cpp -- Exact resize inverse and history ownership checks.

#include "TestHarness.h"
#include "TerrainResizeTestFixtures.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainHistoryCommand.h"
#include "Terrain/TerrainGarbageCollector.h"
#include "Terrain/TerrainPublication.h"

namespace TerraEngine::EditorTests
{
	void RegisterTerrainResizeHistoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainResize.HistoryCarriesExactExpansionInverse", [](TestContext& context) {
			auto root = TerraEngine::Tests::TerrainResizeFixtures::MakeRoot();
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Editor::Terrain::TerrainPreparedResize prepared{};
			prepared.observedRoot = {root, root->publicationGeneration};
			prepared.preview.delta.kind = TerraEngine::Terrain::TerrainResizeKind::Expand;
			prepared.preview.delta.previousBounds = root->authoredBounds;
			prepared.preview.delta.proposedBounds = {-256, 0, 512, 256};
			prepared.preview.delta.expansionPolicy = TerraEngine::Terrain::TerrainExpansionPolicy::EdgeBlend;
			prepared.preview.proposedAreaCount = 3u;
			prepared.preview.proposalFingerprint = {3u, 4u};
			std::string error;
			const auto transaction = TerraEngine::Editor::Terrain::TerrainResizeJobs{}.BuildTransaction(store, prepared, false, error);
			context.Expect(transaction && transaction->historyGroup == "Expand Terrain"
				&& transaction->resizeDelta && transaction->inverseResizeDelta
				&& transaction->inverseResizeDelta->previousBounds == prepared.preview.delta.proposedBounds
				&& transaction->inverseResizeDelta->proposedBounds == root->authoredBounds,
				"One expansion history command must carry exact before/after bounds and its inverse request.");
		});

		registry.Add("Editor.TerrainResize.CropHistoryRestoresRemovedDescriptors", [](TestContext& context) {
			auto mutableRoot = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(
				*TerraEngine::Tests::TerrainResizeFixtures::MakeRoot({0, 0, 512, 256}));
			auto removed = TerraEngine::Tests::TerrainResizeFixtures::MakeDescriptor({1, 0}, 5u);
			mutableRoot->directoryShards.front().descriptors.emplace(removed.coordinate, removed);
			std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> root = mutableRoot;
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Editor::Terrain::TerrainPreparedResize prepared{};
			prepared.observedRoot = {root, root->publicationGeneration};
			prepared.preview.delta.kind = TerraEngine::Terrain::TerrainResizeKind::Crop;
			prepared.preview.delta.previousBounds = root->authoredBounds;
			prepared.preview.delta.proposedBounds = {0, 0, 256, 256};
			prepared.preview.delta.descriptorRemovals = {{1, 0}};
			prepared.preview.proposedAreaCount = 1u;
			prepared.preview.requiresDataLossConfirmation = true;
			prepared.preview.proposalFingerprint = {5u, 6u};
			std::string error;
			TerraEngine::Editor::Terrain::TerrainResizeJobs jobs;
			context.Expect(!jobs.BuildTransaction(store, prepared, false, error),
				"Crop history cannot be constructed before explicit data-loss confirmation.");
			const auto transaction = jobs.BuildTransaction(store, prepared, true, error);
			context.Expect(transaction && transaction->inverseResizeDelta
				&& transaction->inverseResizeDelta->descriptorUpserts.size() == 1u
				&& transaction->inverseResizeDelta->descriptorUpserts.front().authoredContentHash == removed.authoredContentHash,
				"Confirmed crop history must retain the exact removed descriptor for one-step undo.");
		});

		registry.Add("Editor.TerrainResize.CropBlobsRemainPinnedUntilHistoryPruning", [](TestContext& context) {
			auto mutableRoot = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(
				*TerraEngine::Tests::TerrainResizeFixtures::MakeRoot({0, 0, 512, 256}));
			const std::vector<std::uint8_t> removedBytes{0x63u, 0x72u, 0x6fu, 0x70u};
			const auto removedHash = TerraEngine::Terrain::HashTerrainBytes(removedBytes);
			auto removed = TerraEngine::Tests::TerrainResizeFixtures::MakeDescriptor({1, 0}, 5u);
			removed.channels.heightHash = removedHash;
			removed.authoredContentHash = removedHash;
			mutableRoot->directoryShards.front().descriptors.emplace(removed.coordinate, removed);
			const std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> root = mutableRoot;
			TerraEngine::Terrain::TerrainAssetStore store(root);
			context.Expect(store.Blobs().PutVerified(removedHash, removedBytes),
				"The crop history fixture must register its removed immutable blob.");
			TerraEngine::Editor::Terrain::TerrainPreparedResize prepared{};
			prepared.observedRoot = {root, root->publicationGeneration};
			prepared.preview.delta.kind = TerraEngine::Terrain::TerrainResizeKind::Crop;
			prepared.preview.delta.previousBounds = root->authoredBounds;
			prepared.preview.delta.proposedBounds = {0, 0, 256, 256};
			prepared.preview.delta.descriptorRemovals = {{1, 0}};
			prepared.preview.delta.retainedBlobHashes = {removedHash};
			const TerraEngine::Terrain::TerrainAreaCoordinate originCoordinate{0, 0};
			const auto& origin = root->directoryShards.front().descriptors.at(originCoordinate);
			prepared.preview.observations.push_back({originCoordinate, origin.editGeneration, origin.authoredContentHash});
			prepared.preview.observations.push_back({removed.coordinate, removed.editGeneration, removed.authoredContentHash});
			prepared.preview.proposedAreaCount = 1u;
			prepared.preview.requiresDataLossConfirmation = true;
			prepared.preview.proposalFingerprint = {7u, 8u};
			std::string error;
			TerraEngine::Editor::Terrain::TerrainResizeJobs jobs;
			const auto transaction = jobs.BuildTransaction(store, prepared, true, error);
			TerraEngine::Terrain::TerrainPublication publication(store);
			TerraEngine::Terrain::TerrainTransactionReceipt receipt{};
			if(transaction) receipt = publication.Publish(*transaction);
			context.Expect(transaction && receipt.outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed
				&& receipt.historyPayload.has_value(), "The confirmed crop must publish one history-bearing generation.");
			if(!transaction || !receipt.historyPayload) return;
			TerraEngine::Terrain::TerrainMutationQueue queue;
			{
				auto forward = *transaction;
				auto inverse = *transaction;
				inverse.resizeDelta = transaction->inverseResizeDelta;
				inverse.inverseResizeDelta = transaction->resizeDelta;
				TerraEngine::Editor::Terrain::TerrainHistoryCommand command(
					store, queue, std::move(forward), std::move(inverse), receipt.retainedHistoryBytes);
				const auto protectedCollection = TerraEngine::Terrain::TerrainGarbageCollector(store).Collect({});
				context.Expect(std::find(protectedCollection.removed.begin(), protectedCollection.removed.end(), removedHash)
					== protectedCollection.removed.end(),
					"A live crop history command must pin the removed descriptor blob.");
			}
			const auto prunedCollection = TerraEngine::Terrain::TerrainGarbageCollector(store).Collect({});
			context.Expect(std::find(prunedCollection.removed.begin(), prunedCollection.removed.end(), removedHash)
				!= prunedCollection.removed.end(),
				"The removed crop blob may be reclaimed only after history pruning releases its pin.");
		});
	}
}
