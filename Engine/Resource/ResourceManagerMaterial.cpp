#include "Resource/ResourceManager.h"

#include <cstring>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineMaterialFile.h"

namespace
{
	// Read and validate a TMAT file. This function only performs disk I/O and
	// buffer parsing; cache management lives in ResourceManager so the same
	// loader can be reused by synchronous and asynchronous callers.
	std::shared_ptr<TerraEngineMaterialFile> LoadMaterialFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		if (fileBytes.size() < sizeof(Header))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT file is too small: " << resolvedPath);
			return nullptr;
		}

		Header header{};
		std::memcpy(&header, fileBytes.data(), sizeof(Header));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		if (magic != kMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT magic mismatch in " << resolvedPath);
			return nullptr;
		}

		if (header.version != kCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT version mismatch in " << resolvedPath << " (expected " << kCurrentVersion << ")");
			return nullptr;
		}

		if (header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT headerSize exceeds file length for " << resolvedPath);
			return nullptr;
		}

		if (header.textureCount > kMaxTextureBindings || header.floatParameterCount > kMaxFloatParameters)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT exceeds supported limits in " << resolvedPath);
			return nullptr;
		}

		const std::size_t payloadOffset = header.headerSize;
		auto material = std::make_shared<TerraEngineMaterialFile>();
		material->header = header;

		material->storage.assign(fileBytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset), fileBytes.end());

		const std::size_t payloadSize = material->storage.size();
		const std::uint8_t* payload = material->storage.data();

		if (header.textureCount > 0)
		{
			if (header.textureArrayOffset < header.headerSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT texture array offset underflows header for " << resolvedPath);
				return nullptr;
			}

			const std::size_t textureOffset = header.textureArrayOffset - header.headerSize;
			const std::size_t textureBytes = header.textureCount * sizeof(TextureBinding);
			if (textureOffset + textureBytes > payloadSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT texture array overruns payload in " << resolvedPath);
				return nullptr;
			}

			material->textures = std::span<const TextureBinding>(reinterpret_cast<const TextureBinding*>(payload + textureOffset), header.textureCount);
		}

		if (header.floatParameterCount > 0)
		{
			if (header.floatParameterOffset < header.headerSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT float parameter offset underflows header for " << resolvedPath);
				return nullptr;
			}

			const std::size_t floatOffset = header.floatParameterOffset - header.headerSize;
			const std::size_t floatBytes = header.floatParameterCount * sizeof(FloatParameter);
			if (floatOffset + floatBytes > payloadSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT float parameters overrun payload in " << resolvedPath);
				return nullptr;
			}

			material->floats = std::span<const FloatParameter>(reinterpret_cast<const FloatParameter*>(payload + floatOffset), header.floatParameterCount);
		}

		return material;
	}
}

std::shared_ptr<TerraEngineMaterialFile> ResourceManager::LoadMaterial(const std::filesystem::path& path)
{
	return LoadOrGetCached<TerraEngineMaterialFile>(path, materialCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMaterialFile(*this, resolvedPath);
	});
}

std::shared_future<std::shared_ptr<TerraEngineMaterialFile>> ResourceManager::LoadMaterialAsync(const std::filesystem::path& path)
{
	return LoadOrSchedule<TerraEngineMaterialFile>(path, materialCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMaterialFile(*this, resolvedPath);
	});
}
