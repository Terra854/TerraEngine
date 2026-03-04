#include "Resource/ResourceManager.h"
#include "Renderer/RenderSystem.h"

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
#include "Resource/ShaderCollection.h"

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
	engineRoot = std::filesystem::current_path();
}

void ResourceManager::SetEngineRoot(const std::filesystem::path& engineRootPath)
{
	engineRoot = engineRootPath.lexically_normal();
	TERRAENGINE_LOG(LogLevel::Debug, "Engine root set to " << engineRoot);
}

void ResourceManager::SetAssetRoot(const std::filesystem::path& assetRootPath)
{
	// Normalize immediately so the cache keys stay predictable even if
	// callers provide paths with redundant separators or relative jumps.
	assetRoot = assetRootPath.lexically_normal();
	// Verbose debug logging makes it obvious which asset tree subsequent loads will pull from.
	TERRAENGINE_LOG(LogLevel::Debug, "Asset root set to " << assetRoot);
}

std::filesystem::path ResourceManager::GetAssetRoot() const
{
	return assetRoot;
}

void ResourceManager::LoadDefaultResources()
{
	TerraEngine::UUID defaultMeshId{};
	defaultMeshId.fill(0);
	defaultMeshId[15] = 1;

	TerraEngine::UUID defaultFragId{};
	defaultFragId.fill(0);
	defaultFragId[15] = 2;

	TerraEngine::UUID defaultTaskId{};
	defaultTaskId.fill(0);
	defaultTaskId[15] = 3;

	TerraEngine::UUID defaultMeshOnlyCollectionId{};
	defaultMeshOnlyCollectionId.fill(0);
	defaultMeshOnlyCollectionId[15] = 1;

	TerraEngine::UUID defaultTaskCollectionId{};
	defaultTaskCollectionId.fill(0);
	defaultTaskCollectionId[15] = 2;

	auto ResolveEnginePath = [this](const std::filesystem::path& p) {
		if (p.is_absolute()) return p.lexically_normal();
		return (engineRoot / p).lexically_normal();
	};

	// Pre-register Mesh Shader
	const std::string meshPath = "Shaders/Default.mesh.tspf";
	std::string meshKey = NormalizeKey(ResolveEnginePath(meshPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[meshKey] = defaultMeshId;
	}
	
	// LoadShader normally uses assetRoot, so we must pass the absolute path
	LoadShader(ResolveEnginePath(meshPath));

	// Pre-register Task Shader
	const std::string taskPath = "Shaders/Default.task.tspf";
	std::string taskKey = NormalizeKey(ResolveEnginePath(taskPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[taskKey] = defaultTaskId;
	}
	LoadShader(ResolveEnginePath(taskPath));
	const bool taskShaderReady = (GetShader(defaultTaskId) != nullptr);

	// Pre-register Fragment Shader
	const std::string fragPath = "Shaders/Default.frag.tspf";
	std::string fragKey = NormalizeKey(ResolveEnginePath(fragPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[fragKey] = defaultFragId;
	}
	LoadShader(ResolveEnginePath(fragPath));
	// Pre-register mesh-only fallback collection UUID.
	std::ostringstream fallbackCollectionKeyStream;
	fallbackCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Mesh) << ":" << meshKey << ";";
	fallbackCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Fragment) << ":" << fragKey << ";";
	const std::string fallbackCollectionKey = fallbackCollectionKeyStream.str();

	// Pre-register task+mesh+fragment collection UUID.
	std::ostringstream taskCollectionKeyStream;
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Task) << ":" << taskKey << ";";
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Mesh) << ":" << meshKey << ";";
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Fragment) << ":" << fragKey << ";";
	const std::string taskCollectionKey = taskCollectionKeyStream.str();
	{
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId[fallbackCollectionKey] = defaultMeshOnlyCollectionId;
		shaderCollectionCache.pathToId[taskCollectionKey] = defaultTaskCollectionId;
	}

	std::vector<ShaderStagePath> fallbackStages = {
		{ ShaderStage::Mesh, ResolveEnginePath(meshPath) },
		{ ShaderStage::Fragment, ResolveEnginePath(fragPath) }
	};
	LoadShaderCollection(fallbackStages);

	if (taskShaderReady)
	{
		std::vector<ShaderStagePath> taskStages = {
			{ ShaderStage::Task, ResolveEnginePath(taskPath) },
			{ ShaderStage::Mesh, ResolveEnginePath(meshPath) },
			{ ShaderStage::Fragment, ResolveEnginePath(fragPath) }
		};
		LoadShaderCollection(taskStages);
	}
	else
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Default task shader was not available; renderer will use mesh-only fallback collection.");
	}
}

