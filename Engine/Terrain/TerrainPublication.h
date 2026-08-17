// TerrainPublication.h -- Protected, all-or-nothing terrain root publication.

#pragma once

#include "Terrain/TerrainMutationQueue.h"

namespace TerraEngine::Terrain
{
	class TERRAENGINE_API TerrainPublication
	{
	public:
		explicit TerrainPublication(TerrainAssetStore& store) : assetStore(store) {}
		void SetInjectFailureForTests(bool value) noexcept { injectFailureForTests = value; }
		[[nodiscard]] TerrainTransactionReceipt Publish(TerrainEditTransaction transaction);
		[[nodiscard]] std::vector<TerrainTransactionReceipt> PublishQueued(TerrainMutationQueue& queue);

	private:
		TerrainAssetStore& assetStore;
		bool injectFailureForTests = false;
	};
}
