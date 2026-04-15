#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <utility>
#include <vector>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>

#include "Core/Export.h"
#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

// Forward declarations keep this header lightweight and avoid forcing every
// includer to pull the binary format structs; the concrete loaders include
// the definitions they need inside their translation units.
struct Material;
struct Mesh;
struct Shader;
struct ShaderCollection;
struct Texture;
enum class ShaderStage : std::uint32_t;

enum class AssetType {
	Mesh,
	Texture,
	Material,
	ShaderCollection
};

struct ShaderStagePath
{
	ShaderStage stage = ShaderStage::Vertex;
	std::filesystem::path path;
};

// Centralized resource cache that understands TerraEngine's authored file
// formats (TMAT, TEMF, TSPF, TETF). The manager resolves asset paths,
// deduplicates loads, and issues asynchronous tasks through the shared
// thread pool so multiple systems can pull resources without blocking the
// main thread. This class intentionally avoids SystemsBase inheritance
// because resources are utility data rather than frame-driven systems, but
// it still exposes explicit Initialize/Shutdown hooks so export archives can
// be mounted once during engine startup instead of on every file read.
class TERRAENGINE_API ResourceManager
{
public:
	// Retrieve the shared instance. The manager is a singleton so every
	// caller observes the same caches and in-flight tasks.
	static ResourceManager& Instance();

	ResourceManager(const ResourceManager&) = delete;
	ResourceManager& operator=(const ResourceManager&) = delete;
	~ResourceManager();

	// Performs one-time startup work for the current runtime session. In export
	// builds this mounts the TEPAK archive and reads game.json before other
	// systems attempt to load scenes or assets through the manager.
	void Initialize();

	// Releases runtime-only state such as mounted export archives and cached
	// startup metadata. This keeps repeated engine sessions in the same process
	// from inheriting stale resource state.
	void Shutdown();

	// Configure the base directory used to resolve internal engine assets.
	void SetEngineRoot(const std::filesystem::path& engineRootPath);

	// Configure the base directory used to resolve relative asset paths.
	// The path is normalized immediately so lookup keys are consistent
	// regardless of the caller's working directory.
	void SetAssetRoot(const std::filesystem::path& assetRootPath);

	// Inspect the currently configured asset root to help debugging when
	// resources cannot be found.
	std::filesystem::path GetAssetRoot() const;

	// Inspect the currently configured engine root so runtime systems can find
	// bundled engine-owned assets such as shaders and fallback fonts without
	// depending on the process working directory.
	std::filesystem::path GetEngineRoot() const
	{
		return engineRoot;
	}

	// Records the exported TEPAK archive that should be mounted later during
	// Initialize(). The launcher only provides the path; ResourceManager owns
	// the actual mount so content bootstrap happens inside engine startup.
	void ConfigureExportArchive(const std::filesystem::path& archivePath);

	// Exposes the startup scene discovered from the export manifest so the
	// engine can hand it to SceneManager before scene initialization begins.
	std::optional<std::filesystem::path> GetConfiguredStartupScene() const;

	// Returns the manifest-backed product metadata that Application.cs exposes to
	// gameplay scripts. These values come from project-root or export game.json.
	std::string GetProductName() const;
	std::string GetVersion() const;

	// Lets runtime systems distinguish packaged export sessions from editor
	// project sessions so exported builds can keep their writable files in a
	// flatter layout without changing the editor project structure.
	bool IsExportRuntime() const;

	// Loads the default resources (shaders) with fixed UUIDs.
	void LoadDefaultResources();

	// Returns the fixed UUID for the default shader collection.
	TerraEngine::UUID GetDefaultShaderCollectionID() const;
	TerraEngine::UUID GetDefaultTaskShaderCollectionID() const;

	// Synchronous load helpers. These block the calling thread until the
	// file is parsed, but they reuse cached results or join any in-flight
	// asynchronous request for the same asset to avoid duplicate work.
	TerraEngine::UUID LoadMaterial(const std::filesystem::path& path);
	TerraEngine::UUID LoadMesh(const std::filesystem::path& path);
	TerraEngine::UUID LoadShader(const std::filesystem::path& path);
	TerraEngine::UUID LoadShaderCollection(const std::vector<ShaderStagePath>& stagePaths);
	TerraEngine::UUID LoadTexture(const std::filesystem::path& path);

	// Re-parse a material from disk to pick up authored changes without clearing
	// the entire cache. This ensures the renderer sees new parameters immediately.
	void ReloadMaterial(const TerraEngine::UUID& id);

