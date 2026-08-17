// TerrainGarbageCollector.h -- Bounded, cancellable cleanup of immutable terrain blobs.

#pragma once

#include "Terrain/TerrainAssetStore.h"

#include <atomic>
#include <vector>

namespace TerraEngine::Terrain
{
	// Every producer that can still resolve a blob contributes references here.
	// The collector never guesses about ownership and therefore cannot reclaim a
	// blob used by history, a pending save, resident coverage, or worker input.
	struct TerrainGarbageCollectionReferences
	{
		std::vector<ContentHash> saved;
		std::vector<ContentHash> predecessor;
		std::vector<ContentHash> history;
		std::vector<ContentHash> activeSave;
		std::vector<ContentHash> resident;
		std::vector<ContentHash> inFlight;
	};

	struct TerrainGarbageCollectionRequest
	{
		TerrainGarbageCollectionReferences references{};
		std::size_t maximumRemovals = 64u;
		const std::atomic_bool* cancellation = nullptr;
	};

	struct TerrainGarbageCollectionReceipt
	{
		std::vector<ContentHash> removed;
		bool cancelled = false;
		std::size_t protectedCount = 0u;
	};

	class TERRAENGINE_API TerrainGarbageCollector
	{
	public:
		explicit TerrainGarbageCollector(TerrainAssetStore& store) : assetStore(store) {}
		[[nodiscard]] TerrainGarbageCollectionReceipt Collect(const TerrainGarbageCollectionRequest& request);

	private:
		TerrainAssetStore& assetStore;
	};
}
