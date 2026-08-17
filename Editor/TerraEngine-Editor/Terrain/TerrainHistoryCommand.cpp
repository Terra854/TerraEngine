// TerrainHistoryCommand.cpp -- Receipt-driven terrain undo and redo.

#include "TerrainHistoryCommand.h"

#include "Utilities/UUID.h"

#include <algorithm>

namespace TerraEngine::Editor::Terrain
{
	TerrainHistoryCommand::TerrainHistoryCommand(TerraEngine::Terrain::TerrainAssetStore& store, TerraEngine::Terrain::TerrainMutationQueue& queue,
		TerraEngine::Terrain::TerrainEditTransaction forward, TerraEngine::Terrain::TerrainEditTransaction inverse, std::size_t byteCost)
		: assetStore(store), mutationQueue(queue), forwardTransaction(std::move(forward)), inverseTransaction(std::move(inverse)), retainedBytes(byteCost)
	{
		historyId = "terrain-history-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID());
		for(const auto& patch : forwardTransaction.patches)
			retainedBlobHashes.insert(retainedBlobHashes.end(), patch.retainedBlobHashes.begin(), patch.retainedBlobHashes.end());
		std::sort(retainedBlobHashes.begin(), retainedBlobHashes.end(), [](const auto& left, const auto& right) {
			return left.low64 != right.low64 ? left.low64 < right.low64 : left.high64 < right.high64;
		});
		retainedBlobHashes.erase(std::unique(retainedBlobHashes.begin(), retainedBlobHashes.end()), retainedBlobHashes.end());
	}

	TerrainHistoryCommand::~TerrainHistoryCommand()
	{
		// Publication retained these immutable blobs when the original edit
		// committed. Release them only when pruning/clearing destroys this command.
		for(const auto hash : retainedBlobHashes) assetStore.Blobs().Release(hash);
	}
	void TerrainHistoryCommand::Undo() { EnqueueReplay(inverseTransaction, "undo"); }
	void TerrainHistoryCommand::Redo() { EnqueueReplay(forwardTransaction, "redo"); }
	void TerrainHistoryCommand::EnqueueReplay(const TerraEngine::Terrain::TerrainEditTransaction& source, const char* direction)
	{
		TerraEngine::Terrain::TerrainEditTransaction replay = source;
		const auto capture = assetStore.CaptureRoot();
		if(!capture.IsValid()) { pendingReplayId.clear(); return; }
		replay.transactionId = TerraEngine::GenerateUUID(); replay.origin = direction[0] == 'u' ? TerraEngine::Terrain::TerrainEditOrigin::Undo : TerraEngine::Terrain::TerrainEditOrigin::Redo;
		replay.targetAssetUuid = capture.root->assetUuid; replay.inputRootHash = capture.root->rootContentHash; replay.expectedLayerStackGeneration = capture.root->layerStackGeneration; replay.readSet.clear();
		for(auto& patch : replay.patches)
		{
			const auto current = assetStore.ResolveDescriptorOrImplicit(*capture.root, patch.replacement.coordinate);
			replay.readSet.push_back({current.coordinate, current.editGeneration, current.authoredContentHash});
			patch.replacement.editGeneration = current.editGeneration + 1U;
			// Replays use blobs already pinned by this history entry. Leaving the
			// retention payload attached would retain a new reference on every
			// undo/redo and make history garbage collection unbounded.
			patch.retainedBlobHashes.clear();
			// Weight-group validation deliberately requires exact inverse spans on
			// every replay. Undo/redo does not retain a second blob reference, but
			// it must keep these immutable value bytes for canonical validation.
			if(patch.kind != TerraEngine::Terrain::TerrainPatchKind::ReplaceWeightGroup
				&& patch.kind != TerraEngine::Terrain::TerrainPatchKind::ReplaceHoles)
			{
				patch.beforeSpans.clear();
				patch.afterSpans.clear();
			}
		}
		pendingReplayId = TerraEngine::ToHexString(replay.transactionId);
		if(!mutationQueue.Enqueue(std::move(replay)).Succeeded()) pendingReplayId.clear();
	}
}
