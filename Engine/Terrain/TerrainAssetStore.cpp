// TerrainAssetStore.cpp -- Copy-on-write terrain directory implementation.

#include "Terrain/TerrainAssetStore.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace TerraEngine::Terrain
{
	namespace
	{
		using HashSet = std::unordered_set<ContentHash, ContentHashHasher>;

		bool IsCancelled(const std::atomic_bool* cancellation) noexcept
		{
			return cancellation != nullptr && cancellation->load(std::memory_order_acquire);
		}

		bool WriteDurableTextFile(const std::filesystem::path& path, const std::string& text)
		{
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (!output) return false;
			output.write(text.data(), static_cast<std::streamsize>(text.size()));
			output.flush();
			if (!output.good()) return false;
			output.close();
		#ifdef _WIN32
			const HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return false;
			const bool flushed = FlushFileBuffers(handle) != FALSE;
			CloseHandle(handle);
			return flushed;
		#else
			return true;
		#endif
		}

		bool WriteDurableBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
		{
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (!output) return false;
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			output.flush();
			if (!output.good()) return false;
			output.close();
		#ifdef _WIN32
			const HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return false;
			const bool flushed = FlushFileBuffers(handle) != FALSE;
			CloseHandle(handle);
			return flushed;
		#else
			return true;
		#endif
		}

		std::optional<std::vector<std::uint8_t>> ReadBinaryFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input) return std::nullopt;
			input.seekg(0, std::ios::end);
			const std::streamoff size = input.tellg();
			// Root/directory manifests may describe many fixed-size areas, while an
			// individual .tearea remains smaller. Keep a hard but practical file cap.
			if (size <= 0 || size > static_cast<std::streamoff>(64u * 1024u * 1024u)) return std::nullopt;
			input.seekg(0, std::ios::beg);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			input.read(reinterpret_cast<char*>(bytes.data()), size);
			return input ? std::optional<std::vector<std::uint8_t>>{std::move(bytes)} : std::nullopt;
		}

		bool ReplaceCurrentAtomically(const std::filesystem::path& temporary, const std::filesystem::path& current)
		{
		#ifdef _WIN32
			// MOVEFILE_WRITE_THROUGH is the platform durability boundary for the
			// tiny pointer file. The immutable manifest has already been flushed.
			return MoveFileExW(temporary.wstring().c_str(), current.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		#else
			std::error_code error;
			std::filesystem::rename(temporary, current, error);
			return !error;
		#endif
		}

		std::string EncodeRootManifest(const TerrainAssetRoot& root)
		{
			std::ostringstream text;
			text << "TERRAINROOT2\n";
			text << TerraEngine::ToHexString(root.assetUuid) << ' ' << root.publicationGeneration << ' ' << root.persistedRootGeneration << ' '
				<< ToHexString(root.rootContentHash) << ' ' << root.authoredBounds.minX << ' ' << root.authoredBounds.minZ << ' '
				<< root.authoredBounds.maxXExclusive << ' ' << root.authoredBounds.maxZExclusive << ' ' << root.defaultHeightMm << ' '
				<< root.defaultLayerId << ' ' << root.layerStackGeneration << '\n';
			text << "LAYERS " << (root.layerStack ? root.layerStack->layers.size() : 0u) << '\n';
			if (root.layerStack)
				for (const TerrainLayer& layer : root.layerStack->layers)
					text << layer.stableId << ' ' << std::quoted(layer.name) << ' ' << TerraEngine::ToHexString(layer.materialAssetUuid) << ' '
						<< layer.mapping.scaleX << ' ' << layer.mapping.scaleZ << ' ' << layer.mapping.offsetX << ' ' << layer.mapping.offsetZ << ' ' << layer.mapping.rotationRadians << ' '
						<< layer.enabled << ' ' << static_cast<std::uint32_t>(layer.fallbackState) << '\n';
			text << root.directoryShards.size() << '\n';
			for (const TerrainDirectoryShard& shard : root.directoryShards)
			{
				text << shard.areaBounds.minX << ' ' << shard.areaBounds.minZ << ' ' << shard.areaBounds.maxXExclusive << ' '
					<< shard.areaBounds.maxZExclusive << ' ' << ToHexString(shard.identity) << ' ' << shard.descriptors.size() << '\n';
				for (const auto& [coordinate, descriptor] : shard.descriptors)
				{
					text << coordinate.x << ' ' << coordinate.z << ' ' << descriptor.editGeneration << ' ' << descriptor.minHeightMm << ' '
						<< descriptor.maxHeightMm << ' ' << ToHexString(descriptor.authoredContentHash) << ' '
						<< (descriptor.channels.heightHash ? ToHexString(*descriptor.channels.heightHash) : "-") << ' '
						<< (descriptor.channels.holeHash ? ToHexString(*descriptor.channels.holeHash) : "-") << ' '
						<< descriptor.channels.weightGroupHashes.size();
					for (const auto& [group, hash] : descriptor.channels.weightGroupHashes) text << ' ' << group << ' ' << ToHexString(hash);
					text << '\n';
				}
			}
			return text.str();
		}

		std::optional<ContentHash> ParseContentHash(const std::string& text)
		{
			if (text.size() != 32u) return std::nullopt;
			auto parseHalf = [&](std::size_t offset) -> std::optional<std::uint64_t>
			{
				std::uint64_t value = 0u;
				const auto result = std::from_chars(text.data() + offset, text.data() + offset + 16u, value, 16);
				return result.ec == std::errc{} && result.ptr == text.data() + offset + 16u ? std::optional<std::uint64_t>{value} : std::nullopt;
			};
			// ContentHash text is canonical high word first (see ToHexString), while
			// the typed storage keeps low64 first for xxHash interop.
			const auto high = parseHalf(0u); const auto low = parseHalf(16u);
			return low && high ? std::optional<ContentHash>{{*low, *high}} : std::nullopt;
		}

		std::optional<TerrainAssetRoot> DecodeRootManifest(const std::string& text)
		{
			std::istringstream input(text); std::string magic;
			if (!(input >> magic) || magic != "TERRAINROOT2") return std::nullopt;
			TerrainAssetRoot root{}; std::string assetText; std::string rootHash;
			if (!(input >> assetText >> root.publicationGeneration >> root.persistedRootGeneration >> rootHash >> root.authoredBounds.minX >> root.authoredBounds.minZ >> root.authoredBounds.maxXExclusive >> root.authoredBounds.maxZExclusive >> root.defaultHeightMm >> root.defaultLayerId >> root.layerStackGeneration)) return std::nullopt;
			const auto asset = TerraEngine::ParseUUID(assetText); const auto hash = ParseContentHash(rootHash);
			if (!asset || !hash) return std::nullopt;
			root.assetUuid = *asset; root.rootContentHash = *hash;
			std::string nextToken; if (!(input >> nextToken)) return std::nullopt;
			std::size_t shardCount = 0u;
			if (nextToken == "LAYERS")
			{
				std::size_t layerCount = 0u;
				if (!(input >> layerCount) || layerCount > Format::kTerrainMaximumLayers) return std::nullopt;
				if (layerCount != 0u)
				{
					TerrainLayerStack layers{}; layers.generation = root.layerStackGeneration;
					for (std::size_t index = 0u; index < layerCount; ++index)
					{
						TerrainLayer layer{}; std::string material; std::uint32_t fallback = 0u;
						if (!(input >> layer.stableId >> std::quoted(layer.name) >> material >> layer.mapping.scaleX >> layer.mapping.scaleZ >> layer.mapping.offsetX >> layer.mapping.offsetZ >> layer.mapping.rotationRadians >> layer.enabled >> fallback)) return std::nullopt;
						const auto materialId = TerraEngine::ParseUUID(material);
						if (!materialId || fallback > static_cast<std::uint32_t>(TerrainLayerFallbackState::FallbackActive)) return std::nullopt;
						layer.materialAssetUuid = *materialId; layer.fallbackState = static_cast<TerrainLayerFallbackState>(fallback); layers.layers.push_back(std::move(layer));
					}
					if (!ValidateTerrainLayerStack(layers).Succeeded()) return std::nullopt;
					root.layerStack = std::make_shared<const TerrainLayerStack>(std::move(layers));
				}
				if (!(input >> shardCount)) return std::nullopt;
			}
			else
			{
				const auto parsed = std::from_chars(nextToken.data(), nextToken.data() + nextToken.size(), shardCount);
				if (parsed.ec != std::errc{} || parsed.ptr != nextToken.data() + nextToken.size()) return std::nullopt;
			}
			if (shardCount > 65536u) return std::nullopt;
			root.directoryShards.reserve(shardCount);
			for (std::size_t shardIndex = 0u; shardIndex < shardCount; ++shardIndex)
			{
				TerrainDirectoryShard shard{}; std::string identity; std::size_t descriptorCount = 0u;
				if (!(input >> shard.areaBounds.minX >> shard.areaBounds.minZ >> shard.areaBounds.maxXExclusive >> shard.areaBounds.maxZExclusive >> identity >> descriptorCount) || descriptorCount > 1048576u) return std::nullopt;
				const auto parsedIdentity = ParseContentHash(identity); if (!parsedIdentity) return std::nullopt; shard.identity = *parsedIdentity;
				for (std::size_t descriptorIndex = 0u; descriptorIndex < descriptorCount; ++descriptorIndex)
				{
					TerrainAreaDescriptor descriptor{}; std::string descriptorHash; std::string heightHash; std::string holeHash; std::size_t weightCount = 0u;
					if (!(input >> descriptor.coordinate.x >> descriptor.coordinate.z >> descriptor.editGeneration >> descriptor.minHeightMm >> descriptor.maxHeightMm >> descriptorHash >> heightHash >> holeHash >> weightCount) || weightCount > kTerrainMaximumWeightGroups) return std::nullopt;
					const auto parsedDescriptorHash = ParseContentHash(descriptorHash); if (!parsedDescriptorHash) return std::nullopt; descriptor.authoredContentHash = *parsedDescriptorHash;
					if (heightHash != "-") { const auto parsed = ParseContentHash(heightHash); if (!parsed) return std::nullopt; descriptor.channels.heightHash = *parsed; }
					if (holeHash != "-") { const auto parsed = ParseContentHash(holeHash); if (!parsed) return std::nullopt; descriptor.channels.holeHash = *parsed; }
					for (std::size_t weight = 0u; weight < weightCount; ++weight) { std::uint16_t group = 0u; std::string weightHash; if (!(input >> group >> weightHash)) return std::nullopt; const auto parsed = ParseContentHash(weightHash); if (!parsed || group == 0u) return std::nullopt; descriptor.channels.weightGroupHashes.emplace(group, *parsed); }
					if (!shard.descriptors.emplace(descriptor.coordinate, std::move(descriptor)).second) return std::nullopt;
				}
				root.directoryShards.push_back(std::move(shard));
			}
			return root;
		}
	}

	std::optional<TerrainAssetRoot> DecodeTerrainRootManifest(std::string_view text)
	{
		return DecodeRootManifest(std::string(text));
	}

	bool TerrainBlobResolver::PutVerified(ContentHash hash, std::vector<std::uint8_t> bytes)
	{
		if (!hash.IsValid() || bytes.empty() || HashTerrainBytes(bytes) != hash) return false;
		std::unique_lock lock(mutex); const auto found = blobs.find(hash);
		if (found != blobs.end()) return found->second == bytes;
		blobs.emplace(hash, std::move(bytes)); return true;
	}

	std::optional<std::vector<std::uint8_t>> TerrainBlobResolver::Resolve(ContentHash hash) const
	{
		std::shared_lock lock(mutex); const auto found = blobs.find(hash);
		return found == blobs.end() ? std::nullopt : std::optional<std::vector<std::uint8_t>>{found->second};
	}

	std::size_t TerrainBlobResolver::BlobCount() const { std::shared_lock lock(mutex); return blobs.size(); }
	bool TerrainBlobResolver::Retain(ContentHash hash)
	{
		std::unique_lock lock(mutex); if (!blobs.contains(hash)) return false; ++retentionCounts[hash]; return true;
	}
	void TerrainBlobResolver::Release(ContentHash hash) noexcept
	{
		std::unique_lock lock(mutex); const auto found = retentionCounts.find(hash); if (found == retentionCounts.end()) return; if (found->second > 1u) --found->second; else retentionCounts.erase(found);
	}
	std::vector<ContentHash> TerrainBlobResolver::SnapshotHashes() const
	{
		std::shared_lock lock(mutex); std::vector<ContentHash> result; result.reserve(blobs.size()); for (const auto& [hash, bytes] : blobs) result.push_back(hash); return result;
	}
	std::vector<ContentHash> TerrainBlobResolver::CollectUnretained(const std::vector<ContentHash>& protectedHashes, std::size_t maximumRemovals, const std::atomic_bool* cancellation)
	{
		HashSet protectedSet(protectedHashes.begin(), protectedHashes.end()); std::vector<ContentHash> removed; std::unique_lock lock(mutex);
		for (auto iterator = blobs.begin(); iterator != blobs.end() && removed.size() < maximumRemovals; )
		{
			if (IsCancelled(cancellation)) break;
			if (protectedSet.contains(iterator->first) || retentionCounts.contains(iterator->first)) { ++iterator; continue; }
			removed.push_back(iterator->first); iterator = blobs.erase(iterator);
		}
		return removed;
	}

	TerrainAssetStore::TerrainAssetStore(std::shared_ptr<const TerrainAssetRoot> initialRoot) : currentRoot(std::move(initialRoot)) {}

	TerrainRootCapture TerrainAssetStore::CaptureRoot() const
	{
		std::shared_lock lock(rootMutex); return {currentRoot, currentRoot ? currentRoot->publicationGeneration : 0u};
	}

	std::optional<TerrainAreaDescriptor> TerrainAssetStore::FindDescriptor(const TerrainAssetRoot& root, TerrainAreaCoordinate coordinate) const
	{
		for (const TerrainDirectoryShard& shard : root.directoryShards)
		{
			if (!shard.areaBounds.Contains({coordinate.x, coordinate.z})) continue;
			const auto found = shard.descriptors.find(coordinate);
			if (found != shard.descriptors.end()) return found->second;
		}
		return std::nullopt;
	}

	TerrainLazyAreaLoadReceipt TerrainAssetStore::LoadHeightAreaLazy(
		const TerrainRootCapture& capture,
		TerrainAreaCoordinate coordinate,
		const std::atomic_bool* cancellation)
	{
		TerrainLazyAreaLoadReceipt receipt{};
		receipt.area = coordinate;
		receipt.observedRootGeneration = capture.publicationGeneration;
		if (!capture.IsValid())
		{
			receipt.code = TerrainResultCode::InvalidArgument;
			receipt.message = "Lazy terrain loading requires an immutable root capture.";
			return receipt;
		}
		if (cancellation != nullptr && cancellation->load(std::memory_order_acquire))
		{
			receipt.code = TerrainResultCode::Cancelled;
			receipt.message = "Lazy terrain loading was cancelled before blob lookup.";
			return receipt;
		}
		const auto descriptor = FindDescriptor(*capture.root, coordinate);
		if (!descriptor.has_value() || !descriptor->channels.heightHash.has_value())
		{
			receipt.code = TerrainResultCode::Failed;
			receipt.message = "The requested terrain area has no materialized height channel.";
			return receipt;
		}
		receipt.sourceHash = *descriptor->channels.heightHash;
		{
			std::scoped_lock lock(lazyCacheMutex);
			const auto cached = lazyHeightCache.find(receipt.sourceHash);
			if (cached != lazyHeightCache.end())
			{
				receipt.decodedHeight = cached->second;
				receipt.fromCache = true;
				receipt.message = "Resolved a verified terrain height channel from the bounded lazy cache.";
				return receipt;
			}
		}
		const auto encoded = blobResolver.Resolve(receipt.sourceHash);
		if (!encoded.has_value())
		{
			receipt.code = TerrainResultCode::Failed;
			receipt.message = "The requested terrain height blob is unavailable from the mounted resolver.";
			return receipt;
		}
		DecodedTerrainArea decoded{};
		const TerrainCodecResult decodedResult = DecodeTerrainArea(*encoded, decoded);
		if (!decodedResult.Succeeded())
		{
			receipt.code = decodedResult.code;
			receipt.message = decodedResult.message;
			return receipt;
		}
		if (decoded.header.channelKind != TerrainChannelKind::Height
			|| decoded.header.area != coordinate || decoded.header.assetUuid != capture.root->assetUuid)
		{
			receipt.code = TerrainResultCode::CorruptData;
			receipt.message = "Decoded terrain height metadata does not match the captured root and requested area.";
			return receipt;
		}
		if (cancellation != nullptr && cancellation->load(std::memory_order_acquire))
		{
			receipt.code = TerrainResultCode::Cancelled;
			receipt.message = "Lazy terrain loading was cancelled after bounded decode and before cache publication.";
			return receipt;
		}
		auto immutable = std::make_shared<const DecodedTerrainArea>(std::move(decoded));
		{
			std::scoped_lock lock(lazyCacheMutex);
			// A concurrent worker may have populated the same immutable content. In
			// that case reuse its value rather than retaining duplicate decode bytes.
			const auto [iterator, inserted] = lazyHeightCache.emplace(receipt.sourceHash, immutable);
			receipt.decodedHeight = iterator->second;
			receipt.fromCache = !inserted;
		}
		receipt.message = "Decoded and cached one verified terrain height channel.";
		return receipt;
	}

	TerrainAreaDescriptor TerrainAssetStore::ResolveDescriptorOrImplicit(const TerrainAssetRoot& root, TerrainAreaCoordinate coordinate) const
	{
		if (const auto descriptor = FindDescriptor(root, coordinate)) return *descriptor;
		TerrainAreaDescriptor implicit{}; implicit.coordinate = coordinate; implicit.editGeneration = root.publicationGeneration; implicit.minHeightMm = root.defaultHeightMm; implicit.maxHeightMm = root.defaultHeightMm; return implicit;
	}

	TerrainCodecResult TerrainAssetStore::ValidateRoot(const TerrainAssetRoot& root) const
	{
		if (root.publicationGeneration == 0u || !root.authoredBounds.IsValid()) return {TerrainResultCode::InvalidArgument, "Terrain root requires a non-zero publication generation and valid bounds."};
		if (root.layerStack && (root.layerStack->generation != root.layerStackGeneration || !ValidateTerrainLayerStack(*root.layerStack).Succeeded()))
			return {TerrainResultCode::InvalidArgument, "Terrain root layer metadata does not match its immutable stack generation."};
		for (std::size_t left = 0; left < root.directoryShards.size(); ++left)
		{
			const auto& shard = root.directoryShards[left];
			if (!shard.areaBounds.IsValid()) return {TerrainResultCode::InvalidArgument, "Terrain directory shard bounds are invalid."};
			for (const auto& [coordinate, descriptor] : shard.descriptors)
			{
				if (coordinate != descriptor.coordinate || !shard.areaBounds.Contains({coordinate.x, coordinate.z}) || descriptor.editGeneration == 0u || !descriptor.authoredContentHash.IsValid()) return {TerrainResultCode::InvalidArgument, "Terrain directory descriptor violates immutable root invariants."};
			}
			for (std::size_t right = left + 1u; right < root.directoryShards.size(); ++right)
			{
				const auto& other = root.directoryShards[right].areaBounds;
				const bool overlaps = shard.areaBounds.minX < other.maxXExclusive && other.minX < shard.areaBounds.maxXExclusive && shard.areaBounds.minZ < other.maxZExclusive && other.minZ < shard.areaBounds.maxZExclusive;
				if (overlaps) return {TerrainResultCode::InvalidArgument, "Terrain directory shards may not overlap."};
			}
		}
		return {};
	}

	TerrainCodecResult TerrainAssetStore::BuildCandidateRoot(const TerrainAssetRoot& base, const std::vector<TerrainAreaDescriptor>& replacements, ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		outCandidate.reset(); if (auto validation = ValidateRoot(base); !validation.Succeeded()) return validation;
		auto candidate = std::make_shared<TerrainAssetRoot>(base); candidate->publicationGeneration = base.publicationGeneration + 1u; candidate->rootContentHash = candidateRootHash;
		if (candidate->publicationGeneration == 0u || !candidateRootHash.IsValid()) return {TerrainResultCode::Overflow, "Terrain root generation or identity overflowed."};
		for (const TerrainAreaDescriptor& replacement : replacements)
		{
			bool replaced = false;
			for (TerrainDirectoryShard& shard : candidate->directoryShards)
			{
				if (!shard.areaBounds.Contains({replacement.coordinate.x, replacement.coordinate.z})) continue;
				shard.descriptors[replacement.coordinate] = replacement; replaced = true; break;
			}
			if (!replaced) return {TerrainResultCode::OutOfBounds, "Terrain patch lies outside a declared directory shard."};
		}
		if (auto validation = ValidateRoot(*candidate); !validation.Succeeded()) return validation;
		outCandidate = std::move(candidate); return {};
	}

	TerrainCodecResult TerrainAssetStore::BuildLayerStackCandidate(
		const TerrainAssetRoot& base, const TerrainLayerPatch& patch,
		ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		outCandidate.reset();
		if (const auto validation = ValidateRoot(base); !validation.Succeeded()) return validation;
		TerrainLayerStack current{};
		if (base.layerStack) current = *base.layerStack;
		else
		{
			// Legacy roots contained only a default layer id. Materialize that one
			// stable fallback layer before applying the first metadata patch.
			current.generation = base.layerStackGeneration == 0u ? 1u : base.layerStackGeneration;
			current.layers = {{base.defaultLayerId, "Default"}};
		}
		TerrainLayerStack updated{};
		if (const auto patchResult = ApplyTerrainLayerPatch(current, patch, updated); !patchResult.Succeeded()) return patchResult;
		auto candidate = std::make_shared<TerrainAssetRoot>(base);
		candidate->publicationGeneration = base.publicationGeneration + 1u;
		candidate->rootContentHash = candidateRootHash;
		candidate->layerStackGeneration = updated.generation;
		candidate->layerStack = std::make_shared<const TerrainLayerStack>(std::move(updated));
		if (candidate->publicationGeneration == 0u || !candidateRootHash.IsValid()) return {TerrainResultCode::Overflow, "Terrain metadata root generation or identity overflowed."};
		outCandidate = std::move(candidate);
		return {};
	}

	TerrainCodecResult TerrainAssetStore::PublishCandidate(std::shared_ptr<const TerrainAssetRoot> expectedBase, std::shared_ptr<const TerrainAssetRoot> candidate)
	{
		if (!expectedBase || !candidate) return {TerrainResultCode::InvalidArgument, "Terrain publication requires both observed and candidate roots."};
		if (auto validation = ValidateRoot(*candidate); !validation.Succeeded()) return validation;
		std::unique_lock lock(rootMutex);
		if (currentRoot != expectedBase) return {TerrainResultCode::Conflict, "Terrain root changed while a worker was preparing its candidate."};
		currentRoot = std::move(candidate); return {};
	}

	std::vector<ContentHash> TerrainAssetStore::CollectReferencedBlobHashes(const TerrainAssetRoot& root) const
	{
		HashSet unique;
		for (const TerrainDirectoryShard& shard : root.directoryShards)
			for (const auto& [coordinate, descriptor] : shard.descriptors)
			{
				if (descriptor.channels.heightHash) unique.insert(*descriptor.channels.heightHash);
				if (descriptor.channels.holeHash) unique.insert(*descriptor.channels.holeHash);
				for (const auto& [group, hash] : descriptor.channels.weightGroupHashes) unique.insert(hash);
			}
		return {unique.begin(), unique.end()};
	}

	TerrainPersistedRootState TerrainAssetStore::CapturePersistedRootState() const
	{
		std::scoped_lock lock(persistenceMutex); return persistedRoot;
	}

	std::vector<ContentHash> TerrainAssetStore::CollectPersistedReferencedBlobHashes() const
	{
		std::scoped_lock lock(persistenceMutex);
		std::vector<ContentHash> result;
		if (savedRootSnapshot) result = CollectReferencedBlobHashes(*savedRootSnapshot);
		if (predecessorRootSnapshot)
		{
			const std::vector<ContentHash> predecessor = CollectReferencedBlobHashes(*predecessorRootSnapshot);
			result.insert(result.end(), predecessor.begin(), predecessor.end());
		}
		return result;
	}

	TerrainPersistenceReceipt TerrainAssetStore::PersistAtomically(const TerrainPersistenceRequest& request)
	{
		TerrainPersistenceReceipt receipt{};
		if (request.dataDirectory.empty()) { receipt.code = TerrainResultCode::InvalidArgument; receipt.message = "Terrain persistence requires a data directory."; return receipt; }
		if (IsCancelled(request.cancellation)) { receipt.code = TerrainResultCode::Cancelled; receipt.message = "Terrain save was cancelled before staging."; return receipt; }
		const TerrainRootCapture capture = CaptureRoot();
		if (!capture.IsValid() || !ValidateRoot(*capture.root).Succeeded()) { receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain save refused an invalid authored root."; return receipt; }

		const std::vector<ContentHash> references = CollectReferencedBlobHashes(*capture.root);
		std::vector<std::pair<ContentHash, std::vector<std::uint8_t>>> blobs;
		blobs.reserve(references.size());
		for (const ContentHash hash : references)
		{
			if (IsCancelled(request.cancellation)) { receipt.code = TerrainResultCode::Cancelled; receipt.message = "Terrain save was cancelled while collecting immutable blobs."; return receipt; }
			const auto bytes = blobResolver.Resolve(hash);
			if (!bytes || HashTerrainBytes(*bytes) != hash) { receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain save found an unavailable or identity-mismatched referenced blob."; return receipt; }
			blobs.emplace_back(hash, *bytes);
		}

		std::error_code error;
		const std::filesystem::path staging = request.dataDirectory / (".staging-" + ToHexString(capture.root->rootContentHash));
		std::filesystem::remove_all(staging, error); error.clear();
		std::filesystem::create_directories(staging / "blobs", error);
		if (error) { receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save could not create private staging."; return receipt; }
		for (const auto& [hash, bytes] : blobs)
		{
			if (IsCancelled(request.cancellation)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Cancelled; receipt.message = "Terrain save was cancelled while staging blobs."; return receipt; }
			if (request.injectFailureForTests == TerrainPersistenceFailurePoint::BlobWrite || !WriteDurableBinaryFile(staging / "blobs" / (ToHexString(hash) + ".tearea"), bytes)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save failed before a blob became durable."; return receipt; }
		}
		const std::string manifest = EncodeRootManifest(*capture.root);
		const auto decoded = DecodeRootManifest(manifest);
		if (!decoded || !ValidateRoot(*decoded).Succeeded()) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain root staging failed self-validation."; return receipt; }
		// The directory payload is independently staged even though v1 keeps the
		// compact descriptor records in the root manifest as well. This makes the
		// write order explicit for future sharded-directory providers.
		std::filesystem::create_directories(staging / "directories", error);
		if (error || request.injectFailureForTests == TerrainPersistenceFailurePoint::DirectoryWrite || !WriteDurableTextFile(staging / "directories" / (ToHexString(capture.root->rootContentHash) + ".dir"), manifest)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable directory could not be staged."; return receipt; }
		std::filesystem::create_directories(staging / "roots", error);
		if (error || request.injectFailureForTests == TerrainPersistenceFailurePoint::ManifestWrite || !WriteDurableTextFile(staging / "roots" / (ToHexString(capture.root->rootContentHash) + ".root"), manifest)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable root manifest could not be staged."; return receipt; }
		if (request.injectFailureForTests == TerrainPersistenceFailurePoint::DurableFlush) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain durable flush failure was injected before CURRENT."; return receipt; }

		std::filesystem::create_directories(request.dataDirectory / "blobs", error);
		std::filesystem::create_directories(request.dataDirectory / "directories", error);
		std::filesystem::create_directories(request.dataDirectory / "roots", error);
		if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save could not prepare durable directories."; return receipt; }
		for (const auto& [hash, bytes] : blobs)
		{
			const std::filesystem::path finalBlob = request.dataDirectory / "blobs" / (ToHexString(hash) + ".tearea");
			if (std::filesystem::exists(finalBlob))
			{
				const auto existing = ReadBinaryFile(finalBlob);
				if (!existing || *existing != bytes || HashTerrainBytes(*existing) != hash) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain immutable blob path already contains different bytes."; return receipt; }
			}
			else std::filesystem::rename(staging / "blobs" / (ToHexString(hash) + ".tearea"), finalBlob, error);
			if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable blob promotion failed."; return receipt; }
		}
		const std::filesystem::path finalDirectory = request.dataDirectory / "directories" / (ToHexString(capture.root->rootContentHash) + ".dir");
		if (std::filesystem::exists(finalDirectory))
		{
			const auto existing = ReadBinaryFile(finalDirectory);
			const std::vector<std::uint8_t> expected(manifest.begin(), manifest.end());
			if (!existing || *existing != expected) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain immutable directory path already contains different bytes."; return receipt; }
		}
		else std::filesystem::rename(staging / "directories" / (ToHexString(capture.root->rootContentHash) + ".dir"), finalDirectory, error);
		if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable directory promotion failed."; return receipt; }
		const std::filesystem::path finalManifest = request.dataDirectory / "roots" / (ToHexString(capture.root->rootContentHash) + ".root");
		if (std::filesystem::exists(finalManifest))
		{
			const auto existing = ReadBinaryFile(finalManifest);
			const std::vector<std::uint8_t> expected(manifest.begin(), manifest.end());
			if (!existing || *existing != expected) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain immutable root path already contains different bytes."; return receipt; }
		}
		else std::filesystem::rename(staging / "roots" / (ToHexString(capture.root->rootContentHash) + ".root"), finalManifest, error);
		if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable root promotion failed."; return receipt; }

		const TerrainPersistedRootState prior = CapturePersistedRootState();
		const std::filesystem::path currentTemporary = request.dataDirectory / "CURRENT.pending";
		const std::string pointer = ToHexString(capture.root->rootContentHash) + "\n" + (prior.savedRootHash.IsValid() ? ToHexString(prior.savedRootHash) : "-") + "\n";
		if (request.injectFailureForTests == TerrainPersistenceFailurePoint::CurrentReplacement || !WriteDurableTextFile(currentTemporary, pointer) || !ReplaceCurrentAtomically(currentTemporary, request.dataDirectory / "CURRENT")) { std::filesystem::remove(currentTemporary, error); std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save left the prior CURRENT pointer intact after replacement failed."; return receipt; }
		std::filesystem::remove_all(staging, error);
		{
			std::scoped_lock lock(persistenceMutex);
			predecessorRootSnapshot = savedRootSnapshot;
			savedRootSnapshot = capture.root;
			persistedRoot.predecessorRootHash = persistedRoot.savedRootHash;
			persistedRoot.savedRootHash = capture.root->rootContentHash;
			persistedRoot.savedPublicationGeneration = capture.root->publicationGeneration;
			persistedRoot.recoveredPredecessor = false;
		}
		HashSet reachable(references.begin(), references.end());
		for (const ContentHash hash : CollectPersistedReferencedBlobHashes()) reachable.insert(hash);
		for (const ContentHash hash : blobResolver.SnapshotHashes()) if (!reachable.contains(hash)) receipt.orphanedBlobHashes.push_back(hash);
		receipt.savedRootHash = capture.root->rootContentHash; receipt.predecessorRootHash = prior.savedRootHash; receipt.message = "Terrain blobs, manifest, and CURRENT pointer were persisted atomically."; return receipt;
	}

	TerrainPersistenceReceipt TerrainAssetStore::OpenPersistedRoot(const std::filesystem::path& dataDirectory)
	{
		return RecoverPersistedRoot(dataDirectory);
	}

	TerrainPersistenceReceipt TerrainAssetStore::RecoverPersistedRoot(const std::filesystem::path& dataDirectory)
	{
		TerrainPersistenceReceipt receipt{}; std::ifstream current(dataDirectory / "CURRENT", std::ios::binary); std::string primary; std::string predecessor;
		if (!current || !std::getline(current, primary) || !std::getline(current, predecessor)) { receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain recovery could not read a complete CURRENT pointer."; return receipt; }
		for (const std::string& candidateText : {primary, predecessor})
		{
			if (candidateText == "-") continue;
			const auto candidateHash = ParseContentHash(candidateText); if (!candidateHash) continue;
			std::ifstream manifestFile(dataDirectory / "roots" / (candidateText + ".root"), std::ios::binary);
			if (!manifestFile.is_open()) continue;
			std::stringstream buffer; buffer << manifestFile.rdbuf();
			const auto root = DecodeRootManifest(buffer.str());
			if (!root || root->rootContentHash != *candidateHash || !ValidateRoot(*root).Succeeded()) continue;
			bool allBlobsPresent = true; for (const ContentHash hash : CollectReferencedBlobHashes(*root)) if (!std::filesystem::exists(dataDirectory / "blobs" / (ToHexString(hash) + ".tearea"))) { allBlobsPresent = false; break; }
			if (!allBlobsPresent) continue;
			std::shared_ptr<const TerrainAssetRoot> predecessorSnapshot;
			if (candidateText == primary && predecessor != "-")
			{
				const auto predecessorHash = ParseContentHash(predecessor);
				std::ifstream predecessorFile(dataDirectory / "roots" / (predecessor + ".root"), std::ios::binary);
				std::stringstream predecessorBuffer;
				if (predecessorFile.is_open()) predecessorBuffer << predecessorFile.rdbuf();
				const auto predecessorRoot = predecessorHash && predecessorFile.is_open() ? DecodeRootManifest(predecessorBuffer.str()) : std::nullopt;
				if (predecessorRoot && predecessorRoot->rootContentHash == *predecessorHash && ValidateRoot(*predecessorRoot).Succeeded()) predecessorSnapshot = std::make_shared<TerrainAssetRoot>(*predecessorRoot);
			}
			for (const ContentHash hash : CollectReferencedBlobHashes(*root))
			{
				const auto bytes = ReadBinaryFile(dataDirectory / "blobs" / (ToHexString(hash) + ".tearea"));
				if (!bytes || !blobResolver.PutVerified(hash, *bytes)) { allBlobsPresent = false; break; }
			}
			if (!allBlobsPresent) continue;
			{
				std::unique_lock lock(rootMutex); currentRoot = std::make_shared<TerrainAssetRoot>(*root);
			}
			{
				const std::optional<ContentHash> predecessorHash = predecessor == "-" ? std::optional<ContentHash>{ContentHash{}} : ParseContentHash(predecessor);
				std::scoped_lock lock(persistenceMutex);
				persistedRoot = {*candidateHash, candidateText == primary && predecessorHash ? *predecessorHash : ContentHash{}, root->publicationGeneration, candidateText != primary};
				savedRootSnapshot = std::make_shared<TerrainAssetRoot>(*root);
				predecessorRootSnapshot = std::move(predecessorSnapshot);
			}
			receipt.savedRootHash = *candidateHash; receipt.usedPredecessor = candidateText != primary; receipt.message = receipt.usedPredecessor ? "Terrain recovery selected the complete predecessor root." : "Terrain recovery selected CURRENT root."; return receipt;
		}
		receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain recovery found neither CURRENT nor predecessor root complete."; return receipt;
	}

	TerrainCodecResult TerrainAssetStore::ExpandAuthoredBounds(const TerrainAssetRoot& base, TerrainSampleBounds expandedBounds, ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		outCandidate.reset();
		if (!expandedBounds.IsValid() || expandedBounds.minX > base.authoredBounds.minX || expandedBounds.minZ > base.authoredBounds.minZ || expandedBounds.maxXExclusive < base.authoredBounds.maxXExclusive || expandedBounds.maxZExclusive < base.authoredBounds.maxZExclusive)
			return {TerrainResultCode::InvalidArgument, "Terrain extent expansion may only contain the existing authored bounds."};
		auto candidate = std::make_shared<TerrainAssetRoot>(base);
		candidate->authoredBounds = expandedBounds;
		candidate->publicationGeneration = base.publicationGeneration + 1u;
		candidate->rootContentHash = candidateRootHash;
		if (candidate->publicationGeneration == 0u || !candidateRootHash.IsValid())
			return {TerrainResultCode::Overflow, "Terrain extent expansion overflowed its root generation or identity."};
		// Directories and descriptors intentionally remain untouched. This makes
		// implicit default areas visible without resampling any existing channel.
		outCandidate = std::move(candidate);
		return {};
	}

	TerrainCodecResult TerrainAssetStore::MaterializeImplicitArea(const TerrainAssetRoot& base, TerrainAreaDescriptor materialized, ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		if (FindDescriptor(base, materialized.coordinate).has_value())
			return {TerrainResultCode::Conflict, "Terrain materialization refuses to overwrite an existing authored area."};
		// An expanded root may not yet have a directory shard for its newly visible
		// default area. Create one immutable one-area shard rather than rewriting
		// a neighbouring directory or resampling any existing payload.
		for (const TerrainDirectoryShard& shard : base.directoryShards)
		{
			if (shard.areaBounds.Contains({materialized.coordinate.x, materialized.coordinate.z}))
				return BuildCandidateRoot(base, {std::move(materialized)}, candidateRootHash, outCandidate);
		}
		if (materialized.coordinate.x == (std::numeric_limits<std::int64_t>::max)()
			|| materialized.coordinate.z == (std::numeric_limits<std::int64_t>::max)()
			|| materialized.editGeneration == 0u || !materialized.authoredContentHash.IsValid())
			return {TerrainResultCode::Overflow, "Terrain area materialization cannot form a bounded directory shard."};
		auto candidate = std::make_shared<TerrainAssetRoot>(base);
		candidate->publicationGeneration = base.publicationGeneration + 1u;
		candidate->rootContentHash = candidateRootHash;
		TerrainDirectoryShard shard{};
		shard.areaBounds = {materialized.coordinate.x, materialized.coordinate.z, materialized.coordinate.x + 1, materialized.coordinate.z + 1};
		shard.descriptors.emplace(materialized.coordinate, std::move(materialized));
		candidate->directoryShards.push_back(std::move(shard));
		if (auto validation = ValidateRoot(*candidate); !validation.Succeeded()) return validation;
		outCandidate = std::move(candidate);
		return {};
	}
}
