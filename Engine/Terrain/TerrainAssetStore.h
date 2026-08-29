// TerrainAssetStore.h -- Immutable authored terrain roots and blob lookup.

#pragma once

#include "Terrain/TerrainTypes.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainLayerStack.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Terrain
{
	struct TerrainChannelSnapshot
	{
		std::optional<ContentHash> heightHash;
		std::optional<ContentHash> holeHash;
		std::map<std::uint16_t, ContentHash> weightGroupHashes;
	};

	struct TerrainAreaDescriptor
	{
		TerrainAreaCoordinate coordinate{};
		Generation editGeneration = 0u;
		TerrainChannelSnapshot channels{};
		std::int32_t minHeightMm = 0;
		std::int32_t maxHeightMm = 0;
		std::vector<std::uint16_t> layerPresence;
		ContentHash authoredContentHash{};
	};

	struct TerrainDirectoryShard
	{
		TerrainSampleBounds areaBounds{};
		ContentHash identity{};
		std::map<TerrainAreaCoordinate, TerrainAreaDescriptor> descriptors;
	};

	struct TerrainAssetRoot
	{
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		Generation publicationGeneration = 0u;
		Generation persistedRootGeneration = 0u;
		ContentHash rootContentHash{};
		double sampleSpacingMetres = 1.0;
		TerrainSampleBounds authoredBounds{};
		std::int32_t defaultHeightMm = 0;
		std::uint16_t defaultLayerId = 1u;
		Generation layerStackGeneration = 0u;
		// Metadata is immutable with the root. Older persisted roots may omit this
		// pointer and use defaultLayerId until migrated by a metadata transaction.
		std::shared_ptr<const TerrainLayerStack> layerStack;
		std::vector<TerrainDirectoryShard> directoryShards;
	};

	struct TerrainRootCapture
	{
		std::shared_ptr<const TerrainAssetRoot> root;
		Generation publicationGeneration = 0u;
		[[nodiscard]] bool IsValid() const noexcept { return root != nullptr && publicationGeneration != 0u; }
	};

	// The root identity is the hash of these exact canonical bytes. The hash is
	// deliberately absent from the document, avoiding the old self-hash cycle.
	struct TerrainRootCodecResult
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		TerrainRootDiagnosticCode diagnostic = TerrainRootDiagnosticCode::None;
		std::optional<TerrainAssetRoot> root;
		std::string canonicalBytes;
		ContentHash contentHash{};
		std::string message;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok && contentHash.IsValid(); }
		[[nodiscard]] explicit operator bool() const noexcept { return root.has_value(); }
		[[nodiscard]] const TerrainAssetRoot* operator->() const noexcept { return root ? &*root : nullptr; }
		[[nodiscard]] TerrainAssetRoot* operator->() noexcept { return root ? &*root : nullptr; }
		[[nodiscard]] const TerrainAssetRoot& operator*() const { return *root; }
		[[nodiscard]] TerrainAssetRoot& operator*() { return *root; }
	};

	[[nodiscard]] TERRAENGINE_API TerrainRootCodecResult EncodeTerrainRootManifest(const TerrainAssetRoot& root);
	[[nodiscard]] TERRAENGINE_API TerrainRootCodecResult DecodeTerrainRootManifest(std::string_view text);

	// Persistence tracks saved identity separately from the live publication
	// generation. Saving never rewrites a root that workers may still read.
	struct TerrainPersistedRootState
	{
		ContentHash savedRootHash{};
		ContentHash predecessorRootHash{};
		Generation savedPublicationGeneration = 0u;
		bool recoveredPredecessor = false;
	};

	enum class TerrainPersistenceFailurePoint : std::uint8_t
	{
		None,
		BlobWrite,
		DirectoryWrite,
		ManifestWrite,
		DurableFlush,
		CurrentReplacement
	};

	struct TerrainPersistenceRequest
	{
		std::filesystem::path dataDirectory;
		const std::atomic_bool* cancellation = nullptr;
		TerrainPersistenceFailurePoint injectFailureForTests = TerrainPersistenceFailurePoint::None;
	};

	struct TerrainPersistenceReceipt
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		TerrainRootDiagnosticCode diagnostic = TerrainRootDiagnosticCode::None;
		ContentHash savedRootHash{};
		ContentHash predecessorRootHash{};
		bool usedPredecessor = false;
		std::vector<ContentHash> orphanedBlobHashes;
		std::string message;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok; }
	};

	// A lazy read result keeps decoded bytes detached from the live root. It is
	// safe to prepare on a worker and is rejected by residency publication when
	// its observed root generation is no longer current.
	struct TerrainLazyAreaLoadReceipt
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		TerrainAreaCoordinate area{};
		Generation observedRootGeneration = 0u;
		ContentHash sourceHash{};
		std::shared_ptr<const DecodedTerrainArea> decodedHeight;
		bool fromCache = false;
		std::string message;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok && decodedHeight != nullptr; }
	};

	// In-memory byte source gives later file/TEPAK loaders one narrow interface.
	// It deliberately returns a copy so callers cannot mutate immutable blobs.
	class TERRAENGINE_API TerrainBlobResolver
	{
	public:
		bool PutVerified(ContentHash hash, std::vector<std::uint8_t> bytes);
		[[nodiscard]] std::optional<std::vector<std::uint8_t>> Resolve(ContentHash hash) const;
		[[nodiscard]] std::size_t BlobCount() const;
		[[nodiscard]] bool Retain(ContentHash hash);
		void Release(ContentHash hash) noexcept;
		[[nodiscard]] std::vector<ContentHash> SnapshotHashes() const;
		[[nodiscard]] std::vector<ContentHash> CollectUnretained(const std::vector<ContentHash>& protectedHashes, std::size_t maximumRemovals, const std::atomic_bool* cancellation);

	private:
		mutable std::shared_mutex mutex;
		std::unordered_map<ContentHash, std::vector<std::uint8_t>, ContentHashHasher> blobs;
		std::unordered_map<ContentHash, std::size_t, ContentHashHasher> retentionCounts;
	};

	class TERRAENGINE_API TerrainAssetStore
	{
	public:
		explicit TerrainAssetStore(std::shared_ptr<const TerrainAssetRoot> initialRoot);
		[[nodiscard]] TerrainRootCapture CaptureRoot() const;
		[[nodiscard]] std::optional<TerrainAreaDescriptor> FindDescriptor(const TerrainAssetRoot& root, TerrainAreaCoordinate coordinate) const;
		[[nodiscard]] TerrainAreaDescriptor ResolveDescriptorOrImplicit(const TerrainAssetRoot& root, TerrainAreaCoordinate coordinate) const;
		[[nodiscard]] TerrainCodecResult ValidateRoot(const TerrainAssetRoot& root) const;
		[[nodiscard]] TerrainCodecResult BuildCandidateRoot(
			const TerrainAssetRoot& base,
			const std::vector<TerrainAreaDescriptor>& replacements,
			ContentHash candidateRootHash,
			std::shared_ptr<const TerrainAssetRoot>& outCandidate) const;
		// Resize uses one candidate builder so bounds, descriptor membership, and
		// directory identities can never become visible as separate generations.
		[[nodiscard]] TerrainCodecResult BuildResizeCandidate(
			const TerrainAssetRoot& base, TerrainSampleBounds proposedBounds,
			const std::vector<TerrainAreaDescriptor>& upserts,
			const std::vector<TerrainAreaCoordinate>& removals,
			std::shared_ptr<const TerrainAssetRoot>& outCandidate) const;
		// Expansion changes declared coverage only. Existing immutable areas are
		// copied verbatim; callers choose which newly covered areas to materialize.
		[[nodiscard]] TerrainCodecResult ExpandAuthoredBounds(
			const TerrainAssetRoot& base,
			TerrainSampleBounds expandedBounds,
			ContentHash candidateRootHash,
			std::shared_ptr<const TerrainAssetRoot>& outCandidate) const;
		[[nodiscard]] TerrainCodecResult MaterializeImplicitArea(
			const TerrainAssetRoot& base,
			TerrainAreaDescriptor materialized,
			ContentHash candidateRootHash,
			std::shared_ptr<const TerrainAssetRoot>& outCandidate) const;
		[[nodiscard]] TerrainCodecResult BuildLayerStackCandidate(
			const TerrainAssetRoot& base, const TerrainLayerPatch& patch,
			ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const;
		[[nodiscard]] TerrainCodecResult PublishCandidate(std::shared_ptr<const TerrainAssetRoot> expectedBase, std::shared_ptr<const TerrainAssetRoot> candidate);
		// Worker-safe persistence stages only copied roots and copied blob bytes.
		// CURRENT changes last, after all referenced files validate and flush.
		[[nodiscard]] TerrainPersistenceReceipt PersistAtomically(const TerrainPersistenceRequest& request);
		// Opening rehydrates the saved/predecessor snapshots before editor code
		// creates history or a new save, preserving CURRENT recovery semantics.
		[[nodiscard]] TerrainPersistenceReceipt OpenPersistedRoot(const std::filesystem::path& dataDirectory);
		[[nodiscard]] TerrainPersistenceReceipt RecoverPersistedRoot(const std::filesystem::path& dataDirectory);
		[[nodiscard]] TerrainPersistedRootState CapturePersistedRootState() const;
		[[nodiscard]] std::vector<ContentHash> CollectReferencedBlobHashes(const TerrainAssetRoot& root) const;
		[[nodiscard]] std::vector<ContentHash> CollectPersistedReferencedBlobHashes() const;
		// Loads exactly one height channel on demand. Directory lookup remains
		// sparse, decompression is bounded by the codec, and cancellation is
		// checked before and after the expensive decode step.
		[[nodiscard]] TerrainLazyAreaLoadReceipt LoadHeightAreaLazy(
			const TerrainRootCapture& capture,
			TerrainAreaCoordinate coordinate,
			const std::atomic_bool* cancellation = nullptr);

		TerrainBlobResolver& Blobs() noexcept { return blobResolver; }
		const TerrainBlobResolver& Blobs() const noexcept { return blobResolver; }

	private:
		mutable std::shared_mutex rootMutex;
		std::shared_ptr<const TerrainAssetRoot> currentRoot;
		TerrainBlobResolver blobResolver;
		mutable std::mutex lazyCacheMutex;
		std::unordered_map<ContentHash, std::shared_ptr<const DecodedTerrainArea>, ContentHashHasher> lazyHeightCache;
		mutable std::mutex persistenceMutex;
		TerrainPersistedRootState persistedRoot;
		std::shared_ptr<const TerrainAssetRoot> savedRootSnapshot;
		std::shared_ptr<const TerrainAssetRoot> predecessorRootSnapshot;
	};
}