TerraEngine::UUID ResourceManager::GetDefaultShaderCollectionID() const
{
	TerraEngine::UUID id{};
	id.fill(0);
	id[15] = 1;
	return id;
}

TerraEngine::UUID ResourceManager::GetDefaultTaskShaderCollectionID() const
{
	TerraEngine::UUID id{};
	id.fill(0);
	id[15] = 2;
	return id;
}

void ResourceManager::ClearCaches()
{
	// Acquire exclusive locks in a consistent order to avoid deadlocks if
	// multiple threads ever clear different caches concurrently.
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.pathToId.clear();
		materialCache.idToPath.clear();
		materialCache.resources.clear();
		materialCache.inFlight.clear();
	}
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.pathToId.clear();
		meshCache.idToPath.clear();
		meshCache.resources.clear();
		meshCache.inFlight.clear();
	}
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId.clear();
		shaderCache.idToPath.clear();
		shaderCache.resources.clear();
		shaderCache.inFlight.clear();
	}
	{
		// Clear paired shader lookups so future installs cannot reuse stale stage combinations.
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId.clear();
		shaderCollectionCache.idToPath.clear();
		shaderCollectionCache.resources.clear();
		shaderCollectionCache.inFlight.clear();
	}
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.pathToId.clear();
		textureCache.idToPath.clear();
		textureCache.resources.clear();
		textureCache.inFlight.clear();
	}
}

void ResourceManager::DeleteResource(const TerraEngine::UUID& id, AssetType type)
{
	auto removeFromCache = [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		if (auto it = cache.idToPath.find(id); it != cache.idToPath.end()) {
			cache.pathToId.erase(it->second);
			cache.idToPath.erase(it);
		}
		cache.resources.erase(id);
	};

	switch (type) {
	case AssetType::Mesh: removeFromCache(meshCache); break;
	case AssetType::Texture: removeFromCache(textureCache); break;
	case AssetType::Material: removeFromCache(materialCache); break;
	case AssetType::ShaderCollection: removeFromCache(shaderCollectionCache); break;
	}

	// Notify renderer to purge any GPU-side references/buffers
	RenderSystem::Instance().DeleteResource(id);
}

bool ResourceManager::HasResource(const TerraEngine::UUID& id) const
{
	auto inCache = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		return cache.resources.find(id) != cache.resources.end();
	};

	return inCache(meshCache) || inCache(textureCache) || inCache(materialCache) ||
		inCache(shaderCollectionCache) || inCache(shaderCache);
}

void ResourceManager::RegisterAssetPath(const TerraEngine::UUID& id, const std::filesystem::path& path, AssetType type)
{
	const std::string key = NormalizeKey(ResolveAssetPath(path));
	switch (type)
	{
	case AssetType::Mesh:
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.pathToId[key] = id;
		meshCache.idToPath[id] = key;
		break;
	}
	case AssetType::Texture:
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.pathToId[key] = id;
		textureCache.idToPath[id] = key;
		break;
	}
	case AssetType::Material:
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.pathToId[key] = id;
		materialCache.idToPath[id] = key;
		break;
	}
	case AssetType::ShaderCollection:
	{
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId[key] = id;
		shaderCollectionCache.idToPath[id] = key;
		break;
	}
	}
}

Material* ResourceManager::GetMaterial(const TerraEngine::UUID& id)
{
	return GetResourceById<Material>(id, materialCache, [this](const std::filesystem::path& p) { return LoadMaterial(p); });
}

Mesh* ResourceManager::GetMesh(const TerraEngine::UUID& id)
{
	return GetResourceById<Mesh>(id, meshCache, [this](const std::filesystem::path& p) { return LoadMesh(p); });
}

Shader* ResourceManager::GetShader(const TerraEngine::UUID& id)
{
	return GetResourceById<Shader>(id, shaderCache, [this](const std::filesystem::path& p) { return LoadShader(p); });
}

ShaderCollection* ResourceManager::GetShaderCollection(const TerraEngine::UUID& id)
{
	return GetResourceById<ShaderCollection>(id, shaderCollectionCache);
}

Texture* ResourceManager::GetTexture(const TerraEngine::UUID& id)
{
	return GetResourceById<Texture>(id, textureCache, [this](const std::filesystem::path& p) { return LoadTexture(p); });
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
	if (!std::filesystem::exists(path))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Resource file does not exist: " << path);
		return {};
	}

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

	// Emit a debug log with the resolved size so callers can correlate reads with disk usage.
	TERRAENGINE_LOG(LogLevel::Debug, "Reading resource file " << path << " (" << length << " bytes)");

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
