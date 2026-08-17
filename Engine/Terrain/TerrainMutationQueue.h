// TerrainMutationQueue.h -- Bounded multi-producer ingress for typed edits.

#pragma once

#include "Terrain/TerrainEditTransaction.h"

#include <deque>
#include <mutex>

namespace TerraEngine::Terrain
{
	class TERRAENGINE_API TerrainMutationQueue
	{
	public:
		explicit TerrainMutationQueue(std::size_t maximumPending = 256u);
		[[nodiscard]] TerrainCodecResult Enqueue(TerrainEditTransaction transaction);
		[[nodiscard]] std::vector<TerrainEditTransaction> DrainPending();
		void Complete(TerrainTransactionReceipt receipt);
		[[nodiscard]] std::vector<TerrainTransactionReceipt> DrainCompletedReceipts();
		[[nodiscard]] std::size_t PendingCount() const;

	private:
		const std::size_t maximumPending;
		mutable std::mutex mutex;
		std::deque<TerrainEditTransaction> pending;
		std::deque<TerrainTransactionReceipt> completed;
	};
}
