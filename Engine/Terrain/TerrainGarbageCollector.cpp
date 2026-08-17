// TerrainGarbageCollector.cpp -- Provider-neutral reachability protection.

#include "Terrain/TerrainGarbageCollector.h"

#include <algorithm>

namespace TerraEngine::Terrain
{
	TerrainGarbageCollectionReceipt TerrainGarbageCollector::Collect(const TerrainGarbageCollectionRequest& request)
	{
		TerrainGarbageCollectionReceipt receipt{};
		const TerrainRootCapture live = assetStore.CaptureRoot();
		std::vector<ContentHash> protectedHashes;
		if (live.IsValid()) protectedHashes = assetStore.CollectReferencedBlobHashes(*live.root);
		const std::vector<ContentHash> persisted = assetStore.CollectPersistedReferencedBlobHashes();
		protectedHashes.insert(protectedHashes.end(), persisted.begin(), persisted.end());
		auto append = [&](const std::vector<ContentHash>& values) { protectedHashes.insert(protectedHashes.end(), values.begin(), values.end()); };
		append(request.references.saved); append(request.references.predecessor); append(request.references.history);
		append(request.references.activeSave); append(request.references.resident); append(request.references.inFlight);
		std::sort(protectedHashes.begin(), protectedHashes.end(), [](const ContentHash& left, const ContentHash& right) { return left.low64 != right.low64 ? left.low64 < right.low64 : left.high64 < right.high64; });
		protectedHashes.erase(std::unique(protectedHashes.begin(), protectedHashes.end()), protectedHashes.end());
		receipt.protectedCount = protectedHashes.size();
		if (request.cancellation != nullptr && request.cancellation->load(std::memory_order_acquire)) { receipt.cancelled = true; return receipt; }
		receipt.removed = assetStore.Blobs().CollectUnretained(protectedHashes, std::max<std::size_t>(1u, request.maximumRemovals), request.cancellation);
		receipt.cancelled = request.cancellation != nullptr && request.cancellation->load(std::memory_order_acquire);
		return receipt;
	}
}
