#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cstring>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineTextureFile.h"

namespace
{
	constexpr std::uint32_t kDescChunkId = MakeFourCC("DESC");
	constexpr std::uint32_t kMipChunkId = MakeFourCC('M', 'I', 'P', ' ');

	// Safely translate an absolute file offset into a pointer inside the
	// payload buffer that starts after the chunk table.
	const std::uint8_t* MapPayload(const std::filesystem::path& path, std::size_t payloadBase, std::size_t payloadSize, const std::vector<std::uint8_t>& storage, std::size_t fileOffset, std::size_t requiredSize)
	{
		if (fileOffset < payloadBase)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF offset underflows payload base for " << path);
			return nullptr;
		}

		const std::size_t relativeOffset = fileOffset - payloadBase;
		if (relativeOffset + requiredSize > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF offset overruns payload for " << path);
			return nullptr;
		}

		return storage.data() + relativeOffset;
	}

	// Parse a TETF texture container, gather chunk metadata, and expose
	// spans pointing into owned storage.
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

		if (header.versionMajor != kVersionMajor || header.versionMinor != kVersionMinor)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF version mismatch in " << resolvedPath << " (expected " << kVersionMajor << "." << kVersionMinor << ")");
			return nullptr;
		}

		if (header.headerSize < sizeof(TETFHeader) || header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF headerSize is invalid for " << resolvedPath);
			return nullptr;
		}

		const std::size_t chunkTableSize = static_cast<std::size_t>(header.chunkCount) * sizeof(TETFChunkEntry);
		if (header.tocOffset + chunkTableSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF chunk table overruns file for " << resolvedPath);
			return nullptr;
		}

		auto texture = std::make_unique<Texture>();
		texture->header = header;
		texture->chunkTable.resize(header.chunkCount);

		std::memcpy(texture->chunkTable.data(), fileBytes.data() + static_cast<std::ptrdiff_t>(header.tocOffset), chunkTableSize);

		const std::size_t payloadBase = header.tocOffset + chunkTableSize;
		if (payloadBase > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF payload base overruns file for " << resolvedPath);
			return nullptr;
		}

		texture->storage.assign(fileBytes.begin() + static_cast<std::ptrdiff_t>(payloadBase), fileBytes.end());

		const std::size_t payloadSize = texture->storage.size();
		if (payloadSize == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF payload is empty for " << resolvedPath);
			return nullptr;
		}

		const auto findChunkById = [&](std::uint32_t id) -> const TETFChunkEntry* {
			const auto it = std::find_if(texture->chunkTable.begin(), texture->chunkTable.end(), [id](const TETFChunkEntry& entry) {
				return entry.id == id;
			});
			return it != texture->chunkTable.end() ? &(*it) : nullptr;
		};

		const TETFChunkEntry* descEntry = findChunkById(kDescChunkId);
		if (!descEntry)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF missing DESC chunk in " << resolvedPath);
			return nullptr;
		}

		if (descEntry->size < sizeof(TETFDesc))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF DESC chunk is too small in " << resolvedPath);
			return nullptr;
		}

		const std::uint8_t* descPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, static_cast<std::size_t>(descEntry->offset), static_cast<std::size_t>(descEntry->size));
		if (!descPtr)
		{
			return nullptr;
		}

		std::memcpy(&texture->descriptor, descPtr, sizeof(TETFDesc));

		const auto validateChunkIndex = [&](std::uint32_t index) -> bool {
			return index < texture->chunkTable.size();
		};

		if (texture->descriptor.mipCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF mipCount is zero in " << resolvedPath);
			return nullptr;
		}

		std::vector<const TETFChunkEntry*> mipEntries;
		for (const auto& entry : texture->chunkTable)
		{
			if (entry.id == kMipChunkId)
			{
				mipEntries.push_back(&entry);
			}
		}

		if (mipEntries.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF has no MIP chunks in " << resolvedPath);
			return nullptr;
		}

		texture->mips.reserve(mipEntries.size());

		for (const TETFChunkEntry* mipEntry : mipEntries)
		{
			if (mipEntry->size < sizeof(TETFMipChunkHeader))
			{
				TERRAENGINE_LOG(LogLevel::Error, "TETF MIP chunk is too small in " << resolvedPath);
				return nullptr;
			}

			const std::uint8_t* mipPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, static_cast<std::size_t>(mipEntry->offset), static_cast<std::size_t>(mipEntry->size));
			if (!mipPtr)
			{
				return nullptr;
			}

			TETFMipPayload payload{};
			std::memcpy(&payload.header, mipPtr, sizeof(TETFMipChunkHeader));

			const std::size_t sliceBytes = static_cast<std::size_t>(payload.header.sliceCount) * static_cast<std::size_t>(payload.header.sliceStride);
			const std::size_t availableBytes = mipEntry->size - sizeof(TETFMipChunkHeader);
			if (sliceBytes > availableBytes)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TETF MIP payload underruns slice data in " << resolvedPath);
				return nullptr;
			}

			payload.data = std::span<const std::uint8_t>(mipPtr + sizeof(TETFMipChunkHeader), sliceBytes);
			texture->mips.push_back(payload);
		}

		std::sort(texture->mips.begin(), texture->mips.end(), [](const TETFMipPayload& a, const TETFMipPayload& b) {
			return a.header.mipLevel < b.header.mipLevel;
		});

		if (texture->mips.size() != texture->descriptor.mipCount)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "TETF mip count mismatch in " << resolvedPath << " (descriptor " << static_cast<std::uint32_t>(texture->descriptor.mipCount) << ", chunks " << texture->mips.size() << ")");
		}

		if (texture->descriptor.userMetadataChunk != TETF_INVALID_CHUNK_INDEX && validateChunkIndex(texture->descriptor.userMetadataChunk))
		{
			const TETFChunkEntry& metaEntry = texture->chunkTable[texture->descriptor.userMetadataChunk];
			const std::uint8_t* metaPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, static_cast<std::size_t>(metaEntry.offset), static_cast<std::size_t>(metaEntry.size));
			if (metaPtr)
			{
				texture->userMetadata = std::span<const std::uint8_t>(metaPtr, static_cast<std::size_t>(metaEntry.size));
			}
		}

		if (texture->descriptor.thumbnailChunk != TETF_INVALID_CHUNK_INDEX && validateChunkIndex(texture->descriptor.thumbnailChunk))
		{
			const TETFChunkEntry& thumbEntry = texture->chunkTable[texture->descriptor.thumbnailChunk];
			const std::uint8_t* thumbPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, static_cast<std::size_t>(thumbEntry.offset), static_cast<std::size_t>(thumbEntry.size));
			if (thumbPtr)
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
