// TerrainHistoryCommand.h -- Deferred terrain replay through the typed queue.

#pragma once

#include "../Subsystems/UndoRedoSystem.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainMutationQueue.h"

#include <vector>

namespace TerraEngine::Editor::Terrain
{
	class TerrainHistoryCommand final : public ICommand
	{
	public:
		TerrainHistoryCommand(TerraEngine::Terrain::TerrainAssetStore& store, TerraEngine::Terrain::TerrainMutationQueue& queue,
			TerraEngine::Terrain::TerrainEditTransaction forward, TerraEngine::Terrain::TerrainEditTransaction inverse, std::size_t byteCost);
		~TerrainHistoryCommand() override;
		void Undo() override;
		void Redo() override;
		std::string GetName() const override { return "Terrain Stroke"; }
		std::string GetHistoryId() const override { return historyId; }
		std::string GetPendingReplayTransactionId() const override { return pendingReplayId; }
		std::size_t GetHistoryBytes() const override { return retainedBytes; }
	private:
		void EnqueueReplay(const TerraEngine::Terrain::TerrainEditTransaction& templateTransaction, const char* direction);
		TerraEngine::Terrain::TerrainAssetStore& assetStore;
		TerraEngine::Terrain::TerrainMutationQueue& mutationQueue;
		TerraEngine::Terrain::TerrainEditTransaction forwardTransaction;
		TerraEngine::Terrain::TerrainEditTransaction inverseTransaction;
		std::string historyId;
		std::string pendingReplayId;
		std::size_t retainedBytes = 0U;
		std::vector<TerraEngine::Terrain::ContentHash> retainedBlobHashes;
	};
}
