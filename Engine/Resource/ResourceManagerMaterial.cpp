// ResourceManagerMaterial.cpp — Material Resource Management Implementation
//
// Implements the management of material resources, defining the surface properties of rendered objects.
// It handles the loading and parsing of material configuration files, including parameter mapping and texture assignments.
// This module integrates surface definitions with shader collections to drive the visual appearance of scene entities.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <vector>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/MaterialRuntimeContract.h"

namespace
{
	// TMVG is appended after the legacy arrays. Its magic and payload size keep
	// a regular v1 TMAT fully readable while making malformed VG policy fail
	// closed instead of selecting an arbitrary fallback route.
	bool LoadVirtualGeometryMaterialExtension(
		const std::filesystem::path& path,
		const std::vector<std::uint8_t>& fileBytes,
		std::size_t knownPayloadEnd,
		std::optional<VirtualGeometryMaterialExtension>& extension)
	{
		if(knownPayloadEnd >= fileBytes.size())
		{
			return knownPayloadEnd == fileBytes.size();
		}

		const std::size_t remainingBytes = fileBytes.size() - knownPayloadEnd;
		if(remainingBytes < sizeof(VirtualGeometryMaterialExtensionHeader))
		{
			TERRAENGINE_LOG(LogLevel::Warning,
				"Ignoring unrecognized trailing TMAT bytes in " << path);
			return true;
		}

		VirtualGeometryMaterialExtensionHeader extensionHeader{};
		std::memcpy(&extensionHeader, fileBytes.data() + knownPayloadEnd,
			sizeof(extensionHeader));
		if(extensionHeader.magic != kVirtualGeometryMaterialExtensionMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Warning,
				"Ignoring unknown trailing TMAT extension in " << path);
			return true;
		}
		if(extensionHeader.payloadSize != sizeof(VirtualGeometryMaterialExtension)
			|| remainingBytes < sizeof(extensionHeader) + extensionHeader.payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TMVG extension size is invalid in " << path);
			return false;
		}

		VirtualGeometryMaterialExtension parsed{};
		std::memcpy(&parsed,
			fileBytes.data() + knownPayloadEnd + sizeof(extensionHeader),
			sizeof(parsed));
		if(!IsValidVirtualGeometryMaterialExtension(parsed))
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TMVG extension is invalid in " << path);
			return false;
		}

		extension = parsed;
		return true;
	}

	// Read and validate a TMAT file. This function only performs disk I/O and
	// buffer parsing; cache management lives in ResourceManager so the same
	// loader can be reused by synchronous and asynchronous callers.
	std::unique_ptr<Material> LoadMaterialFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fileBytes.size() < sizeof(MaterialHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT file is too small: " << resolvedPath);
			return nullptr;
		}

		MaterialHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(MaterialHeader));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (magic != kMaterialMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT magic mismatch in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.version != kMaterialCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT version mismatch in " << resolvedPath << " (expected " << kMaterialCurrentVersion << ")");
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TMAT headerSize exceeds file length for " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.textureCount > 0)
		{
			if (header.textureArrayOffset < header.headerSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT texture array offset underflows header for " << resolvedPath);
				return nullptr;
			}

			const std::size_t textureOffset = header.textureArrayOffset - header.headerSize;
			const std::size_t textureBytes = header.textureCount * sizeof(TextureBinding);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (textureOffset + textureBytes > payloadSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT texture array overruns payload in " << resolvedPath);
				return nullptr;
			}

			material->textures = std::span<const TextureBinding>(reinterpret_cast<const TextureBinding*>(payload + textureOffset), header.textureCount);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.floatParameterCount > 0)
		{
			if (header.floatParameterOffset < header.headerSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT float parameter offset underflows header for " << resolvedPath);
				return nullptr;
			}

			const std::size_t floatOffset = header.floatParameterOffset - header.headerSize;
			const std::size_t floatBytes = header.floatParameterCount * sizeof(FloatParameter);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (floatOffset + floatBytes > payloadSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TMAT float parameters overrun payload in " << resolvedPath);
				return nullptr;
			}

			material->floats = std::span<const FloatParameter>(reinterpret_cast<const FloatParameter*>(payload + floatOffset), header.floatParameterCount);
		}

		// The legacy header and array bounds above define the first safe byte for
		// an optional extension. A v1 material without extra bytes therefore has
		// no virtual-geometry policy rather than an implied one.
		std::size_t knownPayloadEnd = header.headerSize;
		if(header.textureCount > 0)
		{
			knownPayloadEnd = std::max(knownPayloadEnd,
				static_cast<std::size_t>(header.textureArrayOffset)
					+ static_cast<std::size_t>(header.textureCount) * sizeof(TextureBinding));
		}
		if(header.floatParameterCount > 0)
		{
			knownPayloadEnd = std::max(knownPayloadEnd,
				static_cast<std::size_t>(header.floatParameterOffset)
					+ static_cast<std::size_t>(header.floatParameterCount) * sizeof(FloatParameter));
		}
		if(!LoadVirtualGeometryMaterialExtension(resolvedPath, fileBytes,
			knownPayloadEnd, material->virtualGeometryExtension))
		{
			return nullptr;
		}

		MaterialRuntimeContract contract = ResolveMaterialRuntimeContract(material->header, material->textures);
		material->resolvedSlots = std::move(contract.slots);
		material->fallbackWarnings = std::move(contract.warnings);
		for (const std::string& warning : material->fallbackWarnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "TMAT fallback in " << resolvedPath << ": " << warning);
		}

		return material;
	}

	// IsCompiledMaterialPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsCompiledMaterialPath(const std::filesystem::path& path)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return extension == ".tmat";
	}
}

