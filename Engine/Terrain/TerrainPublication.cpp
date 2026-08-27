// TerrainPublication.cpp -- The sole core path that changes a live terrain root.

#include "Terrain/TerrainPublication.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace TerraEngine::Terrain
{
namespace
{
	TerrainTransactionReceipt Reject(const TerrainEditTransaction& transaction, TerrainResultCode code, std::string message)
	{
		TerrainTransactionReceipt receipt{}; receipt.transactionId = transaction.transactionId; receipt.outcome = code == TerrainResultCode::Cancelled ? TerrainEditOutcome::Cancelled : TerrainEditOutcome::Rejected; receipt.code = code; receipt.message = std::move(message); return receipt;
	}
}

	TerrainTransactionReceipt TerrainPublication::Publish(TerrainEditTransaction transaction)
	{
		const TerrainRootCapture capture = assetStore.CaptureRoot();
		if (!capture.IsValid()) return Reject(transaction, TerrainResultCode::Failed, "Terrain asset has no publishable root.");
		if (auto validation = ValidateTerrainEditTransaction(transaction, *capture.root); !validation.Succeeded()) return Reject(transaction, validation.code, validation.message);
		for (const TerrainAreaObservation& observation : transaction.readSet)
		{
			const TerrainAreaDescriptor current = assetStore.ResolveDescriptorOrImplicit(*capture.root, observation.coordinate);
			if (current.editGeneration != observation.editGeneration || current.authoredContentHash != observation.authoredContentHash) return Reject(transaction, TerrainResultCode::Conflict, "Terrain area changed after worker observation.");
		}
		std::vector<ContentHash> historyBlobs;
		std::size_t inlineHistoryBytes = 0u;
		for (const TerrainPatch& patch : transaction.patches)
		{
			const auto observation = std::find_if(transaction.readSet.begin(), transaction.readSet.end(), [&](const TerrainAreaObservation& value) { return value.coordinate == patch.replacement.coordinate; });
			const TerrainAreaDescriptor current = assetStore.ResolveDescriptorOrImplicit(*capture.root, patch.replacement.coordinate);
			if (observation == transaction.readSet.end() || current.editGeneration == std::numeric_limits<Generation>::max())
				return Reject(transaction, TerrainResultCode::Conflict, "Every terrain patch requires a fresh observation and a representable next area generation.");
			if (patch.preparedBlobHash.IsValid() && !assetStore.Blobs().Resolve(patch.preparedBlobHash).has_value())
				return Reject(transaction, TerrainResultCode::CorruptData, "A terrain patch references an unavailable immutable blob.");
			for (const ContentHash hash : patch.retainedBlobHashes)
			{
				if (!hash.IsValid() || !assetStore.Blobs().Resolve(hash).has_value()) return Reject(transaction, TerrainResultCode::CorruptData, "Terrain history references an unavailable immutable blob.");
				historyBlobs.push_back(hash);
			}
			for (const TerrainExactSpan& span : patch.beforeSpans) { if (span.immutableBlobHash.IsValid()) historyBlobs.push_back(span.immutableBlobHash); else inlineHistoryBytes += span.compressedBytes.size(); }
			for (const TerrainExactSpan& span : patch.afterSpans) { if (span.immutableBlobHash.IsValid()) historyBlobs.push_back(span.immutableBlobHash); else inlineHistoryBytes += span.compressedBytes.size(); }
		}
		std::sort(historyBlobs.begin(), historyBlobs.end(), [](const ContentHash& left, const ContentHash& right) { return left.low64 != right.low64 ? left.low64 < right.low64 : left.high64 < right.high64; });
		historyBlobs.erase(std::unique(historyBlobs.begin(), historyBlobs.end()), historyBlobs.end());
		const bool recordsNewHistory = transaction.origin != TerrainEditOrigin::Undo && transaction.origin != TerrainEditOrigin::Redo;
		std::size_t retainedBlobBytes = 0u;
		for (const ContentHash hash : historyBlobs)
		{
			const auto bytes = assetStore.Blobs().Resolve(hash);
			if (!bytes || (recordsNewHistory && !assetStore.Blobs().Retain(hash))) return Reject(transaction, TerrainResultCode::CorruptData, "Terrain history could not retain an immutable blob before publication.");
			if (recordsNewHistory) retainedBlobBytes += bytes->size();
		}
		const std::size_t historyBytes = recordsNewHistory ? inlineHistoryBytes + retainedBlobBytes : 0u;
		auto releaseRetained = [&]() { if (recordsNewHistory) for (const ContentHash hash : historyBlobs) assetStore.Blobs().Release(hash); };
		if (injectFailureForTests) { releaseRetained(); return Reject(transaction, TerrainResultCode::Failed, "Terrain publication failure was injected before the root swap."); }
		std::vector<TerrainAreaDescriptor> replacements; replacements.reserve(transaction.patches.size());
		for (const TerrainPatch& patch : transaction.patches)
		{
			TerrainAreaDescriptor applied = patch.replacement;
			const TerrainAreaDescriptor current = assetStore.ResolveDescriptorOrImplicit(*capture.root, applied.coordinate);
			// Undo/redo restore older content but never older generations. This is the
			// per-area CAS epoch that keeps stale worker output invalid after replay.
			if (current.editGeneration == std::numeric_limits<Generation>::max()) { releaseRetained(); return Reject(transaction, TerrainResultCode::Overflow, "Terrain area edit generation overflowed."); }
			applied.editGeneration = current.editGeneration + 1u;
			replacements.push_back(std::move(applied));
		}
		std::shared_ptr<const TerrainAssetRoot> candidate;
		TerrainCodecResult built{};
		// Candidate builders derive identity from the complete canonical TERRAROOT1
		// bytes before the protected swap; callers cannot inject synthetic hashes.
		if (transaction.layerStackPatch.has_value()) built = assetStore.BuildLayerStackCandidate(*capture.root, *transaction.layerStackPatch, {}, candidate);
		else built = assetStore.BuildCandidateRoot(*capture.root, replacements, {}, candidate);
		if (!built.Succeeded()) { releaseRetained(); return Reject(transaction, built.code, built.message); }
		if (auto published = assetStore.PublishCandidate(capture.root, candidate); !published.Succeeded()) { releaseRetained(); return Reject(transaction, published.code, published.message); }
		TerrainTransactionReceipt receipt{}; receipt.transactionId = transaction.transactionId; receipt.outcome = TerrainEditOutcome::Committed; receipt.previousRootHash = capture.root->rootContentHash; receipt.publishedRootHash = candidate->rootContentHash; receipt.publicationGeneration = candidate->publicationGeneration; receipt.code = TerrainResultCode::Ok;
		TerrainDerivedInvalidation invalidation{};
		invalidation.assetUuid = candidate->assetUuid;
		invalidation.publicationGeneration = candidate->publicationGeneration;
		invalidation.kinds = DetermineTerrainInvalidationKinds(transaction);
		if(transaction.layerStackPatch.has_value())
		{
			const TerrainLayerPatch& layerPatch = *transaction.layerStackPatch;
			const std::uint16_t layerId = layerPatch.kind == TerrainLayerPatchKind::Add
				? layerPatch.layer.stableId : layerPatch.targetStableId;
			if(layerId != 0u) invalidation.layerIds.push_back(layerId);
		}
		for (const TerrainPatch& patch : transaction.patches)
		{
			receipt.affectedAreas.push_back(patch.replacement.coordinate);
			invalidation.areas.push_back(patch.replacement.coordinate);
		}
		receipt.invalidation = std::move(invalidation);
		receipt.retainedHistoryBytes = historyBytes;
		receipt.retainedBlobHashes = historyBlobs;
		std::vector<TerrainPatch> inverse = transaction.inversePatches;
		if (inverse.empty())
		{
			// Legacy producers may not have supplied a descriptor inverse yet. Keep
			// a typed exact descriptor baseline rather than silently losing undo.
			inverse.reserve(transaction.patches.size());
			for (const TerrainPatch& patch : transaction.patches)
			{
				TerrainPatch previous{};
				previous.replacement = assetStore.ResolveDescriptorOrImplicit(*capture.root, patch.replacement.coordinate);
				previous.kind = patch.kind;
				previous.beforeSpans = patch.afterSpans;
				previous.afterSpans = patch.beforeSpans;
				previous.retainedBlobHashes = patch.retainedBlobHashes;
				inverse.push_back(std::move(previous));
			}
		}
		if (recordsNewHistory)
		{
			receipt.historyPayload = TerrainHistoryPayload{transaction.patches, std::move(inverse), historyBytes, transaction.layerStackPatch, transaction.inverseLayerStackPatch};
		}
		return receipt;
	}

	std::vector<TerrainTransactionReceipt> TerrainPublication::PublishQueued(TerrainMutationQueue& queue)
	{
		std::vector<TerrainTransactionReceipt> receipts; for (TerrainEditTransaction& transaction : queue.DrainPending()) { TerrainTransactionReceipt receipt = Publish(std::move(transaction)); queue.Complete(receipt); receipts.push_back(std::move(receipt)); } return receipts;
	}
}