	// Re-parse a mesh from disk to pick up authored changes.
	void ReloadMesh(const TerraEngine::UUID& id);

	// Re-parse a texture from disk to pick up authored changes.
	std::shared_future<void> ReloadTexture(const TerraEngine::UUID& id);

	// Unregister a resource from the cache and trigger any necessary renderer invalidations.
	void DeleteResource(const TerraEngine::UUID& id, AssetType type);

	// Check if a resource with the given UUID exists in any of the manager's caches.
	bool HasResource(const TerraEngine::UUID& id) const;

	// Register a mapping between a UUID and a filesystem path so that GetMesh/GetTexture
	// can trigger on-demand loading if the resource is not yet in the cache.
	void RegisterAssetPath(const TerraEngine::UUID& id, const std::filesystem::path& path, AssetType type);
	// Resolve a script-facing asset reference into the UUID the renderer uses
	// internally. The lookup accepts either an exact relative path or a simple
	// asset name/stem so gameplay code does not need to memorize UUIDs.
	TerraEngine::UUID ResolveAssetReference(std::string_view assetReference, AssetType type);
	// Convert an internal UUID back into a readable asset path so managed code
	// can display or persist friendly identifiers instead of raw GUID text.
	std::filesystem::path DescribeAssetReference(const TerraEngine::UUID& id, AssetType type) const;

	// Inject a generated material directly into the cache so tooling can synthesize preview materials
	// without writing TMAT files to disk. The cacheKey should be unique and stable (e.g., "generated/preview").
	TerraEngine::UUID RegisterMaterial(const std::string& cacheKey, std::unique_ptr<Material> material);

	// Asynchronous load helpers. Each call schedules a background task on
	// the shared thread pool and returns a shared_future that multiple
	// callers can await safely. Duplicate requests will join the same
	// in-flight future so disk I/O is never duplicated.
	std::shared_future<TerraEngine::UUID> LoadMaterialAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadMeshAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadShaderAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadShaderCollectionAsync(const std::vector<ShaderStagePath>& stagePaths);
	std::shared_future<TerraEngine::UUID> LoadTextureAsync(const std::filesystem::path& path);

	// Fetch a loaded resource using its UUID so callers can pass lightweight
	// identifiers around without holding shared ownership. Returned pointers
	// are non-owning views into the cache and remain valid until ClearCaches
	// or a subsequent reload replaces the resource.
	Material* GetMaterial(const TerraEngine::UUID& id);
	Mesh* GetMesh(const TerraEngine::UUID& id);
	Shader* GetShader(const TerraEngine::UUID& id);
	ShaderCollection* GetShaderCollection(const TerraEngine::UUID& id);
	Texture* GetTexture(const TerraEngine::UUID& id);

	// Clear every cache and abandon in-flight futures. Callers should only
	// use this during teardown when they can guarantee no other threads are
	// still reading resources.
	void ClearCaches();

	// Utility entry point used by format-specific loader helpers to pull
	// bytes from disk after the path has been resolved.
	std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) const;

