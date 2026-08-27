// TerrainAtomicEditTests.cpp -- Protected terrain edit, recovery, and GC regressions.

#include "TestHarness.h"

#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainGarbageCollector.h"
#include "Terrain/TerrainPublication.h"

#include <atomic>
#include <filesystem>
#include <vector>

namespace TerraEngine::Tests
{
namespace
{
	std::shared_ptr<const Terrain::TerrainAssetRoot> MakeRoot(Terrain::TerrainAreaDescriptor& descriptor)
	{
		auto root = std::make_shared<Terrain::TerrainAssetRoot>();
		root->assetUuid = TerraEngine::GenerateUUID();
		root->publicationGeneration = 1u;
		root->persistedRootGeneration = 1u;
		root->rootContentHash = {11u, 12u};
		root->authoredBounds = {-1, -1, 2, 2};
		root->layerStackGeneration = 1u;
		descriptor.coordinate = {0, 0};
		descriptor.editGeneration = 1u;
		descriptor.authoredContentHash = {21u, 22u};
		Terrain::TerrainDirectoryShard shard{};
		shard.areaBounds = {-1, -1, 2, 2};
		shard.descriptors.emplace(descriptor.coordinate, descriptor);
		root->directoryShards.push_back(std::move(shard));
		return root;
	}

	Terrain::TerrainEditTransaction MakeEdit(const Terrain::TerrainAssetRoot& root, const Terrain::TerrainAreaDescriptor& before, Terrain::ContentHash blobHash = {})
	{
		Terrain::TerrainEditTransaction edit{};
		edit.transactionId = TerraEngine::GenerateUUID();
		edit.targetAssetUuid = root.assetUuid;
		edit.inputRootHash = root.rootContentHash;
		edit.expectedLayerStackGeneration = root.layerStackGeneration;
		edit.affectedBounds = {0, 0, 1, 1};
		edit.readSet.push_back({before.coordinate, before.editGeneration, before.authoredContentHash});
		Terrain::TerrainPatch forward{};
		forward.replacement = before;
		forward.replacement.editGeneration = before.editGeneration + 1u;
		forward.replacement.authoredContentHash = {31u, 32u};
		forward.preparedBlobHash = blobHash;
		Terrain::TerrainPatch inverse{};
		inverse.replacement = before;
		inverse.preparedBlobHash = blobHash;
		edit.patches.push_back(forward);
		edit.inversePatches.push_back(inverse);
		return edit;
	}
}

	void RegisterTerrainAtomicEditTests(TestRegistry& registry)
	{
		registry.Add("Terrain.AtomicEdit.RejectsConflictsUnavailableBlobsAndRollsBack", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor before{};
			Terrain::TerrainAssetStore store(MakeRoot(before));
			Terrain::TerrainPublication publication(store);
			const auto root = store.CaptureRoot();
			auto stale = MakeEdit(*root.root, before);
			stale.readSet.front().editGeneration = 99u;
			const auto staleReceipt = publication.Publish(stale);
			auto unavailable = MakeEdit(*root.root, before, {91u, 92u});
			const auto unavailableReceipt = publication.Publish(unavailable);
			publication.SetInjectFailureForTests(true);
			const auto failedReceipt = publication.Publish(MakeEdit(*root.root, before));
			context.Expect(staleReceipt.code == Terrain::TerrainResultCode::Conflict && unavailableReceipt.code == Terrain::TerrainResultCode::CorruptData && failedReceipt.outcome == Terrain::TerrainEditOutcome::Rejected, "Every failed terrain preflight must reject the complete edit with a specific category.");
			context.Expect(store.CaptureRoot().root == root.root && store.CaptureRoot().publicationGeneration == 1u, "Conflicts, unavailable blobs, and injected failures must leave the prior immutable root visible.");
		});

