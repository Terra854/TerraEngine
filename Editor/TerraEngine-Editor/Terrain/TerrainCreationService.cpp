// TerrainCreationService.cpp -- Atomic parent terrain asset creation.

#include "TerrainCreationService.h"

#include "Editor/TerraEngine-Editor/Assets/AssetDatabase.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"
#include "File Formats/TerraEngineTerrainFile.h"
#include "Resource/ResourceManager.h"
#include "Utilities/UUID.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
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
			// Flat creation is represented by the root's stable implicit defaults.
			// Imported heightmaps still materialize every source-covered area.
			if(request.sourceFormat != TerraEngine::AssetPipeline::TerrainSourceFormat::Flat)
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
			if(!shard.descriptors.empty())
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
			root.defaultHeightMm = heightfield.heightsMm.front();
			root.defaultLayerId = 1U;
			root.layerStackGeneration = 1U;
			root.layerStack = std::make_shared<const TerraEngine::Terrain::TerrainLayerStack>(std::move(layerStack));
			if(!shard.descriptors.empty()) root.directoryShards = {std::move(shard)};
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

	std::future<TerrainCreationResult> TerrainCreationService::CreateOrImportAsync(
		TerraEngine::AssetPipeline::TerrainImportRequest request,
		std::filesystem::path destination,
		std::shared_ptr<std::atomic_bool> cancellation,
		TerraEngine::AssetPipeline::TerrainCompileProgressCallback progress) const
	{
		const TerrainCreationService service = *this;
		const std::uint64_t sampleCount = request.sourceFormat == TerraEngine::AssetPipeline::TerrainSourceFormat::Flat
			? static_cast<std::uint64_t>(request.flatWidth) * request.flatDepth
			: static_cast<std::uint64_t>(request.rawWidth) * request.rawHeight;
		auto work = [service, request = std::move(request), destination = std::move(destination),
			cancellation = std::move(cancellation), progress = std::move(progress)]() mutable {
			return service.CreateOrImport(request, destination,
				[cancellation, progress](float fraction, std::string_view stage, std::string_view message) {
					if(cancellation && cancellation->load(std::memory_order_acquire)) return false;
					return !progress || progress(fraction, stage, message);
				});
		};
		auto task = ThreadPool::instance().createTask(std::move(work));
		std::future<TerrainCreationResult> result = task->getFuture();
		ThreadPool::instance().enqueue(task, std::max<std::size_t>(1u,
			static_cast<std::size_t>(std::min<std::uint64_t>(sampleCount,
				(std::numeric_limits<std::size_t>::max)()))));
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
		const std::uint32_t compatibilityWidth = descriptor.value("width", 0u);
		const std::uint32_t compatibilityHeight = descriptor.value("height", 0u);
		const double spacing = descriptor.value("sampleSpacingMetres", 1.0);
		if(!assetId || *assetId == TerraEngine::EMPTY_UUID || compatibilityWidth == 0u || compatibilityHeight == 0u
			|| !std::isfinite(spacing) || spacing <= 0.0)
		{
			result.message = "The terrain descriptor has invalid identity, dimensions, or spacing.";
			return result;
		}

		result.assetId = *assetId;
		result.sampleSpacingMetres = spacing;
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
		result.store = std::make_shared<TerraEngine::Terrain::TerrainAssetStore>(rootSnapshot);
		const auto opened = result.store->OpenPersistedRoot(dataDirectory);
		if (!opened.Succeeded())
		{
			result.store.reset();
			result.message = opened.message;
			return result;
		}
		const auto capture = result.store->CaptureRoot();
		if (!capture.IsValid() || capture.root->assetUuid != result.assetId)
		{
			result.store.reset();
			result.message = "The persisted terrain root changed identity while opening.";
			return result;
		}
		result.authoredBounds = capture.root->authoredBounds;
		const long double widthValue = static_cast<long double>(result.authoredBounds.maxXExclusive)
			- static_cast<long double>(result.authoredBounds.minX);
		const long double depthValue = static_cast<long double>(result.authoredBounds.maxZExclusive)
			- static_cast<long double>(result.authoredBounds.minZ);
		if (widthValue > (std::numeric_limits<std::uint32_t>::max)()
			|| depthValue > (std::numeric_limits<std::uint32_t>::max)()
			|| widthValue <= 0.0L || depthValue <= 0.0L)
		{
			result.store.reset();
			result.message = "The authored terrain extent is too large for the compatibility editor heightfield.";
			return result;
		}
		const auto width64 = static_cast<std::uint64_t>(widthValue);
		const auto depth64 = static_cast<std::uint64_t>(depthValue);
		if (width64 > (std::numeric_limits<std::uint32_t>::max)()
			|| depth64 > (std::numeric_limits<std::uint32_t>::max)()
			|| (width64 != 0u && depth64 > (std::numeric_limits<std::size_t>::max)() / width64))
		{
			result.store.reset();
			result.message = "The authored terrain extent is too large for the compatibility editor heightfield.";
			return result;
		}
		const auto areaBounds = TerraEngine::Terrain::TerrainAreasForSamples(result.authoredBounds);
		const auto areaCount = areaBounds ? areaBounds->AreaCount() : std::nullopt;
		constexpr std::size_t kMaximumCompatibilityAreas = 64u;
		if(!areaCount)
		{
			result.store.reset();
			result.message = "The authored terrain area range overflows the opening budget.";
			return result;
		}
		if(*areaCount > kMaximumCompatibilityAreas)
		{
			// The store-oriented result remains usable for streaming consumers. Avoid
			// allocating one monolithic compatibility heightfield for large terrain.
			result.succeeded = true;
			result.message = "The terrain root opened; detail will stream through its bounded area cache.";
			return result;
		}
		result.heightfield.width = static_cast<std::uint32_t>(width64);
		result.heightfield.height = static_cast<std::uint32_t>(depth64);
		result.heightfield.heightsMm.assign(static_cast<std::size_t>(width64 * depth64), capture.root->defaultHeightMm);
		const auto areas = TerraEngine::Terrain::EnumerateTerrainAreas(result.authoredBounds,
			kMaximumCompatibilityAreas);
		if (!areas)
		{
			result.store.reset();
			result.message = "The authored terrain area range exceeds the opening budget.";
			return result;
		}
		for (const auto coordinate : *areas)
		{
			const auto descriptor = result.store->ResolveDescriptorOrImplicit(*capture.root, coordinate);
			result.areaSnapshots.emplace(coordinate, descriptor);
			if (!descriptor.channels.heightHash) continue;
			const auto loaded = result.store->LoadHeightAreaLazy(capture, coordinate);
			const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(result.authoredBounds, coordinate);
			if (!loaded.Succeeded() || !active)
			{
				result.store.reset();
				result.message = loaded.message.empty() ? "A persisted terrain area could not be decoded." : loaded.message;
				return result;
			}
			const std::int64_t areaOriginX = coordinate.x * TerraEngine::Terrain::kTerrainAreaExtentSamples;
			const std::int64_t areaOriginZ = coordinate.z * TerraEngine::Terrain::kTerrainAreaExtentSamples;
			for (std::uint16_t localZ = active->minZ; localZ < active->maxZExclusive; ++localZ)
				for (std::uint16_t localX = active->minX; localX < active->maxXExclusive; ++localX)
				{
					const auto destinationX = static_cast<std::size_t>(areaOriginX + localX - result.authoredBounds.minX);
					const auto destinationZ = static_cast<std::size_t>(areaOriginZ + localZ - result.authoredBounds.minZ);
					std::int32_t height = 0;
					const std::size_t sourceIndex = static_cast<std::size_t>(localZ) * 256u + localX;
					std::memcpy(&height, loaded.decodedHeight->canonicalPayload.data() + sourceIndex * sizeof(height), sizeof(height));
					result.heightfield.heightsMm[destinationZ * result.heightfield.width + destinationX] = height;
				}
		}
		result.succeeded = true;
		result.compatibilityHeightfieldComplete = true;
		result.message = "The complete root-authoritative terrain extent was opened for authoring.";
		return result;
	}

	bool TerrainCreationService::SynchronizeCompatibilityDescriptor(
		const std::filesystem::path& descriptorPath,
		const TerraEngine::Terrain::TerrainAssetRoot& committedRoot,
		std::string& outError)
	{
		const auto dimensions = TerraEngine::AssetPipeline::TerrainCompatibilityDimensions(
			committedRoot.authoredBounds);
		if(!dimensions)
		{
			outError = "The committed terrain bounds cannot be represented by compatibility dimensions.";
			return false;
		}
		nlohmann::json descriptor;
		try
		{
			std::ifstream input(descriptorPath, std::ios::binary);
			if(!input || !(input >> descriptor) || !descriptor.is_object())
			{
				outError = "The terrain parent descriptor could not be decoded before save.";
				return false;
			}
		}
		catch(const nlohmann::json::exception& exception)
		{
			outError = std::string("The terrain parent descriptor is invalid: ") + exception.what();
			return false;
		}
		descriptor["width"] = dimensions->first;
		descriptor["height"] = dimensions->second;
		descriptor["sampleSpacingMetres"] = committedRoot.sampleSpacingMetres;
		descriptor["assetId"] = TerraEngine::ToHexString(committedRoot.assetUuid);
		const std::filesystem::path staged = descriptorPath.string() + ".sync-"
			+ TerraEngine::ToHexString(TerraEngine::GenerateUUID());
		const std::filesystem::path backup = staged.string() + ".previous";
		{
			std::ofstream output(staged, std::ios::binary | std::ios::trunc);
			if(!output || !(output << descriptor.dump()))
			{
				outError = "The synchronized terrain descriptor could not be staged.";
				return false;
			}
			output.flush();
			if(!output)
			{
				outError = "The synchronized terrain descriptor could not be flushed.";
				return false;
			}
		}
		std::error_code error;
		std::filesystem::rename(descriptorPath, backup, error);
		if(error)
		{
			std::filesystem::remove(staged, error);
			outError = "The previous terrain descriptor could not be retained for atomic replacement.";
			return false;
		}
		std::filesystem::rename(staged, descriptorPath, error);
		if(error)
		{
			std::error_code restoreError;
			std::filesystem::rename(backup, descriptorPath, restoreError);
			std::filesystem::remove(staged, restoreError);
			outError = "The synchronized terrain descriptor could not be atomically published.";
			return false;
		}
		std::filesystem::remove(backup, error);
		return true;
	}
}
