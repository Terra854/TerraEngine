#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "basisu_transcoder.h"
#include "basisu_transcoder_uastc.h"

namespace
{
	constexpr std::uint32_t kDescChunkId = MakeFourCC("DESC");
	constexpr std::uint32_t kUastChunkId = MakeFourCC("UAST");

	// Transcode raw UASTC blocks to hardware formats using the low-level API
	bool TranscodeUastc(Texture& texture, const std::filesystem::path& path)
	{
		if (texture.uastcPayload.empty()) return true;

		auto startTime = std::chrono::high_resolution_clock::now();

		// Map the suggested VkFormat to a simplified target
		enum class InternalFormat { BC7, ASTC, RGBA8 };
		InternalFormat target = InternalFormat::BC7;
		
		const uint32_t vkFormat = texture.descriptor.vkFormat;
		const bool isSRGB = (texture.header.flags & 1) != 0;

		// Corrected mapping based on standard VkFormat enum:
		// 145/146: BC7, 157/158: ASTC, 37/43: RGBA8
		if (vkFormat == 157 || vkFormat == 158) {
			target = InternalFormat::ASTC;
			texture.descriptor.vkFormat = isSRGB ? 158 : 157;
		} else if (vkFormat == 37 || vkFormat == 43) {
			target = InternalFormat::RGBA8;
			texture.descriptor.vkFormat = isSRGB ? 43 : 37;
		} else {
			target = InternalFormat::BC7;
			texture.descriptor.vkFormat = isSRGB ? 146 : 145;
		}

		const size_t bytesPerBlock = (target == InternalFormat::RGBA8) ? 64 : 16;
		
		// Update descriptor to reflect transcoded block state for the RHI
		texture.descriptor.blockWidth = (target == InternalFormat::RGBA8) ? 1 : 4;
		texture.descriptor.blockHeight = (target == InternalFormat::RGBA8) ? 1 : 4;
		texture.descriptor.blockBytes = static_cast<uint16_t>(bytesPerBlock);

		size_t totalTranscodedSize = 0;
		std::vector<size_t> levelSizes;
		
		for (uint32_t i = 0; i < texture.descriptor.mipCount; ++i)
		{
			uint32_t levelWidth = std::max(1u, (uint32_t)texture.descriptor.width >> i);
			uint32_t levelHeight = std::max(1u, (uint32_t)texture.descriptor.height >> i);
			uint32_t blocksX = (levelWidth + 3) / 4;
			uint32_t blocksY = (levelHeight + 3) / 4;
			size_t levelSize = blocksX * blocksY * bytesPerBlock;
			levelSizes.push_back(levelSize);
			totalTranscodedSize += levelSize;
		}

		texture.transcodedData.resize(totalTranscodedSize);
		std::vector<TETFLevelEntry> newLevelTable;
		size_t currentOffset = 0;

		const basist::uastc_block* srcBlocks = reinterpret_cast<const basist::uastc_block*>(texture.uastcPayload.data());
		const size_t totalBlocksInPayload = texture.uastcPayload.size() / 16;
		size_t blocksRead = 0;

		for (uint32_t i = 0; i < texture.descriptor.mipCount; ++i)
		{
			uint32_t levelWidth = std::max(1u, (uint32_t)texture.descriptor.width >> i);
			uint32_t levelHeight = std::max(1u, (uint32_t)texture.descriptor.height >> i);
			uint32_t blocksX = (levelWidth + 3) / 4;
			uint32_t blocksY = (levelHeight + 3) / 4;
			uint32_t numBlocks = blocksX * blocksY;

			if (blocksRead + numBlocks > totalBlocksInPayload)
			{
				TERRAENGINE_LOG(LogLevel::Error, "UASTC payload underflow in " << path.filename() << " level " << i);
				return false;
			}

			uint8_t* dstData = texture.transcodedData.data() + currentOffset;

			for (uint32_t b = 0; b < numBlocks; ++b)
			{
				const basist::uastc_block& srcBlock = srcBlocks[blocksRead + b];
				uint8_t* dstBlock = dstData + b * bytesPerBlock;

				if (target == InternalFormat::BC7)
				{
					basist::transcode_uastc_to_bc7(srcBlock, dstBlock);
				}
				else if (target == InternalFormat::ASTC)
				{
					basist::transcode_uastc_to_astc(srcBlock, dstBlock);
				}
				else
				{
					basist::unpack_uastc(srcBlock, reinterpret_cast<basist::color32*>(dstBlock), false);
				}
			}

			newLevelTable.push_back({ (uint64_t)currentOffset, (uint64_t)levelSizes[i] });
			currentOffset += levelSizes[i];
			blocksRead += numBlocks;
		}

		auto endTime = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
		TERRAENGINE_LOG(LogLevel::Info, "Transcoded " << path.filename() << " (" << texture.descriptor.width << "x" << texture.descriptor.height << ") to VK format " << texture.descriptor.vkFormat << " in " << (duration / 1000.0) << " ms");

		texture.levelTable = std::move(newLevelTable);
		return true;
	}

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

