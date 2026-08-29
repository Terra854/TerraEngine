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

	TerrainCodecResult ValidateTerrainCommonReadyCandidate(
		const TerrainCommonReadyCandidate& candidate,
		const TerrainAssetStore& store)
	{
		const TerrainRootCapture capture = store.CaptureRoot();
		if(!capture.IsValid())
			return {TerrainResultCode::Failed, "Terrain common-ready validation requires a published authored root."};
		if(candidate.assetUuid != capture.root->assetUuid
			|| candidate.rootHash != capture.root->rootContentHash
			|| candidate.publicationGeneration != capture.publicationGeneration)
		{
			return {TerrainResultCode::Conflict, "Terrain common-ready candidate observed a stale authored generation."};
		}
		if(!candidate.rootComplete || !candidate.derivedHalosComplete
			|| !candidate.renderComplete || !candidate.pickingComplete
			|| !candidate.spatialComplete || !candidate.residencyRetirementsComplete
			|| !candidate.demandedCollisionComplete || !candidate.derivedHaloIdentity.IsValid())
		{
			return {TerrainResultCode::InvalidArgument,
				"Terrain common-ready publication requires complete root, halo, render, picking, spatial, residency, and collision products."};
		}
		const Generation expected = candidate.publicationGeneration;
		if(candidate.renderGeneration != expected || candidate.pickingGeneration != expected
			|| candidate.spatialGeneration != expected || candidate.residencyGeneration != expected
			|| candidate.collisionGeneration != expected)
		{
			return {TerrainResultCode::Conflict, "Terrain common-ready products do not share one publication generation."};
		}
		return {};
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
		if (transaction.resizeDelta)
		{
			for (const ContentHash hash : transaction.resizeDelta->retainedBlobHashes)
				historyBlobs.push_back(hash);
		}
		if (transaction.inverseResizeDelta.has_value())
		{
			for (const TerrainAreaDescriptor& descriptor : transaction.inverseResizeDelta->descriptorUpserts)
			{
				if (descriptor.channels.heightHash) historyBlobs.push_back(*descriptor.channels.heightHash);
				if (descriptor.channels.holeHash) historyBlobs.push_back(*descriptor.channels.holeHash);
				for (const auto& [group, hash] : descriptor.channels.weightGroupHashes)
				{
					(void)group;
					historyBlobs.push_back(hash);
				}
			}
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
		if (transaction.resizeDelta.has_value())
		{
			std::vector<TerrainAreaDescriptor> resizeUpserts = transaction.resizeDelta->descriptorUpserts;
			for (TerrainAreaDescriptor& descriptor : resizeUpserts)
			{
				const auto patched = std::find_if(replacements.begin(), replacements.end(), [&](const TerrainAreaDescriptor& value) {
					return value.coordinate == descriptor.coordinate;
				});
				if (patched != replacements.end()) descriptor = *patched;
				else
				{
					const auto current = assetStore.ResolveDescriptorOrImplicit(*capture.root, descriptor.coordinate);
					if (current.editGeneration == std::numeric_limits<Generation>::max())
					{
						releaseRetained();
						return Reject(transaction, TerrainResultCode::Overflow, "Terrain resize descriptor generation overflowed.");
					}
					descriptor.editGeneration = current.editGeneration + 1u;
				}
			}
			for (const TerrainAreaDescriptor& replacement : replacements)
				if (std::none_of(resizeUpserts.begin(), resizeUpserts.end(), [&](const TerrainAreaDescriptor& value) {
					return value.coordinate == replacement.coordinate;
				})) resizeUpserts.push_back(replacement);
			std::sort(resizeUpserts.begin(), resizeUpserts.end(), [](const auto& left, const auto& right) {
				return left.coordinate < right.coordinate;
			});
			built = assetStore.BuildResizeCandidate(*capture.root, transaction.resizeDelta->proposedBounds,
				resizeUpserts, transaction.resizeDelta->descriptorRemovals, candidate);
		}
		else if (transaction.layerStackPatch.has_value()) built = assetStore.BuildLayerStackCandidate(*capture.root, *transaction.layerStackPatch, {}, candidate);
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
		if (transaction.resizeDelta)
		{
			for (const TerrainAreaDescriptor& descriptor : transaction.resizeDelta->descriptorUpserts)
			{
				receipt.affectedAreas.push_back(descriptor.coordinate);
				invalidation.areas.push_back(descriptor.coordinate);
			}
			for (const TerrainAreaCoordinate coordinate : transaction.resizeDelta->descriptorRemovals)
			{
				receipt.affectedAreas.push_back(coordinate);
				invalidation.areas.push_back(coordinate);
			}
			invalidation.areas.insert(invalidation.areas.end(), transaction.resizeDelta->affectedOneRing.begin(),
				transaction.resizeDelta->affectedOneRing.end());
		}
		auto sortUniqueAreas = [](std::vector<TerrainAreaCoordinate>& areas) {
			std::sort(areas.begin(), areas.end());
			areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
		};
		sortUniqueAreas(receipt.affectedAreas);
		sortUniqueAreas(invalidation.areas);
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
			receipt.historyPayload = TerrainHistoryPayload{transaction.patches, std::move(inverse), historyBytes,
				transaction.layerStackPatch, transaction.inverseLayerStackPatch,
				transaction.resizeDelta, transaction.inverseResizeDelta};
		}
		return receipt;
	}

	std::vector<TerrainTransactionReceipt> TerrainPublication::PublishQueued(TerrainMutationQueue& queue)
	{
		std::vector<TerrainTransactionReceipt> receipts;
		for (TerrainAuthoringCompletion& completion : PublishQueuedCompletions(queue))
			receipts.push_back(std::move(completion.receipt));
		return receipts;
	}

	std::vector<TerrainAuthoringCompletion> TerrainPublication::PublishQueuedCompletions(TerrainMutationQueue& queue)
	{
		std::vector<TerrainAuthoringCompletion> completions;
		for (TerrainEditTransaction& transaction : queue.DrainPending())
		{
			TerrainAuthoringCompletion completion{};
			completion.transaction = transaction;
			completion.receipt = Publish(std::move(transaction));
			queue.Complete(completion.receipt);
			completions.push_back(std::move(completion));
		}
		return completions;
	}
}