private:
	ResourceManager();

	template <typename Resource>
	struct ResourceCache
	{
		// Loaded instances keyed by normalized absolute paths and UUIDs for
		// quick lookup. UUID map owns the resources; path map provides
		// deterministic deduplication across synchronous/async callers.
		std::unordered_map<std::string, TerraEngine::UUID> pathToId;
		std::unordered_map<TerraEngine::UUID, std::string> idToPath;
		std::unordered_map<TerraEngine::UUID, std::unique_ptr<Resource>> resources;
		// Futures for tasks that are actively loading. Shared futures let
		// multiple threads wait without spawning redundant tasks.
		std::unordered_map<std::string, std::shared_future<TerraEngine::UUID>> inFlight;
		mutable std::shared_mutex mutex;
	};

	template <typename Resource>
	using LoaderFunction = std::function<std::unique_ptr<Resource>(const std::filesystem::path&)>;

	// Wrap a UUID in a ready future so synchronous results can be reused by
	// asynchronous callers without extra copies.
	std::shared_future<TerraEngine::UUID> MakeReadyFuture(const TerraEngine::UUID& id);

	// Resolve any cached resource or wait for an in-flight future if one
	// exists. Returns std::nullopt when the resource has not been requested
	// yet so the caller knows to perform the load.
	template <typename Resource>
	std::optional<TerraEngine::UUID> ResolveCached(const std::string& key, ResourceCache<Resource>& cache);

	// Ensure the cache has a UUID reserved for the requested key so that
	// repeated calls return stable identifiers even if resources are evicted.
	template <typename Resource>
	TerraEngine::UUID EnsureResourceId(const std::string& key, ResourceCache<Resource>& cache);

	// Retrieve a loaded resource from the UUID lookup table.
	template <typename Resource>
	Resource* GetResourceById(const TerraEngine::UUID& id, ResourceCache<Resource>& cache, const std::function<TerraEngine::UUID(const std::filesystem::path&)>& loader = nullptr);

	// Internal synchronous load wrapper shared by each format-specific
	// loader. The provided loader is only executed when the cache does not
	// already contain the requested asset.
	template <typename Resource>
	TerraEngine::UUID LoadOrGetCached(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader);

	// Route type-specific cache work through one visitor so asset-path
	// registration stays generic instead of open-coding one branch per cache.
	template <typename Visitor>
	void VisitAssetCache(AssetType type, Visitor&& visitor);

	// Internal asynchronous wrapper that enqueues the loader on the shared
	// thread pool. If the resource already exists or is in-flight, the
	// existing shared_future is returned instead of scheduling new work.
	template <typename Resource>
	std::shared_future<TerraEngine::UUID> LoadOrSchedule(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader);

	// Finalize a successful load by updating caches and clearing any
	// in-flight tracking entries. This runs on both synchronous and
	// asynchronous code paths to keep cache semantics consistent.
	template <typename Resource>
	TerraEngine::UUID FinalizeLoad(const std::string& key, ResourceCache<Resource>& cache, const TerraEngine::UUID& id, std::unique_ptr<Resource> resource);

	// Collapse relative paths against the configured asset root so caching
	// keys remain stable even when callers run in different directories.
	std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const;

	// Produce a normalized string key used by the cache maps. Paths are
	// stored using generic separators to avoid Windows/Linux mismatches.
	std::string NormalizeKey(const std::filesystem::path& path) const;

	// Reset manifest metadata to safe defaults so repeated editor/export runs in
	// the same process never leak stale product information.
	void ResetManifestMetadata(std::string_view fallbackProductName = {});

	// Apply name/version values from a parsed manifest while preserving stable
	// fallbacks when fields are absent or empty.
	void ApplyManifestMetadata(const nlohmann::json& manifest, std::string_view fallbackProductName);

	// Editor projects keep their manifest on disk beside ProjectSettings rather
	// than inside a mounted archive, so refresh it whenever the asset root moves.
	void RefreshEditorProjectManifest();

	ThreadPool& threadPool;
	std::filesystem::path assetRoot;
	std::filesystem::path engineRoot;
	std::optional<std::filesystem::path> m_ConfiguredStartupScene;
	std::optional<std::filesystem::path> m_ConfiguredExportArchivePath;
	std::string m_ProductName = "TerraEngine";
	std::string m_Version = "1.0.0";

	ResourceCache<Material> materialCache;
	ResourceCache<Mesh> meshCache;
	ResourceCache<Shader> shaderCache;
	ResourceCache<ShaderCollection> shaderCollectionCache;
	ResourceCache<Texture> textureCache;
};

// Render a UUID as a hex string for readable debug logging without adding a
// dedicated conversion helper elsewhere in the engine.
inline std::string ToHexString(const TerraEngine::UUID& id)
{
	return TerraEngine::ToHexString(id);
}

inline std::shared_future<TerraEngine::UUID> ResourceManager::MakeReadyFuture(const TerraEngine::UUID& id)
{
	std::promise<TerraEngine::UUID> promise;
	promise.set_value(id);
	return promise.get_future().share();
}

template <typename Resource>
std::optional<TerraEngine::UUID> ResourceManager::ResolveCached(const std::string& key, ResourceCache<Resource>& cache)
{
	std::shared_lock lock(cache.mutex);
	if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
	{
		const TerraEngine::UUID& id = existing->second;
		if (cache.resources.find(id) != cache.resources.end())
		{
			// Debug log helps trace cache reuse when resources are requested repeatedly.
			TERRAENGINE_LOG(LogLevel::Debug, "Resource cache hit for " << key << " (id " << ToHexString(id) << ")");
			return id;
		}
	}

	if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
	{
		// Make it obvious when a caller piggybacks an already scheduled load instead of duplicating work.
		TERRAENGINE_LOG(LogLevel::Debug, "Resource load already in-flight for " << key << "; joining existing future");
		auto future = pending->second;
		lock.unlock();
		return future.get();
	}

	return std::nullopt;
}

