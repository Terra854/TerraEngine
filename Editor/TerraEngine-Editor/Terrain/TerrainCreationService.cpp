// TerrainCreationService.cpp -- Atomic parent terrain asset creation.

#include "TerrainCreationService.h"

#include "Editor/TerraEngine-Editor/Assets/AssetDatabase.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"
#include "File Formats/TerraEngineTerrainFile.h"
#include "Resource/ResourceManager.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		bool StagePrivateGeneration(const TerraEngine::AssetPipeline::TerrainImportRequest& request,
			const TerraEngine::AssetPipeline::TerrainHeightfield& heightfield, const std::filesystem::path& stagingDirectory,
			std::string& outError)
		{
			if(heightfield.width == 0U || heightfield.height == 0U
				|| heightfield.heightsMm.size() != static_cast<std::size_t>(heightfield.width) * heightfield.height)
			{
				outError = "The decoded terrain heightfield dimensions are invalid.";
				return false;
			}
			std::error_code error;
			const std::uint32_t areaCountX = (heightfield.width + 255U) / 256U;
			const std::uint32_t areaCountZ = (heightfield.height + 255U) / 256U;
			TerraEngine::Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {0, 0, static_cast<std::int64_t>(areaCountX), static_cast<std::int64_t>(areaCountZ)};
			std::vector<std::uint8_t> shardIdentityBytes;
			for(std::uint32_t areaZ = 0U; areaZ < areaCountZ; ++areaZ) for(std::uint32_t areaX = 0U; areaX < areaCountX; ++areaX)
			{
				// The final edge area repeats its nearest authored sample outside the
				// declared extent. That explicit default-edge policy avoids resampling
				// imported data while still satisfying the fixed-area codec contract.
				std::vector<std::int32_t> areaHeights(256U * 256U);
				for(std::uint32_t localZ = 0U; localZ < 256U; ++localZ) for(std::uint32_t localX = 0U; localX < 256U; ++localX)
				{
					const std::uint32_t sourceX = std::min(areaX * 256U + localX, heightfield.width - 1U);
					const std::uint32_t sourceZ = std::min(areaZ * 256U + localZ, heightfield.height - 1U);
					areaHeights[static_cast<std::size_t>(localZ) * 256U + localX] = heightfield.heightsMm[static_cast<std::size_t>(sourceZ) * heightfield.width + sourceX];
				}
				std::vector<std::uint8_t> payload(areaHeights.size() * sizeof(std::int32_t));
				std::memcpy(payload.data(), areaHeights.data(), payload.size());
				TerraEngine::Terrain::Format::TerrainAreaEnvelopeHeader header;
				header.assetUuid = request.assetId; header.area = {static_cast<std::int64_t>(areaX), static_cast<std::int64_t>(areaZ)};
				header.channelKind = TerraEngine::Terrain::TerrainChannelKind::Height;
				TerraEngine::Terrain::EncodedTerrainArea encoded;
				const auto encodeResult = TerraEngine::Terrain::EncodeTerrainArea(header, payload, encoded);
				if(!encodeResult.Succeeded()) { outError = encodeResult.message; return false; }
				const std::string areaHash = TerraEngine::Terrain::ToHexString(encoded.header.envelopeHash);
				const std::filesystem::path blob = stagingDirectory / "blobs" / (areaHash + ".tearea");
				std::filesystem::create_directories(blob.parent_path(), error);
				if(error) { outError = "The staged terrain blob directory could not be created."; return false; }
				std::ofstream blobOutput(blob, std::ios::binary | std::ios::trunc);
				if(!blobOutput) { outError = "The staged terrain area blob could not be opened."; return false; }
				blobOutput.write(reinterpret_cast<const char*>(encoded.bytes.data()), static_cast<std::streamsize>(encoded.bytes.size())); blobOutput.close();
				if(!blobOutput) { outError = "The staged terrain area blob could not be written."; return false; }
				TerraEngine::Terrain::TerrainAreaDescriptor descriptor{};
				descriptor.coordinate = {static_cast<std::int64_t>(areaX), static_cast<std::int64_t>(areaZ)};
				descriptor.editGeneration = 1U;
				descriptor.channels.heightHash = encoded.header.envelopeHash;
				descriptor.authoredContentHash = encoded.header.envelopeHash;
				descriptor.layerPresence = {1U};
				const auto [minimum, maximum] = std::minmax_element(areaHeights.begin(), areaHeights.end());
				descriptor.minHeightMm = *minimum;
				descriptor.maxHeightMm = *maximum;
				shard.descriptors.emplace(descriptor.coordinate, std::move(descriptor));
				shardIdentityBytes.insert(shardIdentityBytes.end(), areaHash.begin(), areaHash.end());
			}
			shard.identity = TerraEngine::Terrain::HashTerrainBytes(shardIdentityBytes);
			TerraEngine::Terrain::TerrainLayerStack layerStack{};
			layerStack.generation = 1U;
			TerraEngine::Terrain::TerrainLayer defaultLayer{};
			defaultLayer.stableId = 1U;
			defaultLayer.name = "Default";
			defaultLayer.enabled = true;
			defaultLayer.allowFallback = true;
			defaultLayer.fallbackState = TerraEngine::Terrain::TerrainLayerFallbackState::FallbackActive;
			layerStack.layers.push_back(std::move(defaultLayer));
			TerraEngine::Terrain::TerrainAssetRoot root{};
			root.assetUuid = request.assetId;
			root.publicationGeneration = 1U;
			root.persistedRootGeneration = 1U;
			root.sampleSpacingMetres = request.sampleSpacingMetres;
			root.authoredBounds = {0, 0, static_cast<std::int64_t>(heightfield.width), static_cast<std::int64_t>(heightfield.height)};
			root.defaultLayerId = 1U;
			root.layerStackGeneration = 1U;
			root.layerStack = std::make_shared<const TerraEngine::Terrain::TerrainLayerStack>(std::move(layerStack));
			root.directoryShards = {std::move(shard)};
			const auto encodedRoot = TerraEngine::Terrain::EncodeTerrainRootManifest(root);
			if(!encodedRoot.Succeeded()) { outError = encodedRoot.message; return false; }
			const std::string rootHash = TerraEngine::Terrain::ToHexString(encodedRoot.contentHash);
			const std::filesystem::path rootManifest = stagingDirectory / "roots" / (rootHash + ".root");
			std::filesystem::create_directories(rootManifest.parent_path(), error);
			if(error) { outError = "The staged terrain root directory could not be created."; return false; }
			std::ofstream rootOutput(rootManifest, std::ios::binary | std::ios::trunc);
			rootOutput.write(encodedRoot.canonicalBytes.data(), static_cast<std::streamsize>(encodedRoot.canonicalBytes.size()));
			rootOutput.close();
			if(!rootOutput) { outError = "The staged terrain root manifest could not be written."; return false; }
			std::ofstream currentOutput(stagingDirectory / "CURRENT", std::ios::binary | std::ios::trunc);
			currentOutput << rootHash << "\n-\n"; currentOutput.close();
			if(!currentOutput) { outError = "The staged terrain CURRENT pointer could not be written."; return false; }
			return true;
		}
	}

	TerrainCreationService CreateProjectTerrainCreationService()
	{
		return TerrainCreationService(
			[](const std::filesystem::path& assetPath, std::string& outError)
			{
				AssetDatabase& assets = AssetDatabase::Instance();
				std::filesystem::path absolutePath = assetPath;
				if(!absolutePath.is_absolute())
					absolutePath = assets.GetAssetRoot().parent_path() / absolutePath;
				absolutePath = std::filesystem::absolute(absolutePath).lexically_normal();

				// Import only after the descriptor and private generation have both
				// been published. A failed registration makes CreateOrImport roll the
				// complete bundle back instead of leaving a half-visible browser item.
				assets.ImportAsset(absolutePath);
				const auto registered = assets.GetAssets().find(absolutePath);
				if(registered == assets.GetAssets().end()
					|| registered->second.importerName != "TerrainImporter")
				{
					outError = "The completed terrain descriptor could not be registered with the project asset database.";
					return false;
				}

				const std::filesystem::path runtimePath = ResourceManager::Instance().GetRegisteredAssetPath(
					registered->second.guid, AssetType::Terrain);
				if(runtimePath.empty())
				{
					outError = "The terrain asset database entry did not publish a runtime terrain path.";
					return false;
				}
				return true;
			});
	}

	TerrainCreationResult TerrainCreationService::CreateOrImport(const TerraEngine::AssetPipeline::TerrainImportRequest& request,
		const std::filesystem::path& destination, TerraEngine::AssetPipeline::TerrainCompileProgressCallback progress) const
	{
		TerrainCreationResult result;
		if(destination.extension() != ".teterrain") { result.message = "Terrain parent assets must use the .teterrain extension."; return result; }
		const auto compiled = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request, std::move(progress));
		result.cancelled = compiled.cancelled;
		result.heightfield = compiled.heightfield;
		if(!compiled.succeeded) { result.message = compiled.message; return result; }

		std::error_code error;
		const std::filesystem::path dataDirectory = std::filesystem::path(destination.string() + ".data");
		if(std::filesystem::exists(destination) || std::filesystem::exists(dataDirectory))
		{
			result.message = "Terrain creation refuses to replace an existing descriptor or private bundle.";
			return result;
		}
		std::filesystem::create_directories(destination.parent_path(), error);
		if(error) { result.message = "The terrain destination directory could not be created."; return result; }
		const std::filesystem::path temporary = destination.string() + ".tmp-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID());
		const std::filesystem::path dataTemporary = dataDirectory.string() + ".tmp-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID());
		std::string stagingError;
		if(!StagePrivateGeneration(request, compiled.heightfield, dataTemporary, stagingError))
		{
			std::filesystem::remove_all(dataTemporary, error);
			result.message = stagingError;
			return result;
		}
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if(!output) { result.message = "The terrain descriptor temporary file could not be opened."; return result; }
			// This intentionally small parent descriptor names only stable import
			// facts. Area blobs remain in the private sibling data directory.
			output << "{\"format\":\"TerraEngineTerrain\",\"rootFormat\":\"TERRAROOT1\",\"version\":1,\"width\":" << compiled.heightfield.width
				<< ",\"height\":" << compiled.heightfield.height << ",\"sampleSpacingMetres\":" << request.sampleSpacingMetres
				<< ",\"assetId\":\"" << TerraEngine::ToHexString(request.assetId) << "\"}";
		}
		std::filesystem::rename(dataTemporary, dataDirectory, error);
		if(error) { std::filesystem::remove(temporary, error); std::filesystem::remove_all(dataTemporary, error); result.message = "The staged private terrain generation could not be atomically published."; return result; }
		std::filesystem::rename(temporary, destination, error);
		if(error) { std::filesystem::remove(temporary, error); std::filesystem::remove_all(dataDirectory, error); result.message = "The completed terrain descriptor could not be atomically published."; return result; }

		std::string registrationError;
		if(registerParentAsset && !registerParentAsset(destination, registrationError))
		{
			std::filesystem::remove_all(dataDirectory, error);
			std::filesystem::remove(destination, error);
			result.message = registrationError.empty() ? "Terrain registration failed; the parent asset was rolled back." : registrationError;
			return result;
		}
		result.succeeded = true;
		result.assetPath = destination;
		result.message = "Terrain parent asset and its private data directory were registered atomically.";
		return result;
	}

	TerrainOpenResult TerrainCreationService::OpenExisting(const std::filesystem::path& source) const
	{
		TerrainOpenResult result;
		nlohmann::json descriptor;
		try
		{
			std::ifstream input(source, std::ios::binary);
			if(!input || !(input >> descriptor) || !descriptor.is_object()
				|| descriptor.value("format", std::string{}) != "TerraEngineTerrain"
				|| descriptor.value("rootFormat", std::string{}) != "TERRAROOT1"
				|| descriptor.value("version", 0u) != 1u)
			{
				result.message = "The terrain descriptor is missing or unsupported.";
				return result;
			}
		}
		catch(const nlohmann::json::exception& exception)
		{
			result.message = std::string("The terrain descriptor is invalid: ") + exception.what();
			return result;
		}
		const auto assetId = descriptor.contains("assetId") && descriptor["assetId"].is_string()
			? TerraEngine::ParseUUID(descriptor["assetId"].get<std::string>()) : std::nullopt;
		const std::uint32_t width = descriptor.value("width", 0u);
		const std::uint32_t height = descriptor.value("height", 0u);
		const double spacing = descriptor.value("sampleSpacingMetres", 1.0);
		if(!assetId || *assetId == TerraEngine::EMPTY_UUID || width == 0u || height == 0u
			|| !std::isfinite(spacing) || spacing <= 0.0)
		{
			result.message = "The terrain descriptor has invalid identity, dimensions, or spacing.";
			return result;
		}

		result.assetId = *assetId;
		result.sampleSpacingMetres = spacing;
		result.heightfield.width = std::min<std::uint32_t>(width, 256u);
		result.heightfield.height = std::min<std::uint32_t>(height, 256u);
		result.heightfield.heightsMm.assign(
			static_cast<std::size_t>(result.heightfield.width) * result.heightfield.height, 0);

		const std::filesystem::path dataDirectory = source.string() + ".data";
		std::ifstream current(dataDirectory / "CURRENT", std::ios::binary);
		std::string rootReference;
		if(!current || !std::getline(current, rootReference) || rootReference.empty())
		{
			result.message = "The terrain private bundle has no readable CURRENT root.";
			return result;
		}
		if(rootReference.size() != 32U)
		{
			result.message = "The terrain CURRENT pointer is not a canonical TERRAROOT1 content hash; recreate or recook the asset.";
			return result;
		}
		std::ifstream rootInput(dataDirectory / "roots" / (rootReference + ".root"), std::ios::binary);
		std::stringstream rootBuffer;
		if(rootInput) rootBuffer << rootInput.rdbuf();
		const auto decodedRoot = TerraEngine::Terrain::DecodeTerrainRootManifest(rootBuffer.str());
		if(!decodedRoot.Succeeded() || !decodedRoot.root
			|| TerraEngine::Terrain::ToHexString(decodedRoot.contentHash) != rootReference
			|| decodedRoot.root->assetUuid != result.assetId
			|| decodedRoot.root->sampleSpacingMetres != spacing)
		{
			result.message = decodedRoot.message.empty() ? "The terrain CURRENT root manifest is invalid." : decodedRoot.message;
			return result;
		}
		const auto rootSnapshot = std::make_shared<const TerraEngine::Terrain::TerrainAssetRoot>(*decodedRoot.root);
		TerraEngine::Terrain::TerrainAssetStore validator(rootSnapshot);
		const auto area = validator.FindDescriptor(*decodedRoot.root, {0, 0});
		if(!area || !area->channels.heightHash)
		{
			result.message = "The terrain CURRENT root has no origin height area.";
			return result;
		}
		const std::filesystem::path blobPath = dataDirectory / "blobs"
			/ (TerraEngine::Terrain::ToHexString(*area->channels.heightHash) + ".tearea");
			std::ifstream blobInput(blobPath, std::ios::binary | std::ios::ate);
			if(!blobInput || blobInput.tellg() <= 0)
			{
				result.message = "The terrain origin height blob is missing.";
				return result;
			}
			const auto byteCount = static_cast<std::size_t>(blobInput.tellg());
			std::vector<std::uint8_t> encoded(byteCount);
			blobInput.seekg(0, std::ios::beg);
			if(!blobInput.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())))
			{
				result.message = "The terrain origin height blob could not be read.";
				return result;
			}
			TerraEngine::Terrain::DecodedTerrainArea decoded;
			const auto decodedResult = TerraEngine::Terrain::DecodeTerrainArea(encoded, decoded);
			const std::size_t expectedBytes = TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t);
			if(!decodedResult.Succeeded() || decoded.header.assetUuid != result.assetId
				|| decoded.header.area != TerraEngine::Terrain::TerrainAreaCoordinate{0, 0}
				|| decoded.canonicalPayload.size() != expectedBytes)
			{
				result.message = decodedResult.Succeeded()
					? "The terrain origin height blob does not match its descriptor."
					: decodedResult.message;
				return result;
			}
			for(std::uint32_t z = 0u; z < result.heightfield.height; ++z)
				std::memcpy(result.heightfield.heightsMm.data() + static_cast<std::size_t>(z) * result.heightfield.width,
					decoded.canonicalPayload.data() + static_cast<std::size_t>(z) * 256u * sizeof(std::int32_t),
					static_cast<std::size_t>(result.heightfield.width) * sizeof(std::int32_t));
		result.succeeded = true;
		result.message = "The persisted terrain origin area was decoded for rendering.";
		return result;
	}
}