// ResourceManager::LoadMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ResourceManager::LoadMaterialAsync is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ResourceManager::RegisterMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::RegisterMaterial(const std::string& cacheKey, std::unique_ptr<Material> material)
{
	// Generated materials do not live on disk, so store them under a synthetic key and keep a stable UUID for callers.
	const std::string normalizedKey = "generated/" + cacheKey;
	std::unique_lock lock(materialCache.mutex);

	TerraEngine::UUID id = TerraEngine::GenerateUUID();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto existing = materialCache.pathToId.find(normalizedKey); existing != materialCache.pathToId.end())
	{
		id = existing->second;
	}
	materialCache.pathToId[normalizedKey] = id;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (material)
	{
		materialCache.resources[id] = std::move(material);
	}
	else
	{
		materialCache.resources.erase(id);
	}
	// Generated materials bypass FinalizeLoad, so publish the same renderer
	// invalidation token here after their complete cache state becomes visible.
	m_RendererResourcePublicationGeneration.fetch_add(1U, std::memory_order_release);

	// Clear any stale in-flight futures that may have been waiting on this key.
	materialCache.inFlight.erase(normalizedKey);
	return id;
}

// ResourceManager::ReloadMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ReloadMaterial(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(materialCache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto pathIt = materialCache.idToPath.find(id); pathIt != materialCache.idToPath.end())
		{
			key = pathIt->second;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Cannot reload material: UUID " << ToHexString(id) << " not found in cache path map.");
		return;
	}

	// Generated materials cannot be reloaded from disk.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (key.starts_with("generated/"))
	{
		return;
	}

	const std::filesystem::path resolvedPath(key);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsCompiledMaterialPath(resolvedPath))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping material reload from non-TMAT path: " << resolvedPath);
		return;
	}

	TERRAENGINE_LOG(LogLevel::Debug, "Reloading material from " << resolvedPath);
	auto material = LoadMaterialFile(*this, resolvedPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (material)
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.resources[id] = std::move(material);
		// A material becomes renderer-visible only after its complete payload has
		// replaced the cached value; malformed reloads intentionally publish nothing.
		m_RendererResourcePublicationGeneration.fetch_add(1U, std::memory_order_release);
	}
}

bool ResourceManager::ReloadMaterialFromAuthoredPath(
	const TerraEngine::UUID& id,
	const std::filesystem::path& authoredPath)
{
	// TMAT source assets already use the runtime binary contract. Loading the
	// atomically published authored file avoids waiting for the file watcher to
	// copy the same bytes into Build before an Inspector preview can update.
	if(id == TerraEngine::EMPTY_UUID || !IsCompiledMaterialPath(authoredPath))
	{
		return false;
	}
	auto material = LoadMaterialFile(*this, authoredPath);
	if(!material)
	{
		return false;
	}
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.resources[id] = std::move(material);
		// Readers observe the generation only after the complete replacement owns
		// all spans into its new storage buffer.
		m_RendererResourcePublicationGeneration.fetch_add(1U, std::memory_order_release);
	}
	return true;
}
