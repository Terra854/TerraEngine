#include "TestHarness.h"
#include "Terrain/TerrainPublication.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Terrain;
		struct RetentionBlob {
			ContentHash hash;
			std::vector<std::uint8_t> bytes;
		};
		std::shared_ptr<TerrainAssetRoot> RetentionRoot(TestContext& context) {
			auto root = std::make_shared<TerrainAssetRoot>();
			root->assetUuid = GenerateUUID();
			root->publicationGeneration = root->persistedRootGeneration = root->layerStackGeneration = 1U;
			root->authoredBounds = {0, 0, kTerrainAreaExtentSamples, kTerrainAreaExtentSamples};
			TerrainAreaDescriptor descriptor;
			descriptor.coordinate = {0, 0};
			descriptor.editGeneration = 1U;
			descriptor.authoredContentHash = {11U, 13U};
			TerrainDirectoryShard shard;
			shard.identity = {17U, 19U};
			shard.areaBounds = {0, 0, 1, 1};
			shard.descriptors.emplace(descriptor.coordinate, descriptor);
			root->directoryShards.push_back(std::move(shard));
			const auto encoded = EncodeTerrainRootManifest(*root);
			context.Expect(encoded.Succeeded(), "The retention fixture must start from a real valid canonical root.");
			root->rootContentHash = encoded.contentHash;
			return root;
		}
		struct RetentionFixture final {
			std::shared_ptr<TerrainAssetRoot> root;
			TerrainAssetStore store;
			TerrainPublication publication;
			std::array<RetentionBlob, 3> blobs;
			explicit RetentionFixture(TestContext& context)
				: root(RetentionRoot(context)), store(root), publication(store) {
				for (unsigned index = 0; index < blobs.size(); ++index) {
					blobs[index].bytes.assign(16U, static_cast<std::uint8_t>(index + 1U));
					blobs[index].hash = HashTerrainBytes(blobs[index].bytes);
					context.Expect(store.Blobs().PutVerified(blobs[index].hash, blobs[index].bytes), "Each history blob must enter the actual verified resolver.");
				}
				// This is the actual retention traversal order, independent of the
				// resolver's hash-table iteration order or invented fixture hashes.
				std::sort(blobs.begin(), blobs.end(), [](const auto& left, const auto& right) {
					return left.hash.low64 != right.hash.low64 ? left.hash.low64 < right.hash.low64 : left.hash.high64 < right.hash.high64;
				});
				context.Expect(blobs[0].hash != blobs[1].hash && blobs[1].hash != blobs[2].hash, "The fixture must contain three distinct real identities.");
			}
			TerrainEditTransaction Edit(TestContext& context, TerrainEditOrigin origin = TerrainEditOrigin::VisibleEditor) const {
				TerrainEditTransaction edit;
				edit.transactionId = GenerateUUID();
				edit.origin = origin;
				edit.targetAssetUuid = root->assetUuid;
				edit.inputRootHash = root->rootContentHash;
				edit.expectedLayerStackGeneration = root->layerStackGeneration;
				edit.affectedBounds = {0, 0, 1, 1};
				const auto& before = root->directoryShards.front().descriptors.begin()->second;
				edit.readSet.push_back({before.coordinate, before.editGeneration, before.authoredContentHash});
				TerrainPatch patch;
				patch.kind = TerrainPatchKind::ReplaceDescriptorMetadata;
				patch.replacement = before;
				patch.replacement.editGeneration = 2U;
				patch.replacement.authoredContentHash = {23U, 29U};
				for (const auto& blob : blobs) {
					// Match the exact opaque-span contract exercised by existing atomic
					// edit tests; validation hashes these actual bytes without decoding.
					TerrainExactSpan span;
					span.uncompressedByteCount = 32U;
					span.compressedBytes = blob.bytes;
					span.compressedContentHash = blob.hash;
					span.immutableBlobHash = blob.hash;
					context.Expect(ValidateTerrainExactSpan(span).Succeeded(), "Every history span must satisfy production validation.");
					patch.beforeSpans.push_back(std::move(span));
				}
				patch.afterSpans.push_back(patch.beforeSpans.front()); // The actual history set must deduplicate A.
				edit.patches.push_back(std::move(patch));
				context.Expect(ValidateTerrainEditTransaction(edit, *root).Succeeded(), "The typed transaction must pass actual preflight before testing retention failure.");
				return edit;
			}
			void Remove(TestContext& context, std::size_t missingIndex) {
				std::vector<ContentHash> protectedHashes;
				for (std::size_t index = 0; index < blobs.size(); ++index) if (index != missingIndex) protectedHashes.push_back(blobs[index].hash);
				const auto removed = store.Blobs().CollectUnretained(protectedHashes, 1U, nullptr);
				context.Expect(removed == std::vector<ContentHash>{blobs[missingIndex].hash}, "Actual bounded GC must remove only the chosen later source before publication.");
			}
			void ExpectAtomicRejection(TestContext& context, const TerrainTransactionReceipt& receipt) const {
				const auto after = store.CaptureRoot();
				context.Expect(receipt.outcome == TerrainEditOutcome::Rejected && receipt.code == TerrainResultCode::CorruptData,
					"Missing history source must reject through the real retention path.");
				context.Expect(receipt.retainedHistoryBytes == 0U && receipt.retainedBlobHashes.empty() && !receipt.historyPayload,
					"A rejected candidate may not publish owned history or partial retention receipts.");
				context.Expect(after.root == root && after.publicationGeneration == 1U && after.root->rootContentHash == root->rootContentHash,
					"Rejection must preserve the exact prior immutable root pointer/hash/generation.");
			}
			void ExpectNoHistoryPins(TestContext& context) {
				(void)store.Blobs().CollectUnretained({}, 8U, nullptr);
				for (const auto& blob : blobs) context.Expect(!store.Blobs().Resolve(blob.hash), "Every failed candidate prefix retain must return to its prior zero-pin state and become collectible.");
			}
		};
	}
	void RegisterCandidateRetentionRollbackTests(TestRegistry& registry) {
		for (const std::size_t missingIndex : {0U, 1U, 2U}) {
			registry.Add("Terrain.CandidateRetention.MissingSourceAfterPrefix." + std::to_string(missingIndex), [missingIndex](TestContext& context) {
				RetentionFixture fixture(context);
				const auto edit = fixture.Edit(context);
				fixture.Remove(context, missingIndex);
				const auto rejected = fixture.publication.Publish(edit);
				fixture.ExpectAtomicRejection(context, rejected);
				fixture.ExpectNoHistoryPins(context);
			});
		}
		registry.Add("Terrain.CandidateRetention.RollbackPreservesPreexistingAndUnrelatedPins", [](TestContext& context) {
			RetentionFixture fixture(context);
			context.Expect(fixture.store.Blobs().Retain(fixture.blobs.front().hash), "The fixture must establish a known preexisting A pin.");
			const std::vector<std::uint8_t> unrelatedBytes(16U, 97U);
			const auto unrelated = HashTerrainBytes(unrelatedBytes);
			context.Expect(fixture.store.Blobs().PutVerified(unrelated, unrelatedBytes) && fixture.store.Blobs().Retain(unrelated), "An unrelated baseline reader must own its own real pin.");
			const auto edit = fixture.Edit(context);
			fixture.Remove(context, 2U);
			fixture.ExpectAtomicRejection(context, fixture.publication.Publish(edit));
			fixture.store.Blobs().Release(fixture.blobs.front().hash); // Release only this fixture's baseline A pin.
			fixture.ExpectNoHistoryPins(context);
			context.Expect(fixture.store.Blobs().Resolve(unrelated).has_value(), "Rolling back a candidate prefix must not release an unrelated owner's pin.");
			fixture.store.Blobs().Release(unrelated);
			(void)fixture.store.Blobs().CollectUnretained({}, 8U, nullptr);
			context.Expect(!fixture.store.Blobs().Resolve(unrelated), "The unrelated owner must become collectible after its own release.");
		});
		registry.Add("Terrain.CandidateRetention.UndoRedoRejectionDoesNotReleaseUnacquiredPins", [](TestContext& context) {
			for (const auto origin : {TerrainEditOrigin::Undo, TerrainEditOrigin::Redo}) {
				RetentionFixture fixture(context);
				context.Expect(fixture.store.Blobs().Retain(fixture.blobs.front().hash), "A replay fixture must begin with its existing history pin.");
				const auto edit = fixture.Edit(context, origin);
				fixture.Remove(context, 2U);
				fixture.ExpectAtomicRejection(context, fixture.publication.Publish(edit));
				(void)fixture.store.Blobs().CollectUnretained({}, 8U, nullptr);
				context.Expect(fixture.store.Blobs().Resolve(fixture.blobs.front().hash).has_value(), "Replay rejection may not decrement a pin it never acquired.");
				fixture.store.Blobs().Release(fixture.blobs.front().hash);
				fixture.ExpectNoHistoryPins(context);
			}
		});
		registry.Add("Terrain.CandidateRetention.FullRetentionFailureThenRetryUsesExactOwnership", [](TestContext& context) {
			RetentionFixture fixture(context);
			const auto edit = fixture.Edit(context);
			fixture.publication.SetInjectFailureForTests(true);
			const auto rejected = fixture.publication.Publish(edit);
			context.Expect(rejected.outcome == TerrainEditOutcome::Rejected && rejected.code == TerrainResultCode::Failed
				&& fixture.store.CaptureRoot().root == fixture.root, "The existing post-retention injected failure must remain atomic.");
			fixture.ExpectNoHistoryPins(context);
			for (const auto& blob : fixture.blobs) context.Expect(fixture.store.Blobs().PutVerified(blob.hash, blob.bytes), "Retry must restore the actual immutable sources.");
			fixture.publication.SetInjectFailureForTests(false);
			const auto committed = fixture.publication.Publish(edit);
			context.Expect(committed.outcome == TerrainEditOutcome::Committed && committed.publicationGeneration == 2U
				&& committed.retainedBlobHashes.size() == 3U && committed.historyPayload, "A later valid operation must commit one deduplicated history owner per real source.");
			for (const auto hash : committed.retainedBlobHashes) fixture.store.Blobs().Release(hash);
			fixture.ExpectNoHistoryPins(context);
		});
	}
}
