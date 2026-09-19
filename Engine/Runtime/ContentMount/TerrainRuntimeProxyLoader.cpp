// TerrainRuntimeProxyLoader.cpp -- Checked TEPAK terrain decoding for rendering.

#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"

#include "File Formats/TerraEngineMeshFile.h"

#include "Physics/Terrain/TerrainHeightFieldActiveRect.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainContentResolver.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::Runtime
{
	namespace
	{
		struct DecodedMountedArea
		{
			TerraEngine::Terrain::TerrainAreaDescriptor descriptor{};
			std::vector<std::int32_t> heights;
			std::vector<std::uint8_t> holes;
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
			const auto read = TerraEngine::TerrainContentResolver::ReadPrivateBundleFile(
				mount, logicalPath, maximumBytes);
			if(!read.Succeeded() || read.bytes.empty()) return std::nullopt;
			return std::string(read.bytes.begin(), read.bytes.end());
		}

		std::optional<TerraEngine::Terrain::TerrainAssetRoot> DecodeImmutableRoot(
			std::string_view rootText,
			std::string_view expectedRootHash,
			const TerraEngine::UUID& expectedAssetId)
		{
			const auto expectedHash = ParseContentHash(expectedRootHash);
			const auto decoded = TerraEngine::Terrain::DecodeTerrainRootManifest(rootText);
			if(!expectedHash || !decoded.Succeeded() || !decoded.root
				|| decoded.root->assetUuid != expectedAssetId || decoded.contentHash != *expectedHash) return std::nullopt;
			const TerraEngine::Terrain::TerrainAssetRoot& root = *decoded.root;

			const auto rootSnapshot = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(root);
			TerraEngine::Terrain::TerrainAssetStore validator(rootSnapshot);
			if(!validator.ValidateRoot(root).Succeeded()) return std::nullopt;
			return root;
		}

		bool DecodeArea(
			const ::ContentMount& mount,
			const std::filesystem::path& dataRoot,
			const TerraEngine::Terrain::ContentHash& expectedHash,
			const TerraEngine::UUID& expectedAssetId,
			TerraEngine::Terrain::TerrainAreaCoordinate expectedArea,
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
					|| decoded.header.area != expectedArea
					|| decoded.header.channelKind != expectedChannel)
				{
					outError = "The mounted terrain area identity does not match its immutable root.";
					return false;
				}
				outPayload = std::move(decoded.canonicalPayload);
				return true;
			}

			// Saved TERRAROOT1 generations may store canonical channel bytes directly
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

		TerraEngine::Physics::TerrainHeightFieldRecipe BuildCollisionRecipe(
			const DecodedMountedArea& area,
			const TerraEngine::UUID& assetId,
			double sampleSpacingMetres,
			TerraEngine::Terrain::TerrainSampleBounds authoredBounds)
		{
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = assetId;
			recipe.areaX = area.descriptor.coordinate.x;
			recipe.areaZ = area.descriptor.coordinate.z;
			recipe.authoredGeneration = area.descriptor.editGeneration;
			recipe.authoredHash = area.descriptor.authoredContentHash.low64
				^ area.descriptor.authoredContentHash.high64;
			if(recipe.authoredHash == 0U) recipe.authoredHash = 1U;
			recipe.sampleSpacingMetres = static_cast<float>(sampleSpacingMetres);
			recipe.heightMillimetres.resize(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount);
			for(std::uint32_t z = 0U; z < TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent; ++z)
			{
				for(std::uint32_t x = 0U; x < TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent; ++x)
				{
					recipe.heightMillimetres[
						static_cast<std::size_t>(z) * TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent + x]
						= area.heights[static_cast<std::size_t>(std::min(z, 255U)) * 256U + std::min(x, 255U)];
				}
			}
			recipe.holeBits = area.holes;
			if(recipe.holeBits.empty())
				recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0U);
			const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(
				authoredBounds, area.descriptor.coordinate);
			// A malformed mounted extent must not turn fixed-grid padding into walkable
			// collision. Clear the mask so recipe validation rejects this candidate at the
			// same protected boundary rather than publishing broadened support.
			if(!active || !TerraEngine::Physics::Terrain::ApplyTerrainActiveRectToRecipe(recipe, *active))
				recipe.holeBits.clear();
			return recipe;
		}

		std::vector<std::int32_t> BuildCookHalo(
			const std::map<TerraEngine::Terrain::TerrainAreaCoordinate, DecodedMountedArea>& areas,
			TerraEngine::Terrain::TerrainAreaCoordinate coordinate,
			bool& outMissingOuterHalo)
		{
			constexpr std::uint32_t kHaloExtent = 258U;
			std::vector<std::int32_t> halo(static_cast<std::size_t>(kHaloExtent) * kHaloExtent);
			const DecodedMountedArea& current = areas.at(coordinate);
			for(std::int32_t haloZ = 0; haloZ < static_cast<std::int32_t>(kHaloExtent); ++haloZ)
			{
				for(std::int32_t haloX = 0; haloX < static_cast<std::int32_t>(kHaloExtent); ++haloX)
				{
					std::int32_t localX = haloX - 1;
					std::int32_t localZ = haloZ - 1;
					TerraEngine::Terrain::TerrainAreaCoordinate sourceCoordinate = coordinate;
					if(localX < 0) { --sourceCoordinate.x; localX += 256; }
					else if(localX >= 256) { ++sourceCoordinate.x; localX -= 256; }
					if(localZ < 0) { --sourceCoordinate.z; localZ += 256; }
					else if(localZ >= 256) { ++sourceCoordinate.z; localZ -= 256; }
					const auto source = areas.find(sourceCoordinate);
					if(source == areas.end())
					{
						// A missing descriptor is an outer sparse boundary, not a corrupt
						// interior blob. Clamp only this normal/completion sample; descriptors
						// that exist but fail decode reject the complete root before this point.
						outMissingOuterHalo = true;
						localX = std::clamp(haloX - 1, 0, 255);
						localZ = std::clamp(haloZ - 1, 0, 255);
						halo[static_cast<std::size_t>(haloZ) * kHaloExtent + haloX]
							= current.heights[static_cast<std::size_t>(localZ) * 256U + localX];
						continue;
					}
					halo[static_cast<std::size_t>(haloZ) * kHaloExtent + haloX]
						= source->second.heights[static_cast<std::size_t>(localZ) * 256U + localX];
				}
			}
			return halo;
		}
	}

	MountedTerrainRuntimeResult BuildMountedTerrainRuntime(
		const ::ContentMount& mount,
		const TerraEngine::UUID& expectedAssetId,
		std::string_view logicalTerrainPath)
	{
		MountedTerrainRuntimeResult failed{};
		failed.renderProxy.assetId = expectedAssetId;
		if(expectedAssetId == TerraEngine::EMPTY_UUID || logicalTerrainPath.empty())
		{
			failed.renderProxy.diagnostic = "Mounted terrain proxy preparation requires a durable asset ID and logical path.";
			return failed;
		}

		const TerraEngine::TerrainContentReadResult descriptorRead =
			TerraEngine::TerrainContentResolver::ReadRoot(mount, logicalTerrainPath);
		if(!descriptorRead.Succeeded())
		{
			failed.renderProxy.diagnostic = "The terrain descriptor is unavailable from the mounted export package.";
			return failed;
		}

		double descriptorSampleSpacingMetres = 1.0;
		try
		{
			const nlohmann::json descriptor = nlohmann::json::parse(descriptorRead.bytes);
			const auto descriptorAssetId = descriptor.contains("assetId") && descriptor["assetId"].is_string()
				? TerraEngine::ParseUUID(descriptor["assetId"].get<std::string>()) : std::nullopt;
			descriptorSampleSpacingMetres = descriptor.value("sampleSpacingMetres", 0.0);
			if(descriptor.value("format", std::string{}) != "TerraEngineTerrain"
				|| descriptor.value("rootFormat", std::string{}) != "TERRAROOT1"
				|| descriptor.value("version", 0U) != 1U || !descriptorAssetId
				|| *descriptorAssetId != expectedAssetId || !std::isfinite(descriptorSampleSpacingMetres)
				|| descriptorSampleSpacingMetres <= 0.0)
			{
				failed.renderProxy.diagnostic = "The mounted terrain descriptor has invalid format, identity, or sample spacing.";
				return failed;
			}
		}
		catch(const nlohmann::json::exception& exception)
		{
			failed.renderProxy.diagnostic = std::string("The mounted terrain descriptor is invalid: ") + exception.what();
			return failed;
		}

		const std::filesystem::path terrainPath(logicalTerrainPath);
		const std::filesystem::path dataRoot(terrainPath.generic_string() + ".data");
		const auto currentText = ReadMountedText(mount, (dataRoot / "CURRENT").generic_string(), 4096U);
		if(!currentText)
		{
			failed.renderProxy.diagnostic = "The mounted terrain bundle has no readable CURRENT pointer.";
			return failed;
		}
		std::istringstream current(*currentText);
		std::string primary;
		std::string predecessor;
		std::getline(current, primary);
		std::getline(current, predecessor);
		if(primary.empty())
		{
			failed.renderProxy.diagnostic = "The mounted terrain CURRENT pointer is empty.";
			return failed;
		}

		for(const std::string& candidate : {primary, predecessor})
		{
			if(candidate.empty() || candidate == "-" || candidate.find("..") != std::string::npos) continue;
			std::optional<TerraEngine::Terrain::TerrainAssetRoot> root;
			if(ParseContentHash(candidate))
			{
				const auto rootText = ReadMountedText(
					mount, (dataRoot / "roots" / (candidate + ".root")).generic_string(), 64U * 1024U * 1024U);
				if(rootText) root = DecodeImmutableRoot(*rootText, candidate, expectedAssetId);
			}
			if(!root) continue;
			if(root->sampleSpacingMetres != descriptorSampleSpacingMetres)
			{
				failed.renderProxy.diagnostic = "The public terrain descriptor and canonical TERRAROOT1 disagree on sample spacing.";
				continue;
			}

			std::map<TerraEngine::Terrain::TerrainAreaCoordinate, DecodedMountedArea> decodedAreas;
			bool generationComplete = true;
			const auto rootSnapshot = std::make_shared<const TerraEngine::Terrain::TerrainAssetRoot>(*root);
			TerraEngine::Terrain::TerrainAssetStore rootResolver(rootSnapshot);
			const auto currentAreas = TerraEngine::Terrain::EnumerateTerrainAreas(root->authoredBounds, 1024u * 1024u);
			if (!currentAreas)
			{
				failed.renderProxy.diagnostic = "The mounted terrain authored extent exceeds the runtime area budget.";
				continue;
			}
			for(const auto coordinate : *currentAreas)
			{
				const auto explicitDescriptor = rootResolver.FindDescriptor(*root, coordinate);
				const auto descriptor = rootResolver.ResolveDescriptorOrImplicit(*root, coordinate);
				DecodedMountedArea decoded{};
				decoded.descriptor = descriptor;
				if (!explicitDescriptor)
				{
					// Sparse expansion remains complete at coarse/direct level without
					// manufacturing blobs for areas that still equal root defaults.
					decoded.heights.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, root->defaultHeightMm);
					decodedAreas.emplace(coordinate, std::move(decoded));
					continue;
				}
				if(!descriptor.channels.heightHash || descriptor.editGeneration == 0U
					|| !descriptor.authoredContentHash.IsValid()) { generationComplete = false; break; }
					std::vector<std::uint8_t> heightPayload;
					if(!DecodeArea(mount, dataRoot, *descriptor.channels.heightHash, expectedAssetId,
						coordinate, TerraEngine::Terrain::TerrainChannelKind::Height,
						heightPayload, failed.renderProxy.diagnostic)
						|| heightPayload.size() != TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t))
					{
						generationComplete = false;
						break;
					}
					decoded.heights.resize(TerraEngine::Terrain::kTerrainAreaSampleCount);
					std::memcpy(decoded.heights.data(), heightPayload.data(), heightPayload.size());
					if(descriptor.channels.holeHash
						&& (!DecodeArea(mount, dataRoot, *descriptor.channels.holeHash, expectedAssetId,
							coordinate, TerraEngine::Terrain::TerrainChannelKind::Holes,
							decoded.holes, failed.renderProxy.diagnostic)
							|| decoded.holes.size() != TerraEngine::Terrain::kTerrainAreaSampleCount / 8U))
					{
						generationComplete = false;
						break;
					}
				decodedAreas.emplace(coordinate, std::move(decoded));
			}
			if(!generationComplete || decodedAreas.empty()) continue;

			MountedTerrainRuntimeResult completed{};
			completed.areas.reserve(decodedAreas.size());
			const std::array<TerraEngine::Terrain::TerrainAreaCoordinate, 4U> neighborOffsets{
				TerraEngine::Terrain::TerrainAreaCoordinate{-1, 0},
				TerraEngine::Terrain::TerrainAreaCoordinate{1, 0},
				TerraEngine::Terrain::TerrainAreaCoordinate{0, -1},
				TerraEngine::Terrain::TerrainAreaCoordinate{0, 1}};
			for(const auto& [coordinate, decoded] : decodedAreas)
			{
				MountedTerrainAreaRuntime areaRuntime{};
				areaRuntime.area = coordinate;
				areaRuntime.authoredHash = decoded.descriptor.authoredContentHash;
				areaRuntime.authoredGeneration = decoded.descriptor.editGeneration;
				areaRuntime.implicitDefault = !rootResolver.FindDescriptor(*root, coordinate).has_value();
				const auto activeRect = TerraEngine::Terrain::TerrainActiveRectForArea(
					root->authoredBounds, coordinate);
				if(!activeRect)
				{
					failed.renderProxy.diagnostic = "A mounted terrain area lies outside the root-authored extent.";
					generationComplete = false;
					break;
				}
				areaRuntime.activeRect = *activeRect;
				TerraEngine::Renderer::Terrain::TerrainDirectMeshInput directInput{};
				directInput.assetId = expectedAssetId;
				directInput.area = coordinate;
				directInput.authoredHash = areaRuntime.authoredHash;
				directInput.authoredGeneration = areaRuntime.authoredGeneration;
				directInput.sampleSpacingMetres = static_cast<float>(root->sampleSpacingMetres);
				directInput.activeRect = areaRuntime.activeRect;
				directInput.heightMillimetres = decoded.heights;
				directInput.holeCellBits = decoded.holes;
				areaRuntime.directMesh = TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(directInput);

				std::vector<std::int32_t> halo = BuildCookHalo(
					decodedAreas, coordinate, areaRuntime.missingOuterHalo);
				TerraEngine::Renderer::Terrain::TerrainCookInput cookInput{};
				cookInput.assetId = expectedAssetId;
				cookInput.area = coordinate;
				cookInput.authoredHash = areaRuntime.authoredHash;
				cookInput.authoredGeneration = areaRuntime.authoredGeneration;
				cookInput.sampleSpacingMetres = root->sampleSpacingMetres;
				cookInput.activeRect = areaRuntime.activeRect;
				cookInput.heightMillimetres = halo;
				cookInput.holeCellBits = decoded.holes;
				for(std::size_t edge = 0U; edge < neighborOffsets.size(); ++edge)
				{
					const TerraEngine::Terrain::TerrainAreaCoordinate neighbor{
						coordinate.x + neighborOffsets[edge].x,
						coordinate.z + neighborOffsets[edge].z};
					const auto found = decodedAreas.find(neighbor);
					if(found != decodedAreas.end())
						cookInput.neighborAuthoredHashes[edge] = found->second.descriptor.authoredContentHash;
				}
				areaRuntime.pageCook = TerraEngine::Renderer::Terrain::CookTerrainPage(cookInput);
				areaRuntime.collisionRecipe = BuildCollisionRecipe(decoded, expectedAssetId,
					root->sampleSpacingMetres, root->authoredBounds);
				if(!areaRuntime.Succeeded())
				{
					failed.renderProxy.diagnostic = "A mounted terrain area failed direct, page, or collision preparation.";
					generationComplete = false;
					break;
				}
				completed.areas.push_back(std::move(areaRuntime));
			}
			if(!generationComplete) continue;
			const auto origin = std::find_if(completed.areas.begin(), completed.areas.end(), [](const auto& area) {
				return area.area == TerraEngine::Terrain::TerrainAreaCoordinate{0, 0};
			});
			const MountedTerrainAreaRuntime& compatibility = origin == completed.areas.end()
				? completed.areas.front() : *origin;
			const DecodedMountedArea& compatibilitySource = decodedAreas.at(compatibility.area);
			TerraEngine::Renderer::Terrain::TerrainDirectMeshInput compatibilityInput{};
			compatibilityInput.assetId = expectedAssetId;
			compatibilityInput.area = compatibility.area;
			compatibilityInput.authoredHash = compatibility.authoredHash;
			compatibilityInput.authoredGeneration = compatibility.authoredGeneration;
			compatibilityInput.sampleSpacingMetres = static_cast<float>(root->sampleSpacingMetres);
			if (const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(root->authoredBounds, compatibility.area))
				compatibilityInput.activeRect = *active;
			compatibilityInput.heightMillimetres = compatibilitySource.heights;
			compatibilityInput.holeCellBits = compatibilitySource.holes;
			completed.renderProxy = TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(compatibilityInput);
			completed.collisionRecipe = BuildCollisionRecipe(
				compatibilitySource, expectedAssetId, root->sampleSpacingMetres, root->authoredBounds);
			completed.sourceRootHash = *ParseContentHash(candidate);
			completed.sourcePublicationGeneration = root->publicationGeneration;
			completed.authoredBounds = root->authoredBounds;
			return completed;
		}

		if(failed.renderProxy.diagnostic.empty())
			failed.renderProxy.diagnostic = "Neither mounted terrain CURRENT generation contains a complete multi-area runtime set.";
		return failed;
	}

	TerraEngine::Renderer::Terrain::TerrainDirectMeshResult BuildMountedTerrainDirectMesh(
		const ::ContentMount& mount,
		const TerraEngine::UUID& expectedAssetId,
		std::string_view logicalTerrainPath)
	{
		return std::move(BuildMountedTerrainRuntime(mount, expectedAssetId, logicalTerrainPath).renderProxy);
	}
}