		if (header.version != kVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF version mismatch in " << resolvedPath << " (expected " << kVersion << ")");
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

		// Load Level Table
		if (texture->descriptor.mipCount > 0)
		{
			const std::size_t levelTableSize = static_cast<std::size_t>(texture->descriptor.mipCount) * sizeof(TETFLevelEntry);
			const std::size_t expectedDescSize = sizeof(TETFDesc) + levelTableSize;
			
			if (descEntry->size < expectedDescSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TETF DESC chunk truncated levelTable in " << resolvedPath);
				return nullptr;
			}

			texture->levelTable.resize(texture->descriptor.mipCount);
			std::memcpy(texture->levelTable.data(), descPtr + sizeof(TETFDesc), levelTableSize);
		}

		const auto validateChunkIndex = [&](std::uint32_t index) -> bool {
			return index < texture->chunkTable.size();
		};

		// Map UASTC Payload
		const TETFChunkEntry* uastEntry = findChunkById(kUastChunkId);
		if (uastEntry)
		{
			const std::uint8_t* uastPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, static_cast<std::size_t>(uastEntry->offset), static_cast<std::size_t>(uastEntry->size));
			if (uastPtr)
			{
				texture->uastcPayload = std::span<const std::uint8_t>(uastPtr, static_cast<std::size_t>(uastEntry->size));
				if (!TranscodeUastc(*texture, resolvedPath))
				{
					return nullptr;
				}
			}
		}
		else
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF missing UAST chunk in " << resolvedPath);
			return nullptr;
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

	bool IsCompiledTexturePath(const std::filesystem::path& path)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return extension == ".tetf";
	}
}

TerraEngine::UUID ResourceManager::LoadTexture(const std::filesystem::path& path)
{
	if (!IsCompiledTexturePath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping source texture load until compiled TETF is available: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	return LoadOrGetCached<Texture>(path, textureCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadTextureFile(*this, resolvedPath);
	});
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadTextureAsync(const std::filesystem::path& path)
{
	if (!IsCompiledTexturePath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping async source texture load until compiled TETF is available: " << path);
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	return LoadOrSchedule<Texture>(path, textureCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadTextureFile(*this, resolvedPath);
	});
}

std::shared_future<void> ResourceManager::ReloadTexture(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(textureCache.mutex);
		if (const auto pathIt = textureCache.idToPath.find(id); pathIt != textureCache.idToPath.end())
		{
			key = pathIt->second;
		}
	}

	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Cannot reload texture yet: UUID " << ToHexString(id) << " has no compiled TETF path registered.");
		std::promise<void> p;
		p.set_value();
		return p.get_future().share();
	}

	const std::filesystem::path resolvedPath(key);
	if (!IsCompiledTexturePath(resolvedPath))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping texture reload from non-TETF path: " << resolvedPath);
		std::promise<void> p;
		p.set_value();
		return p.get_future().share();
	}

	TERRAENGINE_LOG(LogLevel::Debug, "Reloading texture from " << resolvedPath);
	
	auto task = threadPool.createTask([this, key, resolvedPath, id]() {
		auto texture = LoadTextureFile(*this, resolvedPath);
		if (texture)
		{
			std::unique_lock lock(textureCache.mutex);
			textureCache.resources[id] = std::move(texture);
		}
	});

	auto future = task->getFuture().share();
	threadPool.enqueue(task);
	return future;
}
