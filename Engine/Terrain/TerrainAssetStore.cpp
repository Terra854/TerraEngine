// TerrainAssetStore.cpp -- Copy-on-write terrain directory implementation.

#include "Terrain/TerrainAssetStore.h"

#include "File Formats/TerraEngineTerrainFile.h"
#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>
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

		TerrainRootCodecResult RootCodecFailure(TerrainResultCode code, TerrainRootDiagnosticCode diagnostic, std::string message)
		{
			TerrainRootCodecResult result{};
			result.code = code;
			result.diagnostic = diagnostic;
			result.message = std::move(message);
			return result;
		}

		bool MatchesTerrainBlobIdentity(ContentHash hash, std::span<const std::uint8_t> bytes)
		{
			if (!hash.IsValid() || bytes.empty()) return false;
			if (HashTerrainBytes(bytes) == hash) return true;
			// TEAREA01 avoids a self-hash cycle by calculating envelopeHash with the
			// hash field zeroed. Decode verifies that canonical identity and payload,
			// so the resolver can safely key durable envelopes by the root's hash.
			DecodedTerrainArea decoded;
			return DecodeTerrainArea(bytes, decoded).Succeeded()
				&& decoded.header.envelopeHash == hash;
		}

		void RefreshDirectoryIdentities(TerrainAssetRoot& root)
		{
			for (TerrainDirectoryShard& shard : root.directoryShards)
			{
				std::ostringstream identity;
				identity.imbue(std::locale::classic());
				identity << shard.areaBounds.minX << ' ' << shard.areaBounds.minZ << ' '
					<< shard.areaBounds.maxXExclusive << ' ' << shard.areaBounds.maxZExclusive << '\n';
				for (const auto& [coordinate, descriptor] : shard.descriptors)
					identity << coordinate.x << ' ' << coordinate.z << ' ' << descriptor.editGeneration << ' '
						<< ToHexString(descriptor.authoredContentHash) << '\n';
				const std::string bytesText = identity.str();
				const std::vector<std::uint8_t> bytes(bytesText.begin(), bytesText.end());
				shard.identity = HashTerrainBytes(bytes);
			}
		}

		TerrainRootCodecResult EncodeRootManifest(const TerrainAssetRoot& root)
		{
			if (root.assetUuid == TerraEngine::EMPTY_UUID || root.publicationGeneration == 0u
				|| root.persistedRootGeneration == 0u || !std::isfinite(root.sampleSpacingMetres)
				|| root.sampleSpacingMetres <= 0.0 || !root.authoredBounds.IsValid()
				|| root.defaultLayerId == 0u || root.layerStackGeneration == 0u)
				return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 metadata is incomplete or non-finite.");
			TerrainLayerStack synthesized{};
			const TerrainLayerStack* layers = root.layerStack.get();
			if (layers == nullptr)
			{
				synthesized.generation = root.layerStackGeneration;
				TerrainLayer defaultLayer{};
				defaultLayer.stableId = root.defaultLayerId;
				defaultLayer.name = "Default";
				defaultLayer.enabled = true;
				defaultLayer.allowFallback = true;
				defaultLayer.fallbackState = TerrainLayerFallbackState::FallbackActive;
				synthesized.layers.push_back(std::move(defaultLayer));
				layers = &synthesized;
			}
			if (layers->layers.empty() || layers->layers.size() > Format::kTerrainMaximumLayers)
				return RootCodecFailure(TerrainResultCode::BudgetExceeded, TerrainRootDiagnosticCode::TerrainLayerDefinitionLimitExceeded, "TERRAROOT1 permits one to sixty-four layer definitions.");
			std::size_t enabledCount = 0u;
			for (std::size_t index = 0u; index < layers->layers.size(); ++index)
			{
				const TerrainLayer& layer = layers->layers[index];
				if (layer.stableId == 0u) return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainLayerIdentityInvalid, "TERRAROOT1 layer ids must be non-zero and unique.");
				if (!IsValidTerrainLayerName(layer.name)) return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainLayerNameInvalid, "TERRAROOT1 layer names must be bounded printable UTF-8 without edge whitespace.");
				if (!std::isfinite(layer.mapping.tileSizeXMetres) || !std::isfinite(layer.mapping.tileSizeZMetres)
					|| !std::isfinite(layer.mapping.offsetXMetres) || !std::isfinite(layer.mapping.offsetZMetres)
					|| !std::isfinite(layer.mapping.rotationRadians) || !std::isfinite(layer.mapping.triplanarSharpness)
					|| layer.mapping.tileSizeXMetres <= 0.0 || layer.mapping.tileSizeZMetres <= 0.0 || layer.mapping.triplanarSharpness <= 0.0)
					return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainLayerMappingInvalid, "TERRAROOT1 layer mapping values must be finite with positive tile sizes.");
				for (std::size_t prior = 0u; prior < index; ++prior)
					if (layers->layers[prior].stableId == layer.stableId || EqualNormalizedTerrainLayerName(layers->layers[prior].name, layer.name))
						return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainLayerIdentityInvalid, "TERRAROOT1 layer ids and names must be unique.");
				if (layer.enabled) ++enabledCount;
			}
			if (enabledCount == 0u || enabledCount > Format::kTerrainMaximumEnabledLayers)
				return RootCodecFailure(TerrainResultCode::BudgetExceeded, TerrainRootDiagnosticCode::TerrainEnabledLayerLimitViolated, "TERRAROOT1 requires one to sixteen enabled layers.");
			if (root.directoryShards.size() > Format::kTerrainMaximumDirectoryShards)
				return RootCodecFailure(TerrainResultCode::BudgetExceeded, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains too many directory shards.");

			std::ostringstream text;
			text.imbue(std::locale::classic());
			text << std::setprecision(std::numeric_limits<double>::max_digits10);
			text << Format::kTerrainRootMagic << '\n';
			text << "ASSET " << TerraEngine::ToHexString(root.assetUuid) << '\n';
			text << "GENERATION " << root.publicationGeneration << ' ' << root.persistedRootGeneration << '\n';
			text << "SAMPLE_SPACING_METRES " << (root.sampleSpacingMetres == 0.0 ? 0.0 : root.sampleSpacingMetres) << '\n';
			text << "BOUNDS " << root.authoredBounds.minX << ' ' << root.authoredBounds.minZ << ' ' << root.authoredBounds.maxXExclusive << ' ' << root.authoredBounds.maxZExclusive << '\n';
			text << "DEFAULTS " << root.defaultHeightMm << ' ' << root.defaultLayerId << ' ' << root.layerStackGeneration << '\n';
			text << "LAYER_COUNT " << layers->layers.size() << '\n';
			for (const TerrainLayer& layer : layers->layers)
			{
				const bool allowFallback = layer.allowFallback;
				const char* mappingMode = layer.mapping.mode == TerrainLayerMappingMode::Triplanar ? "TRIPLANAR" : "TERRAIN_XZ";
				text << "LAYER " << layer.stableId << ' ' << std::quoted(layer.name) << ' ' << (layer.enabled ? 1 : 0) << ' ' << (allowFallback ? 1 : 0) << ' '
					<< TerraEngine::ToHexString(layer.materialAssetUuid) << ' ' << mappingMode << ' ' << layer.mapping.tileSizeXMetres << ' ' << layer.mapping.tileSizeZMetres << ' '
					<< (layer.mapping.offsetXMetres == 0.0 ? 0.0 : layer.mapping.offsetXMetres) << ' ' << (layer.mapping.offsetZMetres == 0.0 ? 0.0 : layer.mapping.offsetZMetres) << ' '
					<< (layer.mapping.rotationRadians == 0.0 ? 0.0 : layer.mapping.rotationRadians) << ' ' << layer.mapping.triplanarSharpness << '\n';
			}
			text << "SHARD_COUNT " << root.directoryShards.size() << '\n';
			for (const TerrainDirectoryShard& shard : root.directoryShards)
			{
				if (!shard.areaBounds.IsValid() || !shard.identity.IsValid() || shard.descriptors.size() > Format::kTerrainMaximumDirectoryEntriesPerShard)
					return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid directory shard.");
				text << "SHARD " << shard.areaBounds.minX << ' ' << shard.areaBounds.minZ << ' ' << shard.areaBounds.maxXExclusive << ' '
					<< shard.areaBounds.maxZExclusive << ' ' << ToHexString(shard.identity) << ' ' << shard.descriptors.size() << '\n';
				for (const auto& [coordinate, descriptor] : shard.descriptors)
				{
					if (!shard.areaBounds.Contains({coordinate.x, coordinate.z}) || descriptor.coordinate != coordinate || descriptor.editGeneration == 0u
						|| !descriptor.authoredContentHash.IsValid() || descriptor.minHeightMm > descriptor.maxHeightMm
						|| descriptor.channels.weightGroupHashes.size() > kTerrainMaximumWeightGroups)
						return RootCodecFailure(TerrainResultCode::InvalidArgument, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid area reference.");
					text << "AREA " << coordinate.x << ' ' << coordinate.z << ' ' << descriptor.editGeneration << ' ' << descriptor.minHeightMm << ' '
						<< descriptor.maxHeightMm << ' ' << ToHexString(descriptor.authoredContentHash) << ' '
						<< (descriptor.channels.heightHash ? ToHexString(*descriptor.channels.heightHash) : "-") << ' '
						<< (descriptor.channels.holeHash ? ToHexString(*descriptor.channels.holeHash) : "-") << ' '
						<< descriptor.channels.weightGroupHashes.size();
					for (const auto& [group, hash] : descriptor.channels.weightGroupHashes) text << ' ' << group << ' ' << ToHexString(hash);
					text << '\n';
				}
			}
			text << "END\n";
			TerrainRootCodecResult result{};
			result.root = root;
			result.canonicalBytes = text.str();
			const std::vector<std::uint8_t> bytes(result.canonicalBytes.begin(), result.canonicalBytes.end());
			result.contentHash = HashTerrainBytes(bytes);
			result.root->rootContentHash = result.contentHash;
			result.message = "TERRAROOT1 encoded canonically.";
			return result;
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

		TerrainRootCodecResult DecodeRootManifest(std::string_view text)
		{
			if (text.size() > Format::kTerrainRootMaximumBytes)
				return RootCodecFailure(TerrainResultCode::BudgetExceeded, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 exceeds the bounded root byte budget.");
			if (!text.starts_with(Format::kTerrainRootMagic))
				return RootCodecFailure(TerrainResultCode::UnsupportedVersion, TerrainRootDiagnosticCode::UnsupportedTerrainRoot, "Only canonical TERRAROOT1 roots are supported; recreate or recook this terrain asset.");
			if (text.empty() || text.back() != '\n' || text.find('\r') != std::string_view::npos || text.find('\0') != std::string_view::npos)
				return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 must use canonical LF records with one terminal newline.");
			std::istringstream input{std::string(text)};
			input.imbue(std::locale::classic());
			std::string magic;
			std::string label;
			if (!(input >> magic) || magic != Format::kTerrainRootMagic)
				return RootCodecFailure(TerrainResultCode::UnsupportedVersion, TerrainRootDiagnosticCode::UnsupportedTerrainRoot, "Only canonical TERRAROOT1 roots are supported; recreate or recook this terrain asset.");
			TerrainAssetRoot root{};
			std::string assetText;
			if (!(input >> label >> assetText) || label != "ASSET")
				return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 is missing its ASSET record.");
			const auto asset = TerraEngine::ParseUUID(assetText);
			if (!asset) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 has an invalid asset identity.");
			root.assetUuid = *asset;
			if (!(input >> label >> root.publicationGeneration >> root.persistedRootGeneration) || label != "GENERATION"
				|| !(input >> label >> root.sampleSpacingMetres) || label != "SAMPLE_SPACING_METRES"
				|| !(input >> label >> root.authoredBounds.minX >> root.authoredBounds.minZ >> root.authoredBounds.maxXExclusive >> root.authoredBounds.maxZExclusive) || label != "BOUNDS"
				|| !(input >> label >> root.defaultHeightMm >> root.defaultLayerId >> root.layerStackGeneration) || label != "DEFAULTS")
				return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 root records are missing, reordered, or malformed.");
			std::size_t layerCount = 0u;
			if (!(input >> label >> layerCount) || label != "LAYER_COUNT" || layerCount > Format::kTerrainMaximumLayers)
				return RootCodecFailure(TerrainResultCode::BudgetExceeded, TerrainRootDiagnosticCode::TerrainLayerDefinitionLimitExceeded, "TERRAROOT1 has an invalid layer definition count.");
			TerrainLayerStack layers{};
			layers.generation = root.layerStackGeneration;
			for (std::size_t index = 0u; index < layerCount; ++index)
			{
				TerrainLayer layer{};
				std::string material;
				std::string mappingMode;
				std::uint32_t enabled = 0u;
				std::uint32_t allowFallback = 0u;
				double triplanarSharpness = 0.0;
				if (!(input >> label >> layer.stableId >> std::quoted(layer.name) >> enabled >> allowFallback >> material >> mappingMode
					>> layer.mapping.tileSizeXMetres >> layer.mapping.tileSizeZMetres >> layer.mapping.offsetXMetres >> layer.mapping.offsetZMetres >> layer.mapping.rotationRadians >> triplanarSharpness)
					|| label != "LAYER" || enabled > 1u || allowFallback > 1u || (mappingMode != "TERRAIN_XZ" && mappingMode != "TRIPLANAR")
					|| !std::isfinite(triplanarSharpness) || triplanarSharpness <= 0.0)
					return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainLayerMappingInvalid, "TERRAROOT1 contains a malformed layer record.");
				const auto materialId = TerraEngine::ParseUUID(material);
				if (!materialId) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainLayerIdentityInvalid, "TERRAROOT1 contains an invalid layer material identity.");
				layer.materialAssetUuid = *materialId;
				layer.enabled = enabled != 0u;
				layer.allowFallback = allowFallback != 0u;
				layer.mapping.mode = mappingMode == "TRIPLANAR" ? TerrainLayerMappingMode::Triplanar : TerrainLayerMappingMode::TerrainXZ;
				layer.mapping.triplanarSharpness = triplanarSharpness;
				layer.fallbackState = TerrainLayerFallbackState::Healthy;
				layers.layers.push_back(std::move(layer));
			}
			root.layerStack = std::make_shared<const TerrainLayerStack>(std::move(layers));
			std::size_t shardCount = 0u;
			if (!(input >> label >> shardCount) || label != "SHARD_COUNT" || shardCount > Format::kTerrainMaximumDirectoryShards)
				return RootCodecFailure(TerrainResultCode::BudgetExceeded, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 has an invalid shard count.");
			root.directoryShards.reserve(shardCount);
			for (std::size_t shardIndex = 0u; shardIndex < shardCount; ++shardIndex)
			{
				TerrainDirectoryShard shard{}; std::string identity; std::size_t descriptorCount = 0u;
				if (!(input >> label >> shard.areaBounds.minX >> shard.areaBounds.minZ >> shard.areaBounds.maxXExclusive >> shard.areaBounds.maxZExclusive >> identity >> descriptorCount)
					|| label != "SHARD" || descriptorCount > Format::kTerrainMaximumDirectoryEntriesPerShard)
					return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains a malformed shard record.");
				const auto parsedIdentity = ParseContentHash(identity);
				if (!parsedIdentity) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid shard identity.");
				shard.identity = *parsedIdentity;
				for (std::size_t descriptorIndex = 0u; descriptorIndex < descriptorCount; ++descriptorIndex)
				{
					TerrainAreaDescriptor descriptor{}; std::string descriptorHash; std::string heightHash; std::string holeHash; std::size_t weightCount = 0u;
					if (!(input >> label >> descriptor.coordinate.x >> descriptor.coordinate.z >> descriptor.editGeneration >> descriptor.minHeightMm >> descriptor.maxHeightMm >> descriptorHash >> heightHash >> holeHash >> weightCount)
						|| label != "AREA" || weightCount > kTerrainMaximumWeightGroups)
						return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains a malformed area record.");
					const auto parsedDescriptorHash = ParseContentHash(descriptorHash);
					if (!parsedDescriptorHash) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid area identity.");
					descriptor.authoredContentHash = *parsedDescriptorHash;
					if (heightHash != "-") { const auto parsed = ParseContentHash(heightHash); if (!parsed) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid height identity."); descriptor.channels.heightHash = *parsed; }
					if (holeHash != "-") { const auto parsed = ParseContentHash(holeHash); if (!parsed) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid hole identity."); descriptor.channels.holeHash = *parsed; }
					for (std::size_t weight = 0u; weight < weightCount; ++weight)
					{
						std::uint16_t group = 0u; std::string weightHash;
						if (!(input >> group >> weightHash)) return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains a truncated weight reference.");
						const auto parsed = ParseContentHash(weightHash);
						if (!parsed || group == 0u || !descriptor.channels.weightGroupHashes.emplace(group, *parsed).second)
							return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 contains an invalid weight reference.");
					}
					if (!shard.descriptors.emplace(descriptor.coordinate, std::move(descriptor)).second)
						return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid, "TERRAROOT1 repeats an area coordinate.");
				}
				root.directoryShards.push_back(std::move(shard));
			}
			std::string end;
			std::string trailing;
			if (!(input >> end) || end != "END" || (input >> trailing))
				return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 is truncated or contains trailing data.");
			TerrainRootCodecResult canonical = EncodeRootManifest(root);
			if (!canonical.Succeeded()) return canonical;
			if (canonical.canonicalBytes != text)
				return RootCodecFailure(TerrainResultCode::CorruptData, TerrainRootDiagnosticCode::TerrainRootMalformed, "TERRAROOT1 is parseable but not canonical; recreate or recook the asset.");
			canonical.message = "TERRAROOT1 decoded and canonical identity verified.";
			return canonical;
		}
	}

	TerrainRootCodecResult EncodeTerrainRootManifest(const TerrainAssetRoot& root)
	{
		return EncodeRootManifest(root);
	}

	TerrainRootCodecResult DecodeTerrainRootManifest(std::string_view text)
	{
		return DecodeRootManifest(text);
	}

	bool TerrainBlobResolver::PutVerified(ContentHash hash, std::vector<std::uint8_t> bytes)
	{
		if (!MatchesTerrainBlobIdentity(hash, bytes)) return false;
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
		const auto currentAreas = TerrainAreasForSamples(root.authoredBounds);
		if (!currentAreas || !currentAreas->Contains(coordinate)) return std::nullopt;
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
		TerrainAreaDescriptor implicit{};
		implicit.coordinate = coordinate;
		implicit.editGeneration = root.publicationGeneration;
		implicit.minHeightMm = root.defaultHeightMm;
		implicit.maxHeightMm = root.defaultHeightMm;
		implicit.layerPresence = {root.defaultLayerId};
		// The identity depends only on immutable root defaults and the signed area
		// coordinate. It is stable across worker order and needs no stored blob.
		const std::string identityText = TerraEngine::ToHexString(root.assetUuid) + ":implicit:"
			+ std::to_string(coordinate.x) + ":" + std::to_string(coordinate.z) + ":"
			+ std::to_string(root.defaultHeightMm) + ":" + std::to_string(root.defaultLayerId);
		implicit.authoredContentHash = HashTerrainBytes(std::vector<std::uint8_t>(identityText.begin(), identityText.end()));
		return implicit;
	}

	TerrainCodecResult TerrainAssetStore::ValidateRoot(const TerrainAssetRoot& root) const
	{
		if (root.publicationGeneration == 0u || !root.authoredBounds.IsValid()) return {TerrainResultCode::InvalidArgument, "Terrain root requires a non-zero publication generation and valid bounds."};
		const auto currentAreas = TerrainAreasForSamples(root.authoredBounds);
		if (!currentAreas) return {TerrainResultCode::InvalidArgument, "Terrain root requires a non-empty checked authored extent."};
		if (root.layerStack && (root.layerStack->generation != root.layerStackGeneration || !ValidateTerrainLayerStack(*root.layerStack).Succeeded()))
			return {TerrainResultCode::InvalidArgument, "Terrain root layer metadata does not match its immutable stack generation."};
		for (std::size_t left = 0; left < root.directoryShards.size(); ++left)
		{
			const auto& shard = root.directoryShards[left];
			if (!shard.areaBounds.IsValid()) return {TerrainResultCode::InvalidArgument, "Terrain directory shard bounds are invalid."};
			for (const auto& [coordinate, descriptor] : shard.descriptors)
			{
				if (coordinate != descriptor.coordinate || !shard.areaBounds.Contains({coordinate.x, coordinate.z})
					|| !currentAreas->Contains(coordinate) || descriptor.editGeneration == 0u
					|| !descriptor.authoredContentHash.IsValid()) return {TerrainResultCode::InvalidArgument, "Terrain directory descriptor violates immutable root invariants."};
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
		(void)candidateRootHash;
		outCandidate.reset(); if (auto validation = ValidateRoot(base); !validation.Succeeded()) return validation;
		auto candidate = std::make_shared<TerrainAssetRoot>(base); candidate->publicationGeneration = base.publicationGeneration + 1u;
		if (candidate->persistedRootGeneration == 0u) candidate->persistedRootGeneration = 1u;
		if (candidate->publicationGeneration == 0u) return {TerrainResultCode::Overflow, "Terrain root generation overflowed."};
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
		RefreshDirectoryIdentities(*candidate);
		const TerrainRootCodecResult encoded = EncodeRootManifest(*candidate);
		if (!encoded.Succeeded()) return {encoded.code, encoded.message};
		candidate->rootContentHash = encoded.contentHash;
		outCandidate = std::move(candidate); return {};
	}

	TerrainCodecResult TerrainAssetStore::BuildLayerStackCandidate(
		const TerrainAssetRoot& base, const TerrainLayerPatch& patch,
		ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		(void)candidateRootHash;
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
		if (candidate->persistedRootGeneration == 0u) candidate->persistedRootGeneration = 1u;
		candidate->layerStackGeneration = updated.generation;
		candidate->layerStack = std::make_shared<const TerrainLayerStack>(std::move(updated));
		if (candidate->publicationGeneration == 0u) return {TerrainResultCode::Overflow, "Terrain metadata root generation overflowed."};
		if (const auto validation = ValidateRoot(*candidate); !validation.Succeeded()) return validation;
		RefreshDirectoryIdentities(*candidate);
		const TerrainRootCodecResult encoded = EncodeRootManifest(*candidate);
		if (!encoded.Succeeded()) return {encoded.code, encoded.message};
		candidate->rootContentHash = encoded.contentHash;
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
		// Persistence owns canonical directory identities. Some in-memory producers
		// construct a valid descriptor tree before its derived shard hashes exist;
		// serialize a detached copy so saving never mutates the live root and always
		// writes the same TERRAROOT1 bytes for equivalent authored state.
		TerrainAssetRoot persistenceRoot = *capture.root;
		// Worker/editor transactions retain canonical channel payloads in memory so
		// undo can copy exact spans cheaply. Durable .tearea files additionally need
		// the versioned envelope: normalize every referenced channel into that public
		// ABI and rewrite only the detached persistence root to its envelope hashes.
		std::vector<std::pair<ContentHash, std::vector<std::uint8_t>>> blobs;
		std::unordered_map<ContentHash, std::size_t, ContentHashHasher> blobIndices;
		auto canonicalizeChannel = [&](TerrainAreaDescriptor& descriptor,
			std::optional<ContentHash>& channelHash, TerrainChannelKind channelKind,
			std::uint16_t weightGroup) -> TerrainCodecResult
		{
			if (!channelHash) return {};
			const auto source = blobResolver.Resolve(*channelHash);
			if (!source || !MatchesTerrainBlobIdentity(*channelHash, *source))
				return {TerrainResultCode::CorruptData,
					"Terrain save found an unavailable or identity-mismatched referenced blob."};

			const std::size_t expectedPayloadBytes = channelKind == TerrainChannelKind::Height
				? kTerrainAreaSampleCount * sizeof(std::int32_t)
				: channelKind == TerrainChannelKind::Holes
					? kTerrainAreaSampleCount / 8u
					: kTerrainAreaSampleCount * 12u;
			DecodedTerrainArea decoded;
			const TerrainCodecResult decodedResult = DecodeTerrainArea(*source, decoded);
			ContentHash durableHash{};
			std::vector<std::uint8_t> durableBytes;
			if (decodedResult.Succeeded())
			{
				const bool matchingEnvelope = decoded.header.assetUuid == persistenceRoot.assetUuid
					&& decoded.header.area == descriptor.coordinate
					&& decoded.header.channelKind == channelKind
					&& decoded.header.weightGroup == weightGroup
					&& decoded.header.envelopeHash == *channelHash
					&& decoded.canonicalPayload.size() == expectedPayloadBytes;
				if (!matchingEnvelope)
					return {TerrainResultCode::CorruptData,
						"Terrain save rejected a .tearea envelope that does not match its root descriptor."};
				durableHash = *channelHash;
				durableBytes = *source;
			}
			else
			{
				const bool claimsEnvelope = source->size() >= Format::kTerrainAreaMagic.size()
					&& std::equal(Format::kTerrainAreaMagic.begin(),
						Format::kTerrainAreaMagic.end(), source->begin());
				if (claimsEnvelope || source->size() != expectedPayloadBytes)
					return {TerrainResultCode::CorruptData,
						claimsEnvelope ? decodedResult.message
							: "Terrain save rejected a channel payload with a non-canonical byte count."};
				Format::TerrainAreaEnvelopeHeader header;
				header.assetUuid = persistenceRoot.assetUuid;
				header.area = descriptor.coordinate;
				header.channelKind = channelKind;
				header.weightGroup = weightGroup;
				EncodedTerrainArea encoded;
				const TerrainCodecResult encodedResult = EncodeTerrainArea(header, *source, encoded);
				if (!encodedResult.Succeeded()) return encodedResult;
				durableHash = encoded.header.envelopeHash;
				durableBytes = std::move(encoded.bytes);
			}

			*channelHash = durableHash;
			if (!blobIndices.contains(durableHash))
			{
				blobIndices.emplace(durableHash, blobs.size());
				blobs.emplace_back(durableHash, std::move(durableBytes));
			}
			return {};
		};
		for (TerrainDirectoryShard& shard : persistenceRoot.directoryShards)
		{
			for (auto& [coordinate, descriptor] : shard.descriptors)
			{
				(void)coordinate;
				for (const auto [kind, group] : {
					std::pair{TerrainChannelKind::Height, std::uint16_t{0u}},
					std::pair{TerrainChannelKind::Holes, std::uint16_t{0u}}})
				{
					std::optional<ContentHash>& hash = kind == TerrainChannelKind::Height
						? descriptor.channels.heightHash : descriptor.channels.holeHash;
					const TerrainCodecResult result = canonicalizeChannel(descriptor, hash, kind, group);
					if (!result.Succeeded())
					{
						receipt.code = result.code;
						receipt.message = result.message;
						return receipt;
					}
				}
				for (auto& [weightGroup, hash] : descriptor.channels.weightGroupHashes)
				{
					std::optional<ContentHash> optionalHash = hash;
					const TerrainCodecResult result = canonicalizeChannel(
						descriptor, optionalHash, TerrainChannelKind::WeightGroup, weightGroup);
					if (!result.Succeeded())
					{
						receipt.code = result.code;
						receipt.message = result.message;
						return receipt;
					}
					hash = *optionalHash;
				}
			}
		}
		RefreshDirectoryIdentities(persistenceRoot);
		const TerrainRootCodecResult encoded = EncodeRootManifest(persistenceRoot);
		if (!encoded.Succeeded() || !encoded.root) { receipt.code = encoded.code; receipt.diagnostic = encoded.diagnostic; receipt.message = encoded.message; return receipt; }
		const ContentHash rootHash = encoded.contentHash;
		const std::string& manifest = encoded.canonicalBytes;
		const auto normalizedRoot = std::make_shared<const TerrainAssetRoot>(*encoded.root);

		const std::vector<ContentHash> references = CollectReferencedBlobHashes(*capture.root);

		std::error_code error;
		const std::filesystem::path staging = request.dataDirectory / (".staging-" + ToHexString(rootHash));
		std::filesystem::remove_all(staging, error); error.clear();
		std::filesystem::create_directories(staging / "blobs", error);
		if (error) { receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save could not create private staging."; return receipt; }
		for (const auto& [hash, bytes] : blobs)
		{
			if (IsCancelled(request.cancellation)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Cancelled; receipt.message = "Terrain save was cancelled while staging blobs."; return receipt; }
			if (request.injectFailureForTests == TerrainPersistenceFailurePoint::BlobWrite || !WriteDurableBinaryFile(staging / "blobs" / (ToHexString(hash) + ".tearea"), bytes)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save failed before a blob became durable."; return receipt; }
		}
		const auto decoded = DecodeRootManifest(manifest);
		if (!decoded.Succeeded() || !decoded.root || !ValidateRoot(*decoded.root).Succeeded()) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.diagnostic = decoded.diagnostic; receipt.message = "Terrain root staging failed self-validation."; return receipt; }
		// The directory payload is independently staged even though v1 keeps the
		// compact descriptor records in the root manifest as well. This makes the
		// write order explicit for future sharded-directory providers.
		std::filesystem::create_directories(staging / "directories", error);
		if (error || request.injectFailureForTests == TerrainPersistenceFailurePoint::DirectoryWrite || !WriteDurableTextFile(staging / "directories" / (ToHexString(rootHash) + ".dir"), manifest)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable directory could not be staged."; return receipt; }
		std::filesystem::create_directories(staging / "roots", error);
		if (error || request.injectFailureForTests == TerrainPersistenceFailurePoint::ManifestWrite || !WriteDurableTextFile(staging / "roots" / (ToHexString(rootHash) + ".root"), manifest)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable root manifest could not be staged."; return receipt; }
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
				if (!existing || *existing != bytes || !MatchesTerrainBlobIdentity(hash, *existing)) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain immutable blob path already contains different bytes."; return receipt; }
			}
			else std::filesystem::rename(staging / "blobs" / (ToHexString(hash) + ".tearea"), finalBlob, error);
			if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable blob promotion failed."; return receipt; }
		}
		const std::filesystem::path finalDirectory = request.dataDirectory / "directories" / (ToHexString(rootHash) + ".dir");
		if (std::filesystem::exists(finalDirectory))
		{
			const auto existing = ReadBinaryFile(finalDirectory);
			const std::vector<std::uint8_t> expected(manifest.begin(), manifest.end());
			if (!existing || *existing != expected) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain immutable directory path already contains different bytes."; return receipt; }
		}
		else std::filesystem::rename(staging / "directories" / (ToHexString(rootHash) + ".dir"), finalDirectory, error);
		if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable directory promotion failed."; return receipt; }
		const std::filesystem::path finalManifest = request.dataDirectory / "roots" / (ToHexString(rootHash) + ".root");
		if (std::filesystem::exists(finalManifest))
		{
			const auto existing = ReadBinaryFile(finalManifest);
			const std::vector<std::uint8_t> expected(manifest.begin(), manifest.end());
			if (!existing || *existing != expected) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain immutable root path already contains different bytes."; return receipt; }
		}
		else std::filesystem::rename(staging / "roots" / (ToHexString(rootHash) + ".root"), finalManifest, error);
		if (error) { std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain immutable root promotion failed."; return receipt; }

		const TerrainPersistedRootState prior = CapturePersistedRootState();
		const std::filesystem::path currentTemporary = request.dataDirectory / "CURRENT.pending";
		const std::string pointer = ToHexString(rootHash) + "\n" + (prior.savedRootHash.IsValid() ? ToHexString(prior.savedRootHash) : "-") + "\n";
		if (request.injectFailureForTests == TerrainPersistenceFailurePoint::CurrentReplacement || !WriteDurableTextFile(currentTemporary, pointer) || !ReplaceCurrentAtomically(currentTemporary, request.dataDirectory / "CURRENT")) { std::filesystem::remove(currentTemporary, error); std::filesystem::remove_all(staging, error); receipt.code = TerrainResultCode::Failed; receipt.message = "Terrain save left the prior CURRENT pointer intact after replacement failed."; return receipt; }
		std::filesystem::remove_all(staging, error);
		{
			std::scoped_lock lock(persistenceMutex);
			predecessorRootSnapshot = savedRootSnapshot;
			savedRootSnapshot = normalizedRoot;
			persistedRoot.predecessorRootHash = persistedRoot.savedRootHash;
			persistedRoot.savedRootHash = rootHash;
			persistedRoot.savedPublicationGeneration = capture.root->publicationGeneration;
			persistedRoot.recoveredPredecessor = false;
		}
		HashSet reachable(references.begin(), references.end());
		for (const ContentHash hash : CollectPersistedReferencedBlobHashes()) reachable.insert(hash);
		for (const ContentHash hash : blobResolver.SnapshotHashes()) if (!reachable.contains(hash)) receipt.orphanedBlobHashes.push_back(hash);
		receipt.savedRootHash = rootHash; receipt.predecessorRootHash = prior.savedRootHash; receipt.message = "Terrain blobs, canonical TERRAROOT1 manifest, and CURRENT pointer were persisted atomically."; return receipt;
	}

	TerrainPersistenceReceipt TerrainAssetStore::OpenPersistedRoot(const std::filesystem::path& dataDirectory)
	{
		return RecoverPersistedRoot(dataDirectory);
	}

	TerrainPersistenceReceipt TerrainAssetStore::RecoverPersistedRoot(const std::filesystem::path& dataDirectory)
	{
		TerrainPersistenceReceipt receipt{};
		std::ifstream current(dataDirectory / "CURRENT", std::ios::binary); std::string primary; std::string predecessor;
		if (!current || !std::getline(current, primary) || !std::getline(current, predecessor)) { receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain recovery could not read a complete CURRENT pointer."; return receipt; }
		for (const std::string& candidateText : {primary, predecessor})
		{
			if (candidateText == "-") continue;
			const auto candidateHash = ParseContentHash(candidateText); if (!candidateHash) continue;
			std::ifstream manifestFile(dataDirectory / "roots" / (candidateText + ".root"), std::ios::binary);
			if (!manifestFile.is_open()) continue;
			std::stringstream buffer; buffer << manifestFile.rdbuf();
			const TerrainRootCodecResult decodedRoot = DecodeRootManifest(buffer.str());
			if (!decodedRoot.Succeeded() || !decodedRoot.root || decodedRoot.contentHash != *candidateHash
				|| !ValidateRoot(*decodedRoot.root).Succeeded()) continue;
			const TerrainAssetRoot& root = *decodedRoot.root;
			bool allBlobsPresent = true; for (const ContentHash hash : CollectReferencedBlobHashes(root)) if (!std::filesystem::exists(dataDirectory / "blobs" / (ToHexString(hash) + ".tearea"))) { allBlobsPresent = false; break; }
			if (!allBlobsPresent) continue;
			std::shared_ptr<const TerrainAssetRoot> predecessorSnapshot;
			ContentHash validatedPredecessorHash{};
			if (candidateText == primary && predecessor != "-")
			{
				const auto predecessorHash = ParseContentHash(predecessor);
				std::ifstream predecessorFile(dataDirectory / "roots" / (predecessor + ".root"), std::ios::binary);
				std::stringstream predecessorBuffer;
				if (predecessorFile.is_open()) predecessorBuffer << predecessorFile.rdbuf();
				if (predecessorHash && predecessorFile.is_open())
				{
					const TerrainRootCodecResult decodedPredecessor = DecodeRootManifest(predecessorBuffer.str());
					bool predecessorComplete = decodedPredecessor.Succeeded() && decodedPredecessor.root
						&& decodedPredecessor.contentHash == *predecessorHash
						&& decodedPredecessor.root->assetUuid == root.assetUuid
						&& decodedPredecessor.root->publicationGeneration <= root.publicationGeneration
						&& ValidateRoot(*decodedPredecessor.root).Succeeded();
					if(predecessorComplete)
						for(const ContentHash hash : CollectReferencedBlobHashes(*decodedPredecessor.root))
						{
							const auto bytes = ReadBinaryFile(dataDirectory / "blobs" / (ToHexString(hash) + ".tearea"));
							if(!bytes || !MatchesTerrainBlobIdentity(hash, *bytes))
							{
								predecessorComplete = false;
								break;
							}
						}
					if(predecessorComplete)
					{
						predecessorSnapshot = std::make_shared<TerrainAssetRoot>(*decodedPredecessor.root);
						validatedPredecessorHash = *predecessorHash;
					}
				}
			}
			for (const ContentHash hash : CollectReferencedBlobHashes(root))
			{
				const auto bytes = ReadBinaryFile(dataDirectory / "blobs" / (ToHexString(hash) + ".tearea"));
				if (!bytes || !blobResolver.PutVerified(hash, *bytes)) { allBlobsPresent = false; break; }
			}
			if (!allBlobsPresent) continue;
			{
				std::unique_lock lock(rootMutex); currentRoot = std::make_shared<TerrainAssetRoot>(root);
			}
			{
				std::scoped_lock lock(persistenceMutex);
				persistedRoot = {*candidateHash,
					candidateText == primary ? validatedPredecessorHash : ContentHash{},
					root.publicationGeneration, candidateText != primary};
				savedRootSnapshot = std::make_shared<TerrainAssetRoot>(root);
				predecessorRootSnapshot = std::move(predecessorSnapshot);
			}
			receipt.savedRootHash = *candidateHash; receipt.usedPredecessor = candidateText != primary; receipt.message = receipt.usedPredecessor ? "Terrain recovery selected the complete predecessor root." : "Terrain recovery selected CURRENT root."; return receipt;
		}
		receipt.code = TerrainResultCode::CorruptData; receipt.message = "Terrain recovery found neither CURRENT nor predecessor root complete."; return receipt;
	}

	TerrainCodecResult TerrainAssetStore::ExpandAuthoredBounds(const TerrainAssetRoot& base, TerrainSampleBounds expandedBounds, ContentHash candidateRootHash, std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		(void)candidateRootHash;
		outCandidate.reset();
		if (!expandedBounds.IsValid() || expandedBounds.minX > base.authoredBounds.minX || expandedBounds.minZ > base.authoredBounds.minZ || expandedBounds.maxXExclusive < base.authoredBounds.maxXExclusive || expandedBounds.maxZExclusive < base.authoredBounds.maxZExclusive)
			return {TerrainResultCode::InvalidArgument, "Terrain extent expansion may only contain the existing authored bounds."};
		auto candidate = std::make_shared<TerrainAssetRoot>(base);
		candidate->authoredBounds = expandedBounds;
		candidate->publicationGeneration = base.publicationGeneration + 1u;
		if (candidate->persistedRootGeneration == 0u) candidate->persistedRootGeneration = 1u;
		if (candidate->publicationGeneration == 0u)
			return {TerrainResultCode::Overflow, "Terrain extent expansion overflowed its root generation."};
		// Directories and descriptors intentionally remain untouched. This makes
		// implicit default areas visible without resampling any existing channel.
		RefreshDirectoryIdentities(*candidate);
		const TerrainRootCodecResult encoded = EncodeRootManifest(*candidate);
		if (!encoded.Succeeded()) return {encoded.code, encoded.message};
		candidate->rootContentHash = encoded.contentHash;
		outCandidate = std::move(candidate);
		return {};
	}

	TerrainCodecResult TerrainAssetStore::BuildResizeCandidate(const TerrainAssetRoot& base,
		TerrainSampleBounds proposedBounds, const std::vector<TerrainAreaDescriptor>& upserts,
		const std::vector<TerrainAreaCoordinate>& removals,
		std::shared_ptr<const TerrainAssetRoot>& outCandidate) const
	{
		outCandidate.reset();
		if (const TerrainCodecResult validation = ValidateRoot(base); !validation.Succeeded()) return validation;
		const auto proposedAreas = TerrainAreasForSamples(proposedBounds);
		if (!proposedAreas) return {TerrainResultCode::InvalidArgument, "Terrain resize requires non-empty checked sample bounds."};
		for (std::size_t index = 1u; index < removals.size(); ++index)
			if (!(removals[index - 1u] < removals[index]))
				return {TerrainResultCode::InvalidArgument, "Terrain resize removals must be sorted and unique."};
		for (std::size_t index = 1u; index < upserts.size(); ++index)
			if (!(upserts[index - 1u].coordinate < upserts[index].coordinate))
				return {TerrainResultCode::InvalidArgument, "Terrain resize upserts must be sorted and unique."};
		std::map<TerrainAreaCoordinate, TerrainAreaDescriptor> descriptors;
		for (const TerrainDirectoryShard& shard : base.directoryShards)
			for (const auto& [coordinate, descriptor] : shard.descriptors)
				if (proposedAreas->Contains(coordinate)) descriptors[coordinate] = descriptor;
		for (const TerrainAreaCoordinate coordinate : removals) descriptors.erase(coordinate);
		for (const TerrainAreaDescriptor& descriptor : upserts)
		{
			if (!proposedAreas->Contains(descriptor.coordinate) || descriptor.editGeneration == 0u
				|| !descriptor.authoredContentHash.IsValid()
				|| std::binary_search(removals.begin(), removals.end(), descriptor.coordinate))
				return {TerrainResultCode::OutOfBounds, "A resized terrain descriptor is invalid or outside the proposed extent."};
			descriptors[descriptor.coordinate] = descriptor;
		}
		auto candidate = std::make_shared<TerrainAssetRoot>(base);
		candidate->authoredBounds = proposedBounds;
		candidate->publicationGeneration = base.publicationGeneration + 1u;
		if (candidate->publicationGeneration == 0u)
			return {TerrainResultCode::Overflow, "Terrain resize overflowed the publication generation."};
		candidate->directoryShards.clear();
		if (!descriptors.empty())
		{
			TerrainDirectoryShard shard{};
			shard.areaBounds = {proposedAreas->minX, proposedAreas->minZ,
				proposedAreas->maxXExclusive, proposedAreas->maxZExclusive};
			shard.descriptors = std::move(descriptors);
			candidate->directoryShards.push_back(std::move(shard));
		}
		RefreshDirectoryIdentities(*candidate);
		if (const TerrainCodecResult validation = ValidateRoot(*candidate); !validation.Succeeded()) return validation;
		const TerrainRootCodecResult encoded = EncodeRootManifest(*candidate);
		if (!encoded.Succeeded()) return {encoded.code, encoded.message};
		candidate->rootContentHash = encoded.contentHash;
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
		if (candidate->persistedRootGeneration == 0u) candidate->persistedRootGeneration = 1u;
		TerrainDirectoryShard shard{};
		shard.areaBounds = {materialized.coordinate.x, materialized.coordinate.z, materialized.coordinate.x + 1, materialized.coordinate.z + 1};
		shard.descriptors.emplace(materialized.coordinate, std::move(materialized));
		candidate->directoryShards.push_back(std::move(shard));
		if (auto validation = ValidateRoot(*candidate); !validation.Succeeded()) return validation;
		RefreshDirectoryIdentities(*candidate);
		const TerrainRootCodecResult encoded = EncodeRootManifest(*candidate);
		if (!encoded.Succeeded()) return {encoded.code, encoded.message};
		candidate->rootContentHash = encoded.contentHash;
		outCandidate = std::move(candidate);
		return {};
	}
}
