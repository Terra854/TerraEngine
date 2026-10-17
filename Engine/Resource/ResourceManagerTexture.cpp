// ResourceManagerTexture.cpp — Texture Resource Management Implementation
//
// Implements the specialized logic for loading, caching, and managing texture resources.
// It handles the parsing of engine-specific texture formats and coordinates asynchronous transcoding of compressed image data.
// This module ensures that textures are correctly prepared for sampling by the rendering backend.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "Runtime/ArtifactValidation.h"
#include <bit>
#include <array>
#include <limits>
#include "basisu_transcoder.h"
#include "basisu_transcoder_uastc.h"

namespace
{
	constexpr std::uint32_t kDescChunkId = MakeFourCC("DESC");
	constexpr std::uint32_t kUastChunkId = MakeFourCC("UAST");

	// Transcode raw UASTC blocks to hardware formats using the low-level API

	bool TranscodeUastc(Texture& texture, const std::filesystem::path& path)
	{
		if(texture.uastcPayload.empty()) return false;
		const auto startTime=std::chrono::high_resolution_clock::now();
		enum class InternalFormat { BC7, ASTC, RGBA8 };
		const auto requested=texture.descriptor.vkFormat; const bool srgb=(texture.header.flags&1)!=0;
		const auto target=requested==157 || requested==158 ? InternalFormat::ASTC : requested==37 || requested==43 ? InternalFormat::RGBA8 : InternalFormat::BC7;
		texture.descriptor.vkFormat=target==InternalFormat::RGBA8 ? (srgb?43:37) : target==InternalFormat::ASTC ? (srgb?158:157) : (srgb?146:145);
		texture.descriptor.blockWidth=target==InternalFormat::RGBA8?1:4; texture.descriptor.blockHeight=target==InternalFormat::RGBA8?1:4; texture.descriptor.blockBytes=target==InternalFormat::RGBA8?4:16;
		struct Level { unsigned width,height,blocksX,blocksY; std::size_t blocks,bytes; };
		std::vector<Level> levels; levels.reserve(texture.descriptor.mipCount);
		std::size_t totalBytes=0,totalBlocks=0; const auto availableBlocks=texture.uastcPayload.size()/16;
		for(unsigned mip=0;mip<texture.descriptor.mipCount;++mip) {
			// Admission has already proven positive dimensions and a legal shift
			// domain. Widen products before multiplying source blocks or pixels.
			const unsigned width=std::max(1U,unsigned(texture.descriptor.width)>>mip), height=std::max(1U,unsigned(texture.descriptor.height)>>mip);
			const unsigned blocksX=(width+3)/4,blocksY=(height+3)/4;
			const auto blocks=std::size_t(blocksX)*blocksY;
			const auto bytes=target==InternalFormat::RGBA8 ? std::size_t(width)*height*4 : blocks*16;
			if(totalBlocks>availableBlocks || blocks>availableBlocks-totalBlocks || bytes>std::numeric_limits<std::size_t>::max()-totalBytes) return false;
			levels.push_back({width,height,blocksX,blocksY,blocks,bytes}); totalBlocks+=blocks; totalBytes+=bytes;
		}
		texture.transcodedData.resize(totalBytes); std::vector<TETFLevelEntry> decodedLevels; decodedLevels.reserve(levels.size());
		std::size_t sourceBlock=0,levelOffset=0;
		for(const auto& level:levels) {
			auto* destination=texture.transcodedData.data()+levelOffset;
			for(unsigned by=0;by<level.blocksY;++by) for(unsigned bx=0;bx<level.blocksX;++bx,++sourceBlock) {
				basist::uastc_block source{}; std::memcpy(&source,texture.uastcPayload.data()+sourceBlock*16,16); bool decoded=false;
				if(target==InternalFormat::RGBA8) {
					std::array<basist::color32,16> pixels{};
					decoded=basist::unpack_uastc(source,pixels.data(),false);
					if(decoded) {
						// A source block is four small rows, not a contiguous image
						// strip. Scatter only authored pixels, cropping edge padding.
						const auto x=bx*4,y=by*4; const auto columns=std::min(4U,level.width-x),rows=std::min(4U,level.height-y);
						for(unsigned row=0;row<rows;++row) std::memcpy(destination+(std::size_t(y+row)*level.width+x)*4,pixels.data()+row*4,std::size_t(columns)*4);
					}
				} else {
					auto* block=destination+(std::size_t(by)*level.blocksX+bx)*16;
					decoded=target==InternalFormat::BC7 ? basist::transcode_uastc_to_bc7(source,block) : basist::transcode_uastc_to_astc(source,block);
				}
				if(!decoded) { TERRAENGINE_LOG(LogLevel::Error,"UASTC decoder rejected " << path.filename()); return false; }
			}
			decodedLevels.push_back({std::uint64_t(levelOffset),std::uint64_t(level.bytes)}); levelOffset+=level.bytes;
		}
		texture.levelTable=std::move(decodedLevels);
		const auto duration=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now()-startTime).count();
		TERRAENGINE_LOG(LogLevel::Info,"Transcoded " << path.filename() << " (" << texture.descriptor.width << "x" << texture.descriptor.height << ") to VK format " << texture.descriptor.vkFormat << " in " << duration/1000.0 << " ms");
		return true;
	}

	// Safely translate an absolute file offset into a pointer inside the
	// payload buffer that starts after the chunk table.
	const std::uint8_t* MapPayload(const std::filesystem::path& path, std::uint64_t payloadBase, std::uint64_t payloadSize, const std::vector<std::uint8_t>& storage, std::uint64_t fileOffset, std::uint64_t requiredSize)
	{
		if (fileOffset < payloadBase)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF offset underflows payload base for " << path);
			return nullptr;
		}

		const std::uint64_t relativeOffset = fileOffset - payloadBase;

		if (relativeOffset > payloadSize || requiredSize > payloadSize - relativeOffset || relativeOffset > storage.size() || requiredSize > storage.size() - relativeOffset)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF offset overruns payload for " << path);
			return nullptr;
		}

		// Narrow only after the requested span fits actual owned vector bytes.
		return storage.data() + static_cast<std::size_t>(relativeOffset);
	}

	// Parse a TETF texture container, gather chunk metadata, and expose
	// spans pointing into owned storage.
	std::unique_ptr<Texture> LoadTextureFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		// Pure structural admission precedes descriptor use, allocation based on
		// wire counts, pointer derivation, and decoder calls on owned file bytes.
		const auto admission=TerraEngine::Runtime::ValidateArtifact({std::as_bytes(std::span(fileBytes)),TerraEngine::Runtime::ArtifactKind::Texture,TerraEngine::Runtime::NativeImageTarget::Unsupported,resolvedPath.generic_string()});
		if(!admission.IsValid()) {
			TERRAENGINE_LOG(LogLevel::Error,"TETF admission rejected " << resolvedPath << ": " << admission.code << " " << admission.diagnostic);
			return nullptr;
		}


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

		const std::uint64_t chunkTableSize = std::uint64_t(header.chunkCount) * sizeof(TETFChunkEntry);

		if (header.tocOffset < header.headerSize || header.tocOffset > fileBytes.size() || chunkTableSize > fileBytes.size() - header.tocOffset)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TETF chunk table overruns file for " << resolvedPath);
			return nullptr;
		}

		auto texture = std::make_unique<Texture>();
		texture->header = header;
		texture->chunkTable.resize(header.chunkCount);

		std::memcpy(texture->chunkTable.data(), fileBytes.data() + static_cast<std::size_t>(header.tocOffset), static_cast<std::size_t>(chunkTableSize));

		// The preceding subtraction check proves this sum fits owned file bytes.
		const std::uint64_t payloadBase = header.tocOffset + chunkTableSize;

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

		const std::uint8_t* descPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, descEntry->offset, descEntry->size);

		if (!descPtr)
		{
			return nullptr;
		}

		std::memcpy(&texture->descriptor, descPtr, sizeof(TETFDesc));
		// Defense in depth also protects local descriptor consumers if a future
		// envelope change introduces more than one descriptor record.
		const auto maximumMips=std::bit_width(std::max(unsigned(texture->descriptor.width),unsigned(texture->descriptor.height)));
		if(!texture->descriptor.width || !texture->descriptor.height || !texture->descriptor.mipCount || texture->descriptor.mipCount>16 || texture->descriptor.mipCount>maximumMips) return nullptr;


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
			const std::uint8_t* uastPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, uastEntry->offset, uastEntry->size);

			if (!uastPtr) return nullptr;
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
			const std::uint8_t* metaPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, metaEntry.offset, metaEntry.size);

			if (metaPtr)
			{
				texture->userMetadata = std::span<const std::uint8_t>(metaPtr, static_cast<std::size_t>(metaEntry.size));
			}
		}


		if (texture->descriptor.thumbnailChunk != TETF_INVALID_CHUNK_INDEX && validateChunkIndex(texture->descriptor.thumbnailChunk))
		{
			const TETFChunkEntry& thumbEntry = texture->chunkTable[texture->descriptor.thumbnailChunk];
			const std::uint8_t* thumbPtr = MapPayload(resolvedPath, payloadBase, payloadSize, texture->storage, thumbEntry.offset, thumbEntry.size);

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

TerraEngine::UUID ResourceManager::RegisterTexture(const std::string& cacheKey, std::unique_ptr<Texture> texture)
{
	// Generated replacements acquire the same key authority as disk producers.
	return RegisterGenerated(cacheKey, textureCache, std::move(texture));
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


std::shared_future<TerraEngine::UUID> ResourceManager::ReloadTexturePublicationAsync(const TerraEngine::UUID& id)
{
	// Unlike the compatibility void future, this reports the actual accepted
	// asset identity. Missing or failed producers cannot impersonate publication.
	return ReloadCached<Texture>(id, textureCache, [this](const std::filesystem::path& path) {
		return IsCompiledTexturePath(path) ? LoadTextureFile(*this, path) : std::unique_ptr<Texture>{};
	}, false);
}

std::shared_future<void> ResourceManager::ReloadTexture(const TerraEngine::UUID& id)
{
	RequestPtr request;
	ReloadCached<Texture>(id, textureCache, [this](const std::filesystem::path& path) {
		return IsCompiledTexturePath(path) ? LoadTextureFile(*this, path) : std::unique_ptr<Texture>{};
	}, false, &request);
	if (request) return request->completion;
	// Unregistered identities have no producer; preserve the ready void API.
	std::promise<void> completion;
	completion.set_value();
	return completion.get_future().share();
}
