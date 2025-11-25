#include "Resource/ResourceManager.h"

#include <fstream>
#include <sstream>
#include <utility>

#include "Core/Logger/Logger.h"
// Concrete resource definitions are required here so the cache-owned
// std::unique_ptr instances have complete types when the deleters compile.
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "File Formats/TerraEngineTextureFile.h"

ResourceManager& ResourceManager::Instance()
{
	static ResourceManager instance;
	return instance;
}

// Out-of-line destructor ensures the concrete resource types are complete when
// the cache members are destroyed, keeping unique_ptr deleters well-defined.
ResourceManager::~ResourceManager() = default;

ResourceManager::ResourceManager()
	: threadPool(ThreadPool::instance())
{
	assetRoot = std::filesystem::current_path();
}

void ResourceManager::SetAssetRoot(const std::filesystem::path& assetRootPath)
{
	// Normalize immediately so the cache keys stay predictable even if
	// callers provide paths with redundant separators or relative jumps.
	assetRoot = assetRootPath.lexically_normal();
}

std::filesystem::path ResourceManager::GetAssetRoot() const
{
	return assetRoot;
}

void ResourceManager::ClearCaches()
{
	// Acquire exclusive locks in a consistent order to avoid deadlocks if
	// multiple threads ever clear different caches concurrently.
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.pathToId.clear();
		materialCache.resources.clear();
		materialCache.inFlight.clear();
	}
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.pathToId.clear();
		meshCache.resources.clear();
		meshCache.inFlight.clear();
	}
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId.clear();
		shaderCache.resources.clear();
		shaderCache.inFlight.clear();
	}
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.pathToId.clear();
		textureCache.resources.clear();
		textureCache.inFlight.clear();
	}
}

Material* ResourceManager::GetMaterial(const TerraEngine::UUID& id)
{
	// Callers can stash UUIDs from earlier load operations and pull the
	// current instance when they need to bind resources without owning
	// references for the entire application lifetime.
	return GetResourceById(id, materialCache);
}

Mesh* ResourceManager::GetMesh(const TerraEngine::UUID& id)
{
	return GetResourceById(id, meshCache);
}

Shader* ResourceManager::GetShader(const TerraEngine::UUID& id)
{
	return GetResourceById(id, shaderCache);
}

Texture* ResourceManager::GetTexture(const TerraEngine::UUID& id)
{
	return GetResourceById(id, textureCache);
}

std::filesystem::path ResourceManager::ResolveAssetPath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (assetRoot / path).lexically_normal();
}

std::string ResourceManager::NormalizeKey(const std::filesystem::path& path) const
{
	// Use generic_string to avoid mismatches between Windows backslashes and
	// POSIX forward slashes when keys are compared across platforms.
	return path.lexically_normal().generic_string();
}

std::vector<std::uint8_t> ResourceManager::ReadFileBytes(const std::filesystem::path& path) const
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open resource file: " << path);
		return {};
	}

	const std::streamsize length = file.tellg();
	if (length <= 0)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Resource file is empty: " << path);
		return {};
	}

	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length));

	// Sequential read is the simplest and most cache-friendly path because
	// each resource file is authored as a single contiguous blob.
	file.seekg(0, std::ios::beg);
	if (!file.read(reinterpret_cast<char*>(buffer.data()), length))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to read resource file: " << path);
		return {};
	}

	return buffer;
}
