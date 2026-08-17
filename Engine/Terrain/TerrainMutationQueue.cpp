// TerrainMutationQueue.cpp -- Short-lock queue; publication owns all mutation.

#include "Terrain/TerrainMutationQueue.h"

#include <algorithm>

namespace TerraEngine::Terrain
{
	TerrainMutationQueue::TerrainMutationQueue(std::size_t value) : maximumPending(std::max<std::size_t>(1u, value)) {}

	TerrainCodecResult TerrainMutationQueue::Enqueue(TerrainEditTransaction transaction)
	{
		std::scoped_lock lock(mutex);
		if (pending.size() >= maximumPending) return {TerrainResultCode::BudgetExceeded, "Terrain edit queue is at its bounded capacity."};
		if (std::any_of(pending.begin(), pending.end(), [&](const TerrainEditTransaction& current) { return current.transactionId == transaction.transactionId; })) return {TerrainResultCode::Conflict, "Terrain edit transaction id is already queued."};
		pending.push_back(std::move(transaction)); return {};
	}

	std::vector<TerrainEditTransaction> TerrainMutationQueue::DrainPending()
	{
		std::scoped_lock lock(mutex); std::vector<TerrainEditTransaction> result; result.reserve(pending.size()); while (!pending.empty()) { result.push_back(std::move(pending.front())); pending.pop_front(); } return result;
	}

	void TerrainMutationQueue::Complete(TerrainTransactionReceipt receipt) { std::scoped_lock lock(mutex); completed.push_back(std::move(receipt)); }
	std::vector<TerrainTransactionReceipt> TerrainMutationQueue::DrainCompletedReceipts() { std::scoped_lock lock(mutex); std::vector<TerrainTransactionReceipt> result; result.reserve(completed.size()); while (!completed.empty()) { result.push_back(std::move(completed.front())); completed.pop_front(); } return result; }
	std::size_t TerrainMutationQueue::PendingCount() const { std::scoped_lock lock(mutex); return pending.size(); }
}
