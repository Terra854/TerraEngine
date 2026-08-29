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

		registry.Add("Terrain.AtomicEdit.SignedImplicitCrossAreaCommitAndUndoRemainAtomic", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor explicitArea{};
			Terrain::TerrainAssetStore store(MakeRoot(explicitArea));
			Terrain::TerrainPublication publication(store);
			const auto observed = store.CaptureRoot();
			const auto implicitBefore = store.ResolveDescriptorOrImplicit(*observed.root, {-1, 0});
			const auto explicitBefore = store.ResolveDescriptorOrImplicit(*observed.root, {0, 0});
			Terrain::TerrainEditTransaction transaction{};
			transaction.transactionId = TerraEngine::GenerateUUID();
			transaction.targetAssetUuid = observed.root->assetUuid;
			transaction.inputRootHash = observed.root->rootContentHash;
			transaction.expectedLayerStackGeneration = observed.root->layerStackGeneration;
			transaction.affectedBounds = observed.root->authoredBounds;
			transaction.budget.maximumAreas = 2u;
			transaction.budget.maximumSamples = 2u * Terrain::kTerrainAreaSampleCount;
			for(const auto& before : {implicitBefore, explicitBefore})
			{
				Terrain::TerrainPatch patch{};
				patch.kind = Terrain::TerrainPatchKind::ReplaceHeight;
				patch.replacement = before;
				patch.replacement.editGeneration++;
				patch.replacement.authoredContentHash = before.coordinate.x < 0
					? Terrain::ContentHash{101u, 102u} : Terrain::ContentHash{103u, 104u};
				transaction.readSet.push_back({before.coordinate, before.editGeneration, before.authoredContentHash});
				transaction.patches.push_back(std::move(patch));
			}
			context.Expect(transaction.patches.size() == 2u && transaction.readSet.size() == 2u
				&& transaction.readSet.front().coordinate == Terrain::TerrainAreaCoordinate{-1, 0},
				"A signed brush batch must describe both sides in deterministic coordinate order, including implicit coverage.");
			for(const auto& before : {implicitBefore, explicitBefore})
			{
				Terrain::TerrainPatch inverse{};
				inverse.kind = Terrain::TerrainPatchKind::ReplaceHeight;
				inverse.replacement = before;
				inverse.replacement.editGeneration++;
				transaction.inversePatches.push_back(std::move(inverse));
			}
			const auto committed = publication.Publish(transaction);
			const auto afterCommit = store.CaptureRoot();
			context.Expect(committed.outcome == Terrain::TerrainEditOutcome::Committed
				&& afterCommit.publicationGeneration == observed.publicationGeneration + 1u
				&& store.FindDescriptor(*afterCommit.root, {-1, 0}).has_value(),
				"Both signed areas, including materialized implicit coverage, must become visible at one root generation.");

			if(!committed.historyPayload) return;
			Terrain::TerrainEditTransaction undo{};
			undo.transactionId = TerraEngine::GenerateUUID();
			undo.origin = Terrain::TerrainEditOrigin::Undo;
			undo.targetAssetUuid = afterCommit.root->assetUuid;
			undo.inputRootHash = afterCommit.root->rootContentHash;
			undo.expectedLayerStackGeneration = afterCommit.root->layerStackGeneration;
			undo.affectedBounds = transaction.affectedBounds;
			undo.budget.maximumAreas = 2u;
			undo.budget.maximumSamples = 2u * Terrain::kTerrainAreaSampleCount;
			undo.patches = committed.historyPayload->inversePatches;
			for(const auto coordinate : {Terrain::TerrainAreaCoordinate{-1, 0}, Terrain::TerrainAreaCoordinate{0, 0}})
			{
				const auto current = store.ResolveDescriptorOrImplicit(*afterCommit.root, coordinate);
				undo.readSet.push_back({coordinate, current.editGeneration, current.authoredContentHash});
			}
			const auto undone = publication.Publish(std::move(undo));
			context.Expect(undone.outcome == Terrain::TerrainEditOutcome::Committed
				&& store.CaptureRoot().publicationGeneration == observed.publicationGeneration + 2u,
				"Undo must restore both sides through one newer atomic root rather than exposing a half-replayed seam.");
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

		registry.Add("Terrain.AtomicEdit.CommonReadyBarrierRejectsMixedGenerations", [](TestContext& context) {
			Terrain::TerrainAreaDescriptor descriptor{};
			Terrain::TerrainAssetStore store(MakeRoot(descriptor));
			const auto capture = store.CaptureRoot();
			Terrain::TerrainCommonReadyCandidate candidate{};
			candidate.assetUuid = capture.root->assetUuid;
			candidate.rootHash = capture.root->rootContentHash;
			candidate.derivedHaloIdentity = {301u, 302u};
			candidate.publicationGeneration = capture.publicationGeneration;
			candidate.renderGeneration = capture.publicationGeneration;
			candidate.pickingGeneration = capture.publicationGeneration;
			candidate.spatialGeneration = capture.publicationGeneration;
			candidate.residencyGeneration = capture.publicationGeneration;
			candidate.collisionGeneration = capture.publicationGeneration;
			candidate.rootComplete = true;
			candidate.derivedHalosComplete = true;
			candidate.renderComplete = true;
			candidate.pickingComplete = true;
			candidate.spatialComplete = true;
			candidate.residencyRetirementsComplete = true;
			candidate.demandedCollisionComplete = true;
			const auto valid = Terrain::ValidateTerrainCommonReadyCandidate(candidate, store);
			candidate.collisionGeneration++;
			const auto mixed = Terrain::ValidateTerrainCommonReadyCandidate(candidate, store);
			candidate.collisionGeneration = capture.publicationGeneration;
			candidate.pickingComplete = false;
			const auto incomplete = Terrain::ValidateTerrainCommonReadyCandidate(candidate, store);
			context.Expect(valid.Succeeded() && mixed.code == Terrain::TerrainResultCode::Conflict
				&& incomplete.code == Terrain::TerrainResultCode::InvalidArgument,
				"Common-ready publication must accept one complete generation and fail closed for mixed or incomplete downstream products.");
		});
	}
}
