#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineMaterialFile.h"

namespace
{
	// Read and validate a TMAT file. This function only performs disk I/O and
	// buffer parsing; cache management lives in ResourceManager so the same
	// loader can be reused by synchronous and asynchronous callers.
	std::unique_ptr<Material> LoadMaterialFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		if (fileBytes.size() < sizeof(MaterialHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT file is too small: " << resolvedPath);
			return nullptr;
		}

		MaterialHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(MaterialHeader));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		if (magic != kMaterialMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT magic mismatch in " << resolvedPath);
			return nullptr;
		}

		if (header.version != kMaterialCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT version mismatch in " << resolvedPath << " (expected " << kMaterialCurrentVersion << ")");
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
		auto material = std::make_unique<Material>();
		material->header = header;
		material->shaderCollectionId = header.shaderCollectionId;
		// Version-1 TMAT files do not store an explicit shadow behavior field,
		// so the loader derives the least-surprising runtime behavior from the
		// existing opacity and emissive settings.
		material->shadowBehavior = DeriveMaterialShadowBehavior(header.alphaMode, header.emissiveFactor);
		if (material->shaderCollectionId == TerraEngine::EMPTY_UUID)
		{
			// Keep the missing shader reference visible so authored TMAT files can be fixed early.
			TERRAENGINE_LOG(LogLevel::Warning, "TMAT file does not specify a shader collection; " << resolvedPath << " will rely on renderer defaults.");
		}

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

	bool IsCompiledMaterialPath(const std::filesystem::path& path)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return extension == ".tmat";
	}
}

TerraEngine::UUID ResourceManager::LoadMaterial(const std::filesystem::path& path)
{
	if (!IsCompiledMaterialPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping source material load until compiled TMAT is available: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	return LoadOrGetCached<Material>(path, materialCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMaterialFile(*this, resolvedPath);
	});
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadMaterialAsync(const std::filesystem::path& path)
{
	if (!IsCompiledMaterialPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping async source material load until compiled TMAT is available: " << path);
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	return LoadOrSchedule<Material>(path, materialCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMaterialFile(*this, resolvedPath);
	});
}

TerraEngine::UUID ResourceManager::RegisterMaterial(const std::string& cacheKey, std::unique_ptr<Material> material)
{
	// Generated materials do not live on disk, so store them under a synthetic key and keep a stable UUID for callers.
	const std::string normalizedKey = "generated/" + cacheKey;
	std::unique_lock lock(materialCache.mutex);

	TerraEngine::UUID id = TerraEngine::GenerateUUID();
	if (auto existing = materialCache.pathToId.find(normalizedKey); existing != materialCache.pathToId.end())
	{
		id = existing->second;
	}
	materialCache.pathToId[normalizedKey] = id;

	if (material)
	{
		materialCache.resources[id] = std::move(material);
	}
	else
	{
		materialCache.resources.erase(id);
	}

	// Clear any stale in-flight futures that may have been waiting on this key.
	materialCache.inFlight.erase(normalizedKey);
	return id;
}

void ResourceManager::ReloadMaterial(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(materialCache.mutex);
		if (const auto pathIt = materialCache.idToPath.find(id); pathIt != materialCache.idToPath.end())
		{
			key = pathIt->second;
		}
	}

	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Cannot reload material: UUID " << ToHexString(id) << " not found in cache path map.");
		return;
	}

	// Generated materials cannot be reloaded from disk.
	if (key.starts_with("generated/"))
	{
		return;
	}

	const std::filesystem::path resolvedPath(key);
	if (!IsCompiledMaterialPath(resolvedPath))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping material reload from non-TMAT path: " << resolvedPath);
		return;
	}

	TERRAENGINE_LOG(LogLevel::Debug, "Reloading material from " << resolvedPath);
	auto material = LoadMaterialFile(*this, resolvedPath);
	if (material)
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.resources[id] = std::move(material);
	}
}
