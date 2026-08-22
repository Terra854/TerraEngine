// TerrainCreationService.cpp -- Atomic parent terrain asset creation.

#include "TerrainCreationService.h"

#include "Editor/TerraEngine-Editor/Assets/AssetDatabase.h"
#include "Terrain/TerrainAreaCodec.h"
#include "File Formats/TerraEngineTerrainFile.h"
#include "Resource/ResourceManager.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
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
			std::vector<std::string> manifestAreas;
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
				manifestAreas.push_back("{\"x\":" + std::to_string(areaX) + ",\"z\":" + std::to_string(areaZ) + ",\"heightHash\":\"" + areaHash + "\"}");
			}
			const std::filesystem::path rootManifest = stagingDirectory / "roots" / "1.json";
			std::filesystem::create_directories(rootManifest.parent_path(), error);
			if(error) { outError = "The staged terrain root directory could not be created."; return false; }
			std::ofstream rootOutput(rootManifest, std::ios::binary | std::ios::trunc);
			rootOutput << "{\"generation\":1,\"assetId\":\"" << TerraEngine::ToHexString(request.assetId)
				<< "\",\"width\":" << heightfield.width << ",\"height\":" << heightfield.height << ",\"edgePolicy\":\"repeat-nearest\",\"areas\":[";
			for(std::size_t index = 0U; index < manifestAreas.size(); ++index) { if(index != 0U) rootOutput << ','; rootOutput << manifestAreas[index]; }
			rootOutput << "]}";
			rootOutput.close();
			if(!rootOutput) { outError = "The staged terrain root manifest could not be written."; return false; }
			std::ofstream currentOutput(stagingDirectory / "CURRENT", std::ios::binary | std::ios::trunc);
			currentOutput << "roots/1.json\n"; currentOutput.close();
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
		std::filesystem::create_directories(destination.parent_path(), error);
		if(error) { result.message = "The terrain destination directory could not be created."; return result; }
		const std::filesystem::path dataDirectory = std::filesystem::path(destination.string() + ".data");
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
			output << "{\"format\":\"TerraEngineTerrain\",\"version\":1,\"width\":" << compiled.heightfield.width
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
		const float spacing = descriptor.value("sampleSpacingMetres", 1.0F);
		if(!assetId || *assetId == TerraEngine::EMPTY_UUID || width == 0u || height == 0u
			|| !std::isfinite(spacing) || spacing <= 0.0F)
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
		// Hash-named immutable roots are rehydrated by TerrainAssetStore after the
		// window creates its initial authority. A zero-filled seed is safe because
		// it is replaced before proxy generation is published.
		if(!rootReference.starts_with("roots/"))
		{
			result.succeeded = true;
			result.message = "The immutable terrain root is ready for store recovery.";
			return result;
		}

		try
		{
			nlohmann::json root;
			std::ifstream rootInput(dataDirectory / rootReference, std::ios::binary);
			if(!rootInput || !(rootInput >> root) || !root.contains("areas") || !root["areas"].is_array())
			{
				result.message = "The terrain CURRENT root manifest is invalid.";
				return result;
			}
			const auto area = std::find_if(root["areas"].begin(), root["areas"].end(), [](const nlohmann::json& candidate) {
				return candidate.is_object() && candidate.value("x", 1) == 0 && candidate.value("z", 1) == 0
					&& candidate.contains("heightHash") && candidate["heightHash"].is_string();
			});
			if(area == root["areas"].end())
			{
				result.message = "The terrain CURRENT root has no origin height area.";
				return result;
			}
			const std::filesystem::path blobPath = dataDirectory / "blobs"
				/ ((*area)["heightHash"].get<std::string>() + ".tearea");
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
		}
		catch(const nlohmann::json::exception& exception)
		{
			result.message = std::string("The terrain CURRENT root is invalid: ") + exception.what();
			return result;
		}
		result.succeeded = true;
		result.message = "The persisted terrain origin area was decoded for rendering.";
		return result;
	}
}
