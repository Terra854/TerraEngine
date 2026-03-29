#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <ktx.h>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineTextureFile.h"

namespace
{
	struct KtxTextureDestroy {
		void operator()(ktxTexture2* texture) const
		{
			if (texture != nullptr)
			{
				ktxTexture2_Destroy(texture);
			}
		}
	};

	using UniqueKtxTexture2 = std::unique_ptr<ktxTexture2, KtxTextureDestroy>;

	// Translate an absolute byte range inside the wrapped .tetf file into a
	// stable pointer while checking for truncated or corrupt chunk metadata.
	const std::uint8_t* MapFileRange(
		const std::filesystem::path& path,
		const std::vector<std::uint8_t>& fileBytes,
		const std::size_t fileOffset,
		const std::size_t requiredSize)
	{
		if (fileOffset > fileBytes.size() || requiredSize > fileBytes.size() - fileOffset)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF chunk range overruns file for " << path);
			return nullptr;
		}

		return fileBytes.data() + fileOffset;
	}

	const TETFChunkEntry* FindChunkById(const std::vector<TETFChunkEntry>& chunkTable, std::uint32_t id)
	{
		const auto it = std::find_if(chunkTable.begin(), chunkTable.end(), [id](const TETFChunkEntry& entry) {
			return entry.id == id;
		});
		return it != chunkTable.end() ? &(*it) : nullptr;
	}

	bool ValidateCurrentDescriptorAgainstPayload(
		const std::filesystem::path& resolvedPath,
		const TETFDesc& descriptor,
		const ktxTexture2& payload)
	{
		const bool dimensionMismatch =
			descriptor.width != payload.baseWidth ||
			descriptor.height != payload.baseHeight ||
			descriptor.depth != payload.baseDepth ||
			descriptor.layers != payload.numLayers ||
			descriptor.mipCount != payload.numLevels ||
			descriptor.faces != payload.numFaces;

		if (dimensionMismatch)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF descriptor does not match embedded KTX2 payload dimensions in " << resolvedPath);
			return false;
		}

		if (descriptor.vkFormat != payload.vkFormat)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF descriptor vkFormat does not match embedded KTX2 payload in " << resolvedPath);
			return false;
		}

		return true;
	}

	bool ExtractCurrentKtx2Payload(Texture& texture, const std::filesystem::path& resolvedPath)
	{
		ktxTexture2* rawPayload = nullptr;
		const KTX_error_code openResult = ktxTexture2_CreateFromMemory(
			texture.ktx2Payload.data(),
			texture.ktx2Payload.size(),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
			&rawPayload);
		if (openResult != KTX_SUCCESS || rawPayload == nullptr)
		{
			TERRAENGINE_LOG(
				LogLevel::Error,
				"Embedded KTX2 payload failed to open in " << resolvedPath
				<< ": " << ktxErrorString(openResult));
			return false;
		}

		UniqueKtxTexture2 payload(rawPayload);

		// The compiler currently produces 2D textures only. Rejecting unexpected
		// shapes here keeps invalid current files from slipping into the renderer.
		if (payload->numDimensions != 2 || payload->baseDepth != 1 || payload->numLayers != 1 || payload->numFaces != 1)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Embedded KTX2 payload uses an unsupported texture shape in " << resolvedPath);
			return false;
		}

		if (!ValidateCurrentDescriptorAgainstPayload(resolvedPath, texture.descriptor, *payload))
		{
			return false;
		}

		texture.supercompressionScheme = static_cast<std::uint32_t>(payload->supercompressionScheme);
		texture.imageData.assign(payload->pData, payload->pData + payload->dataSize);
		texture.mips.clear();
		texture.mips.reserve(payload->numLevels);

		for (ktx_uint32_t level = 0; level < payload->numLevels; ++level)
		{
			ktx_size_t imageOffset = 0;
			const KTX_error_code offsetResult = ktxTexture_GetImageOffset(ktxTexture(payload.get()), level, 0, 0, &imageOffset);
			if (offsetResult != KTX_SUCCESS)
			{
				TERRAENGINE_LOG(
					LogLevel::Error,
					"Failed to locate mip " << level << " inside embedded KTX2 payload for "
					<< resolvedPath << ": " << ktxErrorString(offsetResult));
				return false;
			}

			const std::size_t imageSize = ktxTexture_GetImageSize(ktxTexture(payload.get()), level);
			if (imageOffset > texture.imageData.size() || imageSize > texture.imageData.size() - imageOffset)
			{
				TERRAENGINE_LOG(LogLevel::Error, "Embedded KTX2 mip data overruns decoded payload for " << resolvedPath);
				return false;
			}

			TETFMipPayload mip{};
			mip.mipLevel = static_cast<std::uint8_t>(level);
			mip.sliceCount = 1;
			mip.sliceStride = static_cast<std::uint32_t>(imageSize);
			mip.width = std::max(1u, payload->baseWidth >> level);
			mip.height = std::max(1u, payload->baseHeight >> level);
			mip.depth = 1;
			mip.data = std::span<const std::uint8_t>(texture.imageData.data() + imageOffset, imageSize);
			texture.mips.push_back(mip);
		}

		return true;
	}

	// Parse a current .tetf texture container, validate that it advertises the
	// required KTX2 payload contract, and expose decoded mip spans for the
	// renderer. Any legacy container is rejected before payload parsing begins.
	std::unique_ptr<Texture> LoadTextureFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		if (fileBytes.size() < sizeof(TETFHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF file is too small: " << resolvedPath);
			return nullptr;
		}

		TETFHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(TETFHeader));

		if (header.magic != kMagic)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF magic mismatch in " << resolvedPath);
			return nullptr;
		}

		if (!IsCurrentTetfVersion(header.versionMajor))
		{
			if (header.versionMajor < kVersionMajor)
			{
				TERRAENGINE_LOG(
					LogLevel::Error,
					"Legacy TETF payload rejected in " << resolvedPath
					<< ". Recompile the source texture to generate a current .tetf with embedded KTX2.");
			}
			else
			{
				TERRAENGINE_LOG(
					LogLevel::Error,
					"TETF version mismatch in " << resolvedPath
					<< " (expected " << kVersionMajor << ")");
			}
			return nullptr;
		}

		if (header.headerSize < sizeof(TETFHeader) || header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF headerSize is invalid for " << resolvedPath);
			return nullptr;
		}

		const std::size_t chunkTableSize = static_cast<std::size_t>(header.chunkCount) * sizeof(TETFChunkEntry);
		if (header.tocOffset > fileBytes.size() || chunkTableSize > fileBytes.size() - header.tocOffset)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF chunk table overruns file for " << resolvedPath);
			return nullptr;
		}

		if (header.totalSize != 0 && header.totalSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF totalSize is invalid for " << resolvedPath);
			return nullptr;
		}

		auto texture = std::make_unique<Texture>();
		texture->header = header;
		texture->chunkTable.resize(header.chunkCount);
		texture->containerBytes = fileBytes;
		std::memcpy(
			texture->chunkTable.data(),
			texture->containerBytes.data() + static_cast<std::ptrdiff_t>(header.tocOffset),
			chunkTableSize);

		const TETFChunkEntry* descEntry = FindChunkById(texture->chunkTable, kDescChunkId);
		if (descEntry == nullptr)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF is missing DESC chunk in " << resolvedPath);
			return nullptr;
		}

		if (descEntry->size < sizeof(TETFDesc))
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF DESC chunk is too small in " << resolvedPath);
			return nullptr;
		}

		const std::uint8_t* descPtr = MapFileRange(
			resolvedPath,
			texture->containerBytes,
			static_cast<std::size_t>(descEntry->offset),
			sizeof(TETFDesc));
		if (descPtr == nullptr)
		{
			return nullptr;
		}

		std::memcpy(&texture->descriptor, descPtr, sizeof(TETFDesc));
		if (texture->descriptor.mipCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF mipCount is zero in " << resolvedPath);
			return nullptr;
		}

		if (texture->descriptor.payloadChunk >= texture->chunkTable.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF payload chunk index is invalid in " << resolvedPath);
			return nullptr;
		}

		const TETFChunkEntry& payloadEntry = texture->chunkTable[texture->descriptor.payloadChunk];
		if (payloadEntry.id != kKtx2ChunkId)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Current TETF payload chunk is not KTX2 in " << resolvedPath);
			return nullptr;
		}

		const std::uint8_t* payloadPtr = MapFileRange(
			resolvedPath,
			texture->containerBytes,
			static_cast<std::size_t>(payloadEntry.offset),
			static_cast<std::size_t>(payloadEntry.size));
		if (payloadPtr == nullptr)
		{
			return nullptr;
		}

		texture->ktx2Payload = std::span<const std::uint8_t>(payloadPtr, static_cast<std::size_t>(payloadEntry.size));
		if (!ExtractCurrentKtx2Payload(*texture, resolvedPath))
		{
			return nullptr;
		}

		const auto validateChunkIndex = [&](std::uint32_t index) -> bool {
			return index < texture->chunkTable.size();
		};

		if (texture->descriptor.userMetadataChunk != TETF_INVALID_CHUNK_INDEX && validateChunkIndex(texture->descriptor.userMetadataChunk))
		{
			const TETFChunkEntry& metaEntry = texture->chunkTable[texture->descriptor.userMetadataChunk];
			const std::uint8_t* metaPtr = MapFileRange(
				resolvedPath,
				texture->containerBytes,
				static_cast<std::size_t>(metaEntry.offset),
				static_cast<std::size_t>(metaEntry.size));
			if (metaPtr != nullptr)
			{
				texture->userMetadata = std::span<const std::uint8_t>(metaPtr, static_cast<std::size_t>(metaEntry.size));
			}
		}

		if (texture->descriptor.thumbnailChunk != TETF_INVALID_CHUNK_INDEX && validateChunkIndex(texture->descriptor.thumbnailChunk))
		{
			const TETFChunkEntry& thumbEntry = texture->chunkTable[texture->descriptor.thumbnailChunk];
			const std::uint8_t* thumbPtr = MapFileRange(
				resolvedPath,
				texture->containerBytes,
				static_cast<std::size_t>(thumbEntry.offset),
				static_cast<std::size_t>(thumbEntry.size));
			if (thumbPtr != nullptr)
			{
				texture->thumbnail = std::span<const std::uint8_t>(thumbPtr, static_cast<std::size_t>(thumbEntry.size));
			}
		}

		return texture;
	}
}

TerraEngine::UUID ResourceManager::LoadTexture(const std::filesystem::path& path)
{
	return LoadOrGetCached<Texture>(path, textureCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadTextureFile(*this, resolvedPath);
	});
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadTextureAsync(const std::filesystem::path& path)
{
	return LoadOrSchedule<Texture>(path, textureCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadTextureFile(*this, resolvedPath);
	});
}

void ResourceManager::ReloadTexture(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(textureCache.mutex);
		for (const auto& [path, uuid] : textureCache.pathToId)
		{
			if (uuid == id)
			{
				key = path;
				break;
			}
		}
	}

	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Cannot reload texture: UUID " << ToHexString(id) << " not found in cache path map.");
		return;
	}

	const std::filesystem::path resolvedPath(key);
	TERRAENGINE_LOG(LogLevel::Debug, "Reloading texture from " << resolvedPath);
	auto texture = LoadTextureFile(*this, resolvedPath);
	if (texture)
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.resources[id] = std::move(texture);
	}
}
