// TerrainRuntimeProxyLoader.cpp -- Checked TEPAK terrain decoding for rendering.

#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainContentResolver.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::Runtime
{
	namespace
	{
		struct TerrainOriginSource
		{
			TerraEngine::Terrain::ContentHash authoredHash{};
			TerraEngine::Terrain::ContentHash heightHash{};
			std::optional<TerraEngine::Terrain::ContentHash> holeHash;
			TerraEngine::Terrain::Generation generation = 0U;
		};

		std::optional<TerraEngine::Terrain::ContentHash> ParseContentHash(std::string_view text)
		{
			if(text.size() != 32U) return std::nullopt;
			auto parseHalf = [text](std::size_t offset) -> std::optional<std::uint64_t>
			{
				std::uint64_t value = 0U;
				const auto parsed = std::from_chars(
					text.data() + offset, text.data() + offset + 16U, value, 16);
				return parsed.ec == std::errc{} && parsed.ptr == text.data() + offset + 16U
					? std::optional<std::uint64_t>{value} : std::nullopt;
			};
			const auto high = parseHalf(0U);
			const auto low = parseHalf(16U);
			return high && low
				? std::optional<TerraEngine::Terrain::ContentHash>{{*low, *high}}
				: std::nullopt;
		}

		std::optional<std::string> ReadMountedText(
			const ::ContentMount& mount,
			std::string_view logicalPath,
			std::uint64_t maximumBytes)
		{
			const MountedContentReadResult read = mount.ReadAllChecked(logicalPath, maximumBytes);
			if(!read.Succeeded() || read.bytes.empty()) return std::nullopt;
			return std::string(read.bytes.begin(), read.bytes.end());
		}

		std::optional<TerrainOriginSource> DecodeLegacyOrigin(
			std::string_view rootText,
			const TerraEngine::UUID& expectedAssetId)
		{
			try
			{
				const nlohmann::json root = nlohmann::json::parse(rootText);
				const auto assetId = root.contains("assetId") && root["assetId"].is_string()
					? TerraEngine::ParseUUID(root["assetId"].get<std::string>()) : std::nullopt;
				if(!assetId || *assetId != expectedAssetId || !root.contains("areas")
					|| !root["areas"].is_array()) return std::nullopt;
				for(const nlohmann::json& area : root["areas"])
				{
					if(!area.is_object() || area.value("x", 1) != 0 || area.value("z", 1) != 0
						|| !area.contains("heightHash") || !area["heightHash"].is_string()) continue;
					const auto heightHash = ParseContentHash(area["heightHash"].get<std::string>());
					const std::uint64_t generation = root.value("generation", 0ULL);
					if(!heightHash || generation == 0U) return std::nullopt;
					return TerrainOriginSource{*heightHash, *heightHash, std::nullopt, generation};
				}
			}
			catch(const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
			return std::nullopt;
		}

		std::optional<TerrainOriginSource> DecodeImmutableOrigin(
			std::string_view rootText,
			std::string_view expectedRootHash,
			const TerraEngine::UUID& expectedAssetId)
		{
			const auto expectedHash = ParseContentHash(expectedRootHash);
			const auto root = TerraEngine::Terrain::DecodeTerrainRootManifest(rootText);
			if(!expectedHash || !root || root->assetUuid != expectedAssetId
				|| root->rootContentHash != *expectedHash) return std::nullopt;

			const auto rootSnapshot = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(*root);
			TerraEngine::Terrain::TerrainAssetStore validator(rootSnapshot);
			if(!validator.ValidateRoot(*root).Succeeded()) return std::nullopt;
			const auto descriptor = validator.FindDescriptor(*root, {0, 0});
			if(!descriptor || !descriptor->channels.heightHash
				|| descriptor->editGeneration == 0U
				|| !descriptor->authoredContentHash.IsValid()) return std::nullopt;
			return TerrainOriginSource{
				descriptor->authoredContentHash,
				*descriptor->channels.heightHash,
				descriptor->channels.holeHash,
				descriptor->editGeneration};
		}

		bool DecodeArea(
			const ::ContentMount& mount,
			const std::filesystem::path& dataRoot,
			const TerraEngine::Terrain::ContentHash& expectedHash,
			const TerraEngine::UUID& expectedAssetId,
			TerraEngine::Terrain::TerrainChannelKind expectedChannel,
			std::vector<std::uint8_t>& outPayload,
			std::string& outError)
		{
			const std::filesystem::path blobPath = dataRoot / "blobs"
				/ (TerraEngine::Terrain::ToHexString(expectedHash) + ".tearea");
			const TerraEngine::TerrainContentReadResult read =
				TerraEngine::TerrainContentResolver::ReadAreaBlob(mount, blobPath.generic_string());
			if(!read.Succeeded())
			{
				outError = "The mounted terrain area blob is missing or has the wrong package class: "
					+ blobPath.generic_string();
				return false;
			}
			TerraEngine::Terrain::DecodedTerrainArea decoded;
			const auto decode = TerraEngine::Terrain::DecodeTerrainArea(read.bytes, decoded);
			if(decode.Succeeded())
			{
				// Initial import generations use the self-describing TEAREA envelope.
				// Validate its embedded identity before exposing the decoded channel.
				if(decoded.header.envelopeHash != expectedHash
					|| decoded.header.assetUuid != expectedAssetId
					|| decoded.header.area != TerraEngine::Terrain::TerrainAreaCoordinate{0, 0}
					|| decoded.header.channelKind != expectedChannel)
				{
					outError = "The mounted terrain area identity does not match its immutable root.";
					return false;
				}
				outPayload = std::move(decoded.canonicalPayload);
				return true;
			}

			// Saved TERRAINROOT2 generations store canonical channel bytes directly
			// under their content hash. This is the format produced after an editor
			// sculpt/save; accepting it is required for authored exports while the
			// exact hash and fixed channel size still fail closed on corruption.
			const std::size_t expectedRawBytes = expectedChannel == TerraEngine::Terrain::TerrainChannelKind::Height
				? TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t)
				: expectedChannel == TerraEngine::Terrain::TerrainChannelKind::Holes
				? TerraEngine::Terrain::kTerrainAreaSampleCount / 8U : 0U;
			if(expectedRawBytes == 0U || read.bytes.size() != expectedRawBytes
				|| TerraEngine::Terrain::HashTerrainBytes(read.bytes) != expectedHash)
			{
				outError = decode.message;
				return false;
			}
			outPayload = read.bytes;
			return true;
		}
	}

	TerraEngine::Renderer::Terrain::TerrainDirectMeshResult BuildMountedTerrainDirectMesh(
		const ::ContentMount& mount,
		const TerraEngine::UUID& expectedAssetId,
		std::string_view logicalTerrainPath)
	{
		TerraEngine::Renderer::Terrain::TerrainDirectMeshResult failed{};
		failed.assetId = expectedAssetId;
		if(expectedAssetId == TerraEngine::EMPTY_UUID || logicalTerrainPath.empty())
		{
			failed.diagnostic = "Mounted terrain proxy preparation requires a durable asset ID and logical path.";
			return failed;
		}

		const TerraEngine::TerrainContentReadResult descriptorRead =
			TerraEngine::TerrainContentResolver::ReadRoot(mount, logicalTerrainPath);
		if(!descriptorRead.Succeeded())
		{
			failed.diagnostic = "The terrain descriptor is unavailable from the mounted export package.";
			return failed;
		}

		float sampleSpacingMetres = 1.0F;
		try
		{
			const nlohmann::json descriptor = nlohmann::json::parse(descriptorRead.bytes);
			const auto descriptorAssetId = descriptor.contains("assetId") && descriptor["assetId"].is_string()
				? TerraEngine::ParseUUID(descriptor["assetId"].get<std::string>()) : std::nullopt;
			sampleSpacingMetres = descriptor.value("sampleSpacingMetres", 0.0F);
			if(descriptor.value("format", std::string{}) != "TerraEngineTerrain"
				|| descriptor.value("version", 0U) != 1U || !descriptorAssetId
				|| *descriptorAssetId != expectedAssetId || !std::isfinite(sampleSpacingMetres)
				|| sampleSpacingMetres <= 0.0F)
			{
				failed.diagnostic = "The mounted terrain descriptor has invalid format, identity, or sample spacing.";
				return failed;
			}
		}
		catch(const nlohmann::json::exception& exception)
		{
			failed.diagnostic = std::string("The mounted terrain descriptor is invalid: ") + exception.what();
			return failed;
		}

		const std::filesystem::path terrainPath(logicalTerrainPath);
		const std::filesystem::path dataRoot(terrainPath.generic_string() + ".data");
		const auto currentText = ReadMountedText(mount, (dataRoot / "CURRENT").generic_string(), 4096U);
		if(!currentText)
		{
			failed.diagnostic = "The mounted terrain bundle has no readable CURRENT pointer.";
			return failed;
		}
		std::istringstream current(*currentText);
		std::string primary;
		std::string predecessor;
		std::getline(current, primary);
		std::getline(current, predecessor);
		if(primary.empty())
		{
			failed.diagnostic = "The mounted terrain CURRENT pointer is empty.";
			return failed;
		}

		for(const std::string& candidate : {primary, predecessor})
		{
			if(candidate.empty() || candidate == "-" || candidate.find("..") != std::string::npos) continue;
			std::optional<TerrainOriginSource> origin;
			if(candidate.starts_with("roots/"))
			{
				const auto rootText = ReadMountedText(
					mount, (dataRoot / std::filesystem::path(candidate)).generic_string(), 4U * 1024U * 1024U);
				if(rootText) origin = DecodeLegacyOrigin(*rootText, expectedAssetId);
			}
			else if(ParseContentHash(candidate))
			{
				const auto rootText = ReadMountedText(
					mount, (dataRoot / "roots" / (candidate + ".root")).generic_string(), 64U * 1024U * 1024U);
				if(rootText) origin = DecodeImmutableOrigin(*rootText, candidate, expectedAssetId);
			}
			if(!origin) continue;

			std::vector<std::uint8_t> heightPayload;
			if(!DecodeArea(mount, dataRoot, origin->heightHash, expectedAssetId,
				TerraEngine::Terrain::TerrainChannelKind::Height, heightPayload, failed.diagnostic)) continue;
			const std::size_t expectedHeightBytes = TerraEngine::Terrain::kTerrainAreaSampleCount
				* sizeof(std::int32_t);
			if(heightPayload.size() != expectedHeightBytes)
			{
				failed.diagnostic = "The mounted terrain origin height payload has the wrong decoded size.";
				continue;
			}
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount);
			std::memcpy(heights.data(), heightPayload.data(), heightPayload.size());

			std::vector<std::uint8_t> holes;
			if(origin->holeHash)
			{
				if(!DecodeArea(mount, dataRoot, *origin->holeHash, expectedAssetId,
					TerraEngine::Terrain::TerrainChannelKind::Holes, holes, failed.diagnostic)) continue;
				if(holes.size() != TerraEngine::Terrain::kTerrainAreaSampleCount / 8U)
				{
					failed.diagnostic = "The mounted terrain origin hole payload has the wrong decoded size.";
					continue;
				}
			}

			TerraEngine::Renderer::Terrain::TerrainDirectMeshInput input{};
			input.assetId = expectedAssetId;
			input.authoredHash = origin->authoredHash;
			input.authoredGeneration = origin->generation;
			input.sampleSpacingMetres = sampleSpacingMetres;
			input.heightMillimetres = heights;
			input.holeCellBits = holes;
			return TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(input);
		}

		if(failed.diagnostic.empty())
			failed.diagnostic = "Neither mounted terrain CURRENT generation contains a complete origin area.";
		return failed;
	}
}
