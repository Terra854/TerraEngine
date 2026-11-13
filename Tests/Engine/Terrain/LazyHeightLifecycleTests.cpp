#include "TestHarness.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainPublication.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <latch>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Terrain {
	// Read-only test access observes real cache ownership; it does not impose the
	// intended cap or replace production admission/eviction with a test model.
	struct TerrainAssetStoreTestAccess {
		static std::pair<std::size_t, std::size_t> CacheOwnership(const TerrainAssetStore& store) {
			std::scoped_lock lock(store.lazyCacheMutex);
			std::size_t bytes = 0;
			for (const auto& [key, value] : store.lazyHeightCache) {
				(void)key;
				bytes += sizeof(DecodedTerrainArea) + value.decoded->canonicalPayload.capacity();
			}
			return {store.lazyHeightCache.size(), bytes};
		}
	};
}

namespace TerraEngine::Tests {
	namespace {
		using namespace Terrain;
		std::shared_ptr<TerrainAssetRoot> LazyRoot(const TerraEngine::UUID& asset, const std::vector<TerrainAreaDescriptor>& descriptors) {
			auto root = std::make_shared<TerrainAssetRoot>();
			root->assetUuid = asset;
			root->publicationGeneration = 1U;
			root->persistedRootGeneration = 1U;
			root->layerStackGeneration = 1U;
			root->rootContentHash = {103U, 107U};
			std::int64_t width = 1;
			for (const auto& descriptor : descriptors) width = std::max(width, descriptor.coordinate.x + 1);
			root->authoredBounds = {0, 0, width * kTerrainAreaExtentSamples, kTerrainAreaExtentSamples};
			TerrainDirectoryShard shard;
			shard.areaBounds = {0, 0, width, 1};
			shard.identity = {109U, 113U};
			for (const auto& descriptor : descriptors) shard.descriptors.emplace(descriptor.coordinate, descriptor);
			root->directoryShards.push_back(std::move(shard));
			return root;
		}
		TerrainAreaDescriptor LazyDescriptor(std::int64_t x, ContentHash hash = {}) {
			TerrainAreaDescriptor descriptor;
			descriptor.coordinate = {x, 0};
			descriptor.editGeneration = 1U;
			descriptor.authoredContentHash = {static_cast<std::uint64_t>(x + 1), 127U};
			if (hash.IsValid()) descriptor.channels.heightHash = hash;
			return descriptor;
		}
		std::vector<std::uint8_t> LazyPayload(std::uint32_t marker) {
			std::vector<std::uint8_t> bytes(kTerrainAreaSampleCount * sizeof(std::int32_t));
			for (unsigned index = 0; index < 4U; ++index) bytes[index] = static_cast<std::uint8_t>(marker >> (index * 8U));
			return bytes;
		}
		EncodedTerrainArea LazyEnvelope(TestContext& context, const TerraEngine::UUID& asset, TerrainAreaCoordinate area, std::span<const std::uint8_t> payload) {
			Format::TerrainAreaEnvelopeHeader header;
			header.assetUuid = asset;
			header.area = area;
			header.channelKind = TerrainChannelKind::Height;
			header.compression = Format::TerrainAreaCompression::Zstd;
			EncodedTerrainArea encoded;
			context.Expect(EncodeTerrainArea(header, payload, encoded).Succeeded(), "A valid production height envelope must encode.");
			return encoded;
		}
		struct SmallLazyFixture final {
			TerraEngine::UUID asset = GenerateUUID();
			std::vector<std::uint8_t> first = LazyPayload(229U);
			std::vector<std::uint8_t> second = LazyPayload(233U);
			std::vector<std::uint8_t> third = LazyPayload(239U);
			TerrainAssetStore store{LazyRoot(asset, {LazyDescriptor(0, HashTerrainBytes(first)), LazyDescriptor(1, HashTerrainBytes(second)), LazyDescriptor(2, HashTerrainBytes(third))})};
			explicit SmallLazyFixture(TestContext& context) {
				for (const auto* bytes : {&first, &second, &third})
					context.Expect(store.Blobs().PutVerified(HashTerrainBytes(*bytes), *bytes), "The small-budget fixture must own actual verified raw height channels.");
			}
			TerrainLazyAreaLoadReceipt Load(std::int64_t area) { return store.LoadHeightAreaLazy(store.CaptureRoot(), {area, 0}); }
		};
	}
	void RegisterLazyHeightLifecycleTests(TestRegistry& registry) {
		registry.Add("Terrain.LazyHeight.LiveTypedEditLoadsBeforePersistence", [](TestContext& context) {
			const auto asset = GenerateUUID();
			const auto original = LazyDescriptor(0);
			TerrainAssetStore store(LazyRoot(asset, {original}));
			const auto raw = LazyPayload(173U);
			const auto hash = HashTerrainBytes(raw);
			context.Expect(store.Blobs().PutVerified(hash, raw), "The resolver must own the actual canonical live bytes.");
			const auto before = store.CaptureRoot();
			TerrainEditTransaction edit;
			edit.transactionId = GenerateUUID();
			edit.targetAssetUuid = asset;
			edit.inputRootHash = before.root->rootContentHash;
			edit.expectedLayerStackGeneration = before.root->layerStackGeneration;
			edit.affectedBounds = {0, 0, 1, 1};
			edit.readSet.push_back({original.coordinate, original.editGeneration, original.authoredContentHash});
			TerrainPatch patch;
			patch.replacement = original;
			patch.replacement.editGeneration = 2U;
			patch.replacement.authoredContentHash = {181U, 191U};
			patch.replacement.channels.heightHash = hash;
			patch.preparedBlobHash = hash;
			edit.patches.push_back(patch);
			TerrainPublication publication(store);
			const auto committed = publication.Publish(edit);
			context.Expect(committed.outcome == TerrainEditOutcome::Committed, "A real typed edit must commit before testing lazy-load compatibility.");
			const auto loaded = store.LoadHeightAreaLazy(store.CaptureRoot(), {0, 0});
			context.Expect(loaded.Succeeded(), "A newly edited live raw height channel must lazy-load before any persistence conversion.");
			context.Expect(loaded.decodedHeight->canonicalPayload == raw && loaded.decodedHeight->header.assetUuid == asset
				&& loaded.decodedHeight->header.area == TerrainAreaCoordinate{0, 0} && loaded.decodedHeight->header.channelKind == TerrainChannelKind::Height,
				"The lazy receipt must preserve actual edited samples and captured metadata.");
			context.Expect(store.CaptureRoot().publicationGeneration == 2U && loaded.observedRootGeneration == 2U, "Lazy loading may not publish another authored generation.");
		});
		registry.Add("Terrain.LazyHeight.SharedRawHashKeepsAreaMetadataSeparate", [](TestContext& context) {
			const auto asset = GenerateUUID();
			const auto raw = LazyPayload(211U);
			const auto hash = HashTerrainBytes(raw);
			TerrainAssetStore store(LazyRoot(asset, {LazyDescriptor(0, hash), LazyDescriptor(1, hash)}));
			context.Expect(store.Blobs().PutVerified(hash, raw), "Both descriptors must resolve one genuine shared raw content hash.");
			const auto first = store.LoadHeightAreaLazy(store.CaptureRoot(), {0, 0});
			const auto second = store.LoadHeightAreaLazy(store.CaptureRoot(), {1, 0});
			context.Expect(first.Succeeded() && second.Succeeded(), "Shared raw bytes must load independently for both area identities.");
			context.Expect(first.decodedHeight->header.area == TerrainAreaCoordinate{0, 0}
				&& second.decodedHeight->header.area == TerrainAreaCoordinate{1, 0}, "A cache hit may not leak the first area's synthesized metadata.");
			context.Expect(first.decodedHeight->canonicalPayload == raw && second.decodedHeight->canonicalPayload == raw, "Metadata separation must preserve shared height content.");
			// Immutable captures can outlive their original source. Include the
			// captured asset identity as well as area in decoded-cache ownership.
			auto otherRoot = LazyRoot(GenerateUUID(), {LazyDescriptor(0, hash)});
			const TerrainRootCapture otherCapture{otherRoot, otherRoot->publicationGeneration};
			const auto other = store.LoadHeightAreaLazy(otherCapture, {0, 0});
			context.Expect(other.Succeeded() && other.decodedHeight->header.assetUuid == otherRoot->assetUuid
				&& first.decodedHeight->header.assetUuid == asset, "Shared raw content at the same coordinate must not leak metadata between captured asset identities.");
		});
		registry.Add("Terrain.LazyHeight.CorruptEnvelopeCannotBecomeRawFallback", [](TestContext& context) {
			const auto asset = GenerateUUID();
			auto bytes = LazyPayload(0U);
			std::copy(Format::kTerrainAreaMagic.begin(), Format::kTerrainAreaMagic.end(), bytes.begin());
			const auto hash = HashTerrainBytes(bytes);
			TerrainAssetStore store(LazyRoot(asset, {LazyDescriptor(0, hash)}));
			context.Expect(store.Blobs().PutVerified(hash, bytes), "The corrupt-envelope fixture must still have a verified raw byte hash.");
			const auto before = store.CaptureRoot();
			const auto rejected = store.LoadHeightAreaLazy(before, {0, 0});
			context.Expect(!rejected.Succeeded() && !rejected.decodedHeight, "Envelope-shaped malformed bytes must reject rather than enter raw fallback.");
			context.Expect(TerrainAssetStoreTestAccess::CacheOwnership(store).first == 0U && store.CaptureRoot().root == before.root, "Rejected decoding must retain neither cache bytes nor a partial authored root.");
		});
		registry.Add("Terrain.LazyHeight.DefaultRetentionIsBoundedWithOutstandingReaders", [](TestContext& context) {
			const auto asset = GenerateUUID();
			std::vector<TerrainAreaDescriptor> descriptors;
			std::vector<EncodedTerrainArea> envelopes;
			// This bounded count exceeds the approved default entry limit by one.
			// Zstd keeps the immutable resolver fixtures small; real decoded payloads
			// exercise both the 256-entry and 64 MiB ownership limits.
			for (unsigned index = 0; index < 257U; ++index) {
				auto envelope = LazyEnvelope(context, asset, {index, 0}, LazyPayload(index + 1U));
				descriptors.push_back(LazyDescriptor(index, envelope.header.envelopeHash));
				envelopes.push_back(std::move(envelope));
			}
			TerrainAssetStore store(LazyRoot(asset, descriptors));
			for (const auto& envelope : envelopes) context.Expect(store.Blobs().PutVerified(envelope.header.envelopeHash, envelope.bytes), "The resolver must verify each real envelope identity.");
			std::shared_ptr<const DecodedTerrainArea> reader;
			std::weak_ptr<const DecodedTerrainArea> firstOwner;
			for (unsigned index = 0; index < 257U; ++index) {
				const auto loaded = store.LoadHeightAreaLazy(store.CaptureRoot(), {index, 0});
				context.Expect(loaded.Succeeded(), "Every bounded test area must decode through the actual lazy loader.");
				if (index == 0U) { reader = loaded.decodedHeight; firstOwner = reader; }
			}
			context.Expect(reader && reader->canonicalPayload == LazyPayload(1U), "Outstanding immutable readers must remain valid across cache eviction.");
			const auto [entries, bytes] = TerrainAssetStoreTestAccess::CacheOwnership(store);
			context.Expect(entries <= 256U && bytes <= 64U * 1024U * 1024U,
				"Actual cache ownership must obey both default limits; entries=" + std::to_string(entries) + ", bytes=" + std::to_string(bytes));
			reader.reset();
			context.Expect(firstOwner.expired(), "The least recently used decoded object must be released after its final external reader exits.");
		});
		registry.Add("Terrain.LazyHeight.CancellationAndEnvelopeRevisitPreserveOwnership", [](TestContext& context) {
			const auto asset = GenerateUUID();
			const auto encoded = LazyEnvelope(context, asset, {0, 0}, LazyPayload(223U));
			TerrainAssetStore store(LazyRoot(asset, {LazyDescriptor(0, encoded.header.envelopeHash)}));
			context.Expect(store.Blobs().PutVerified(encoded.header.envelopeHash, encoded.bytes), "The cancellation fixture must contain a valid verified envelope.");
			std::atomic_bool cancelled{true};
			const auto rejected = store.LoadHeightAreaLazy(store.CaptureRoot(), {0, 0}, &cancelled);
			context.Expect(rejected.code == TerrainResultCode::Cancelled && !rejected.decodedHeight && TerrainAssetStoreTestAccess::CacheOwnership(store).first == 0U, "Cancelled work must not populate the decoded cache.");
			const auto first = store.LoadHeightAreaLazy(store.CaptureRoot(), {0, 0});
			const auto second = store.LoadHeightAreaLazy(store.CaptureRoot(), {0, 0});
			context.Expect(first.Succeeded() && second.Succeeded() && second.fromCache && first.decodedHeight == second.decodedHeight, "An ordinary verified envelope revisit must reuse actual immutable decoded ownership.");
		});
		registry.Add("Terrain.LazyHeight.SmallBudgetEvictsLeastRecentOwnerOnly", [](TestContext& context) {
			SmallLazyFixture fixture(context);
			fixture.store.SetLazyHeightCachePolicy({2U, 64U * 1024U * 1024U});
			const auto first = fixture.Load(0);
			auto second = fixture.Load(1);
			const std::weak_ptr<const DecodedTerrainArea> secondOwner = second.decodedHeight;
			const auto revisit = fixture.Load(0);
			context.Expect(first.Succeeded() && second.Succeeded() && revisit.fromCache && revisit.decodedHeight == first.decodedHeight, "A real revisit must promote the existing first owner.");
			const auto third = fixture.Load(2);
			context.Expect(third.Succeeded() && second.decodedHeight->canonicalPayload == fixture.second, "An evicted reader must retain valid immutable samples.");
			second.decodedHeight.reset();
			context.Expect(secondOwner.expired(), "The least-recent second owner must release after its external reader exits.");
			context.Expect(fixture.Load(0).fromCache && !fixture.Load(1).fromCache, "LRU must retain the promoted first page and re-decode the evicted second page.");
			const auto stats = fixture.store.GetLazyHeightCacheStats();
			const auto actual = TerrainAssetStoreTestAccess::CacheOwnership(fixture.store);
			context.Expect(stats.retainedEntries == 2U && stats.evictions >= 2U && actual.first == stats.retainedEntries && actual.second == stats.retainedBytes, "Copied statistics must describe actual retained ownership under the entry cap.");
		});
		registry.Add("Terrain.LazyHeight.ByteBudgetAndPolicyChangesTrimImmediately", [](TestContext& context) {
			SmallLazyFixture fixture(context);
			fixture.store.SetLazyHeightCachePolicy({8U, 64U * 1024U * 1024U});
			const auto first = fixture.Load(0);
			context.Expect(first.Succeeded(), "The accounting baseline must decode successfully.");
			const auto oneOwnerBytes = fixture.store.GetLazyHeightCacheStats().retainedBytes;
			context.Expect(oneOwnerBytes >= first.decodedHeight->canonicalPayload.size() + sizeof(DecodedTerrainArea), "Byte accounting must include decoded capacity and fixed object storage.");
			fixture.store.SetLazyHeightCachePolicy({8U, oneOwnerBytes});
			const auto second = fixture.Load(1);
			context.Expect(second.Succeeded() && first.decodedHeight->canonicalPayload == fixture.first, "A byte eviction must preserve external readers.");
			const auto bounded = fixture.store.GetLazyHeightCacheStats();
			context.Expect(bounded.retainedEntries == 1U && bounded.retainedBytes <= oneOwnerBytes, "The byte cap must apply even when the entry cap permits more pages.");
			fixture.store.SetLazyHeightCachePolicy({0U, oneOwnerBytes});
			const auto empty = fixture.store.GetLazyHeightCacheStats();
			const auto policy = fixture.store.GetLazyHeightCachePolicy();
			context.Expect(empty.retainedEntries == 0U && empty.retainedBytes == 0U && policy.maximumEntries == 0U && policy.maximumBytes == oneOwnerBytes, "A synchronized policy change must immediately trim existing cache ownership.");
		});
		registry.Add("Terrain.LazyHeight.ZeroLimitsDisableRetention", [](TestContext& context) {
			SmallLazyFixture fixture(context);
			for (const TerrainLazyHeightCachePolicy policy : {TerrainLazyHeightCachePolicy{0U, 64U * 1024U * 1024U}, TerrainLazyHeightCachePolicy{8U, 0U}}) {
				fixture.store.SetLazyHeightCachePolicy(policy);
				const auto first = fixture.Load(0);
				const auto second = fixture.Load(0);
				context.Expect(first.Succeeded() && second.Succeeded() && !first.fromCache && !second.fromCache && first.decodedHeight != second.decodedHeight, "Disabled retention must still return independent valid decoded readers.");
				const auto actual = TerrainAssetStoreTestAccess::CacheOwnership(fixture.store);
				context.Expect(actual.first == 0U && actual.second == 0U, "Zero in either limit must leave no hidden cache owners.");
			}
		});
		registry.Add("Terrain.LazyHeight.OversizedPageReturnsUncached", [](TestContext& context) {
			SmallLazyFixture fixture(context);
			fixture.store.SetLazyHeightCachePolicy({8U, sizeof(DecodedTerrainArea)});
			const auto first = fixture.Load(0);
			const auto second = fixture.Load(0);
			context.Expect(first.Succeeded() && second.Succeeded() && first.decodedHeight->canonicalPayload == fixture.first
				&& !first.fromCache && !second.fromCache && first.decodedHeight != second.decodedHeight, "A page larger than its cache byte budget must remain usable without hidden retention.");
			context.Expect(fixture.store.GetLazyHeightCacheStats().retainedEntries == 0U, "Oversized uncached reads may not exceed the policy.");
		});
		registry.Add("Terrain.LazyHeight.ConcurrentDuplicateAdmissionHasOneOwner", [](TestContext& context) {
			SmallLazyFixture fixture(context);
			fixture.store.SetLazyHeightCachePolicy({8U, 64U * 1024U * 1024U});
			std::vector<TerrainLazyAreaLoadReceipt> receipts(8U);
			std::vector<std::exception_ptr> failures(8U);
			std::latch start(1);
			std::vector<std::jthread> workers;
			workers.reserve(receipts.size());
			try {
				for (std::size_t index = 0; index < receipts.size(); ++index) workers.emplace_back([&, index] {
					start.wait();
					try { receipts[index] = fixture.Load(0); } catch (...) { failures[index] = std::current_exception(); }
				});
			} catch (...) { start.count_down(); throw; }
			start.count_down();
			for (auto& worker : workers) worker.join();
			for (std::size_t index = 0; index < receipts.size(); ++index) {
				context.Expect(!failures[index] && receipts[index].Succeeded(), "Every concurrent production lazy read must finish with a valid receipt.");
				context.Expect(receipts[index].decodedHeight == receipts.front().decodedHeight, "Concurrent duplicate admission must converge on one actual immutable owner.");
			}
			const auto stats = fixture.store.GetLazyHeightCacheStats();
			context.Expect(stats.retainedEntries == 1U && stats.hits + stats.misses == receipts.size()
				&& TerrainAssetStoreTestAccess::CacheOwnership(fixture.store).first == 1U, "Concurrent requests must not duplicate accounting or retained map entries.");
		});
		registry.Add("Terrain.LazyHeight.WrongEnvelopeMetadataNeverEntersCache", [](TestContext& context) {
			const auto asset = GenerateUUID();
			for (const bool wrongAsset : {false, true}) {
				const auto encoded = LazyEnvelope(context, wrongAsset ? GenerateUUID() : asset, wrongAsset ? TerrainAreaCoordinate{0, 0} : TerrainAreaCoordinate{1, 0}, LazyPayload(241U));
				TerrainAssetStore store(LazyRoot(asset, {LazyDescriptor(0, encoded.header.envelopeHash)}));
				context.Expect(store.Blobs().PutVerified(encoded.header.envelopeHash, encoded.bytes), "The mismatch case must use a valid authenticated envelope.");
				const auto before = store.CaptureRoot();
				const auto rejected = store.LoadHeightAreaLazy(before, {0, 0});
				context.Expect(!rejected.Succeeded() && rejected.code == TerrainResultCode::CorruptData && store.GetLazyHeightCacheStats().retainedEntries == 0U
					&& store.CaptureRoot().root == before.root, "Valid bytes with wrong captured metadata must reject without cache or root mutation.");
			}
		});
	}
}