		registry.Add("Terrain.AtomicEdit.RejectedLayerMutationPreservesTheCompleteRoot", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor before{};
			auto initialRoot = std::make_shared<Terrain::TerrainAssetRoot>(*MakeRoot(before));
			Terrain::TerrainLayerStack layers{};
			layers.generation = initialRoot->layerStackGeneration;
			layers.layers = {
				{1u, "Grass", TerraEngine::GenerateUUID()},
				{2u, "Rock", TerraEngine::GenerateUUID()}
			};
			initialRoot->defaultLayerId = 1u;
			initialRoot->layerStack = std::make_shared<const Terrain::TerrainLayerStack>(layers);
			// The fixture must itself be a complete canonical root before testing a
			// rejected layer mutation; persisted manifests require shard identities.
			initialRoot->directoryShards.front().identity = {41u, 42u};
			const Terrain::TerrainRootCodecResult encoded = Terrain::EncodeTerrainRootManifest(*initialRoot);
			initialRoot->rootContentHash = encoded.contentHash;

			Terrain::TerrainAssetStore store(initialRoot);
			Terrain::TerrainPublication publication(store);
			const Terrain::TerrainRootCapture observed = store.CaptureRoot();
			Terrain::TerrainEditTransaction transaction{};
			transaction.transactionId = TerraEngine::GenerateUUID();
			transaction.targetAssetUuid = observed.root->assetUuid;
			transaction.inputRootHash = observed.root->rootContentHash;
			transaction.expectedLayerStackGeneration = observed.root->layerStackGeneration;
			transaction.affectedBounds = observed.root->authoredBounds;
			Terrain::TerrainLayer duplicate = observed.root->layerStack->layers.back();
			duplicate.stableId = observed.root->layerStack->layers.front().stableId;
			transaction.layerStackPatch = Terrain::TerrainLayerPatch{
				Terrain::TerrainLayerPatchKind::Add,
				observed.root->layerStackGeneration,
				0u,
				0xffffffffu,
				duplicate
			};

			const Terrain::TerrainTransactionReceipt rejected = publication.Publish(std::move(transaction));
			const Terrain::TerrainRootCapture after = store.CaptureRoot();
			const auto descriptorAfter = store.FindDescriptor(*after.root, before.coordinate);
			context.Expect(encoded.Succeeded() && rejected.outcome == Terrain::TerrainEditOutcome::Rejected
				&& rejected.code == Terrain::TerrainResultCode::InvalidArgument,
				"A duplicate stable layer ID must reject the whole protected root mutation.");
			context.Expect(after.root == observed.root
				&& after.publicationGeneration == observed.publicationGeneration
				&& after.root->rootContentHash == observed.root->rootContentHash
				&& after.root->layerStack == observed.root->layerStack
				&& after.root->layerStack->layers.size() == 2u
				&& descriptorAfter.has_value()
				&& descriptorAfter->authoredContentHash == before.authoredContentHash,
				"Rejected layer metadata must leave the prior root pointer, generation, hash, layer stack, and area descriptor atomically visible.");
		});

		registry.Add("Terrain.AtomicEdit.MonotonicReplayRetainsExactHistoryAndVisibility", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor before{};
			Terrain::TerrainAssetStore store(MakeRoot(before));
			Terrain::TerrainPublication publication(store);
			const std::vector<std::uint8_t> blobBytes = {1u, 2u, 3u, 4u};
			const Terrain::ContentHash blobHash = Terrain::HashTerrainBytes(blobBytes);
			context.Expect(store.Blobs().PutVerified(blobHash, blobBytes), "A history blob must enter immutable storage with its exact identity.");
			auto edit = MakeEdit(*store.CaptureRoot().root, before, blobHash);
			Terrain::TerrainExactSpan span{};
			span.uncompressedByteCount = 8u;
			span.compressedBytes = blobBytes;
			span.compressedContentHash = blobHash;
			span.immutableBlobHash = blobHash;
			edit.patches.front().beforeSpans.push_back(span);
			edit.patches.front().afterSpans.push_back(span);
			edit.patches.front().retainedBlobHashes.push_back(blobHash);
			const auto committed = publication.Publish(edit);
			const auto after = store.CaptureRoot();
			context.Expect(committed.outcome == Terrain::TerrainEditOutcome::Committed && after.publicationGeneration == 2u && committed.historyPayload.has_value() && committed.retainedHistoryBytes == blobBytes.size(), "A committed receipt must retain exact before/after spans only after the protected root swap without double-counting one immutable blob.");
			context.Expect(after.root->directoryShards.front().descriptors.at(before.coordinate).editGeneration == 2u, "The first committed area CAS must advance its generation exactly once.");

			auto replay = MakeEdit(*after.root, after.root->directoryShards.front().descriptors.at(before.coordinate), blobHash);
			replay.origin = Terrain::TerrainEditOrigin::Undo;
			replay.patches = committed.historyPayload->inversePatches;
			replay.patches.front().replacement.authoredContentHash = before.authoredContentHash;
			replay.patches.front().replacement.editGeneration = before.editGeneration;
			replay.readSet.front().authoredContentHash = after.root->directoryShards.front().descriptors.at(before.coordinate).authoredContentHash;
			const auto undone = publication.Publish(replay);
			const auto afterUndo = store.CaptureRoot();
			context.Expect(undone.outcome == Terrain::TerrainEditOutcome::Committed && afterUndo.publicationGeneration == 3u && afterUndo.root->directoryShards.front().descriptors.at(before.coordinate).editGeneration == 3u, "Undo must restore exact prior content through a newer area and root generation.");
			const auto rejectedReplay = publication.Publish(edit);
			context.Expect(rejectedReplay.code == Terrain::TerrainResultCode::Conflict && store.CaptureRoot().publicationGeneration == 3u, "A replay prepared for an older root must be rejected without moving visibility or history state.");
		});

		registry.Add("Terrain.AtomicEdit.PersistenceGcAndDerivedChecksFailClosed", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor descriptor{};
			Terrain::TerrainAssetStore store(MakeRoot(descriptor));
			const std::vector<std::uint8_t> unreferencedBytes = {8u, 9u};
			const Terrain::ContentHash unreferenced = Terrain::HashTerrainBytes(unreferencedBytes);
			store.Blobs().PutVerified(unreferenced, unreferencedBytes);
			Terrain::TerrainGarbageCollector collector(store);
			std::atomic_bool cancellation = false;
			const auto gc = collector.Collect({{}, 1u, &cancellation});
			context.Expect(gc.removed.size() == 1u && gc.removed.front() == unreferenced, "Bounded GC should reclaim only an unprotected immutable blob.");

			const auto capture = store.CaptureRoot();
			Terrain::TerrainDerivedCandidate current{capture.root->assetUuid, capture.root->rootContentHash, capture.publicationGeneration, true, nullptr};
			const auto valid = Terrain::ValidateTerrainDerivedCandidate(current, store);
			current.sourcePublicationGeneration += 1u;
			const auto stale = Terrain::ValidateTerrainDerivedCandidate(current, store);
			cancellation.store(true, std::memory_order_release);
			current.cancellation = &cancellation;
			const auto cancelled = Terrain::ValidateTerrainDerivedCandidate(current, store);
			context.Expect(valid.Succeeded() && stale.code == Terrain::TerrainResultCode::Conflict && cancelled.code == Terrain::TerrainResultCode::Cancelled, "Derived candidates must revalidate generation and cancellation immediately before publication.");

			const std::filesystem::path persistenceDirectory = std::filesystem::temp_directory_path() / ("TerraEngineTerrainAtomic-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			const auto saved = store.PersistAtomically({persistenceDirectory, nullptr, Terrain::TerrainPersistenceFailurePoint::None});
			const auto interrupted = store.PersistAtomically({persistenceDirectory, nullptr, Terrain::TerrainPersistenceFailurePoint::CurrentReplacement});
			Terrain::TerrainAreaDescriptor recoveredDescriptor{};
			Terrain::TerrainAssetStore recovered(MakeRoot(recoveredDescriptor));
			const auto recovery = recovered.RecoverPersistedRoot(persistenceDirectory);
			context.Expect(saved.Succeeded() && interrupted.code == Terrain::TerrainResultCode::Failed && recovery.Succeeded() && recovered.CaptureRoot().root->rootContentHash == saved.savedRootHash, "Manifest-last persistence must preserve a complete predecessor CURRENT root when replacement is interrupted.");
			std::error_code cleanupError;
			std::filesystem::remove_all(persistenceDirectory, cleanupError);
		});
	}
}