template <typename Resource>
TerraEngine::UUID ResourceManager::EnsureResourceId(const std::string& key, ResourceCache<Resource>& cache)
{
	std::unique_lock lock(cache.mutex);
	if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
	{
		return existing->second;
	}

	const auto id = TerraEngine::GenerateUUID();
	cache.pathToId.emplace(key, id);
	cache.idToPath.emplace(id, key);
	// Provide debug visibility whenever a new UUID is assigned to a resource key.
	TERRAENGINE_LOG(LogLevel::Debug, "Assigned UUID " << ToHexString(id) << " to resource key " << key);
	return id;
}

template <typename Resource>
Resource* ResourceManager::GetResourceById(const TerraEngine::UUID& id, ResourceCache<Resource>& cache, const std::function<TerraEngine::UUID(const std::filesystem::path&)>& loader)
{
	{
		std::shared_lock lock(cache.mutex);
		if (auto existing = cache.resources.find(id); existing != cache.resources.end())
		{
			return existing->second.get();
		}

		if (loader)
		{
			if (auto it = cache.idToPath.find(id); it != cache.idToPath.end())
			{
				const std::string path = it->second;
				lock.unlock(); // Must release shared lock before triggering a load which acquires unique lock
				loader(path);
				
				// Re-acquire shared lock and return from cache
				std::shared_lock relock(cache.mutex);
				if (auto reloaded = cache.resources.find(id); reloaded != cache.resources.end())
				{
					return reloaded->second.get();
				}
			}
		}
	}

	return nullptr;
}

template <typename Resource>
TerraEngine::UUID ResourceManager::FinalizeLoad(const std::string& key, ResourceCache<Resource>& cache, const TerraEngine::UUID& id, std::unique_ptr<Resource> resource)
{
	// Capture success before we move the resource into the cache so logging reflects the actual loader result.
	const bool loadedSuccessfully = static_cast<bool>(resource);

	std::unique_lock lock(cache.mutex);
	cache.pathToId[key] = id;

	if (resource)
	{
		cache.resources[id] = std::move(resource);
	}
	else
	{
		cache.resources.erase(id);
	}

	cache.inFlight.erase(key);
	// Log the outcome to tie disk/parsing work back to a UUID for later lookups.
	if (loadedSuccessfully)
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Finalized load for " << key << " (id " << ToHexString(id) << ")");
	}
	else
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Load failed for " << key << "; cache entry cleared for id " << ToHexString(id));
	}
	return id;
}

template <typename Resource>
TerraEngine::UUID ResourceManager::LoadOrGetCached(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);

	if (auto cached = ResolveCached(key, cache))
	{
		return *cached;
	}

	const TerraEngine::UUID id = EnsureResourceId(key, cache);
	// Record when a synchronous load path kicks off so noisy I/O can be traced during debugging.
	TERRAENGINE_LOG(LogLevel::Debug, "Loading resource from " << resolvedPath << " (id " << ToHexString(id) << ")");
	auto resource = loader(resolvedPath);
	return FinalizeLoad(key, cache, id, std::move(resource));
}

template <typename Visitor>
void ResourceManager::VisitAssetCache(AssetType type, Visitor&& visitor)
{
	switch (type)
	{
	case AssetType::Mesh:
		visitor(meshCache);
		return;
	case AssetType::Texture:
		visitor(textureCache);
		return;
	case AssetType::Material:
		visitor(materialCache);
		return;
	case AssetType::ShaderCollection:
		visitor(shaderCollectionCache);
		return;
	default:
		return;
	}
}

template <typename Resource>
std::shared_future<TerraEngine::UUID> ResourceManager::LoadOrSchedule(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);

	{
		std::shared_lock lock(cache.mutex);
		if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
		{
			if (cache.resources.find(existing->second) != cache.resources.end())
			{
				return MakeReadyFuture(existing->second);
			}
		}

		if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
		{
			return pending->second;
		}
	}

	const TerraEngine::UUID id = EnsureResourceId(key, cache);

	// Schedule the expensive file I/O work on the thread pool so callers
	// can continue executing immediately.
	// Debug logging clarifies when background work is spun up versus reused from cache.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async load for " << resolvedPath << " (id " << ToHexString(id) << ")");
	auto task = threadPool.createTask([this, key, resolvedPath, &cache, id, loader]() -> TerraEngine::UUID {
		auto resource = loader(resolvedPath);
		return FinalizeLoad(key, cache, id, std::move(resource));
	});

	auto future = task->getFuture().share();
	{
		std::unique_lock lock(cache.mutex);
		cache.inFlight[key] = future;
	}
	threadPool.enqueue(task);

	return future;
}
