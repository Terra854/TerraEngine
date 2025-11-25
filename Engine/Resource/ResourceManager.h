#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

#include "Core/Export.h"
#include "ThreadPool/ThreadPool.h"

// Forward declarations keep this header lightweight while avoiding the
// kMagicValue naming collisions in the file format headers. Individual
// loader translation units include the concrete definitions they need.
struct TerraEngineMaterialFile;
struct TerraEngineMeshFile;
struct TerraEngineShaderFile;
struct TerraEngineTextureFile;

// Centralized resource cache that understands TerraEngine's authored file
// formats (TMAT, TEMF, TSPF, TETF). The manager resolves asset paths,
// deduplicates loads, and issues asynchronous tasks through the shared
// thread pool so multiple systems can pull resources without blocking the
// main thread. This class intentionally avoids SystemsBase inheritance
// because resources are utility data rather than frame-driven systems.
class TERRAENGINE_API ResourceManager
{
public:
	// Retrieve the shared instance. The manager is a singleton so every
	// caller observes the same caches and in-flight tasks.
	static ResourceManager& Instance();

	ResourceManager(const ResourceManager&) = delete;
	ResourceManager& operator=(const ResourceManager&) = delete;

	// Configure the base directory used to resolve relative asset paths.
	// The path is normalized immediately so lookup keys are consistent
	// regardless of the caller's working directory.
	void SetAssetRoot(const std::filesystem::path& assetRootPath);

	// Inspect the currently configured asset root to help debugging when
	// resources cannot be found.
	std::filesystem::path GetAssetRoot() const;

	// Synchronous load helpers. These block the calling thread until the
	// file is parsed, but they reuse cached results or join any in-flight
	// asynchronous request for the same asset to avoid duplicate work.
	std::shared_ptr<TerraEngineMaterialFile> LoadMaterial(const std::filesystem::path& path);
	std::shared_ptr<TerraEngineMeshFile> LoadMesh(const std::filesystem::path& path);
	std::shared_ptr<TerraEngineShaderFile> LoadShader(const std::filesystem::path& path);
	std::shared_ptr<TerraEngineTextureFile> LoadTexture(const std::filesystem::path& path);

	// Asynchronous load helpers. Each call schedules a background task on
	// the shared thread pool and returns a shared_future that multiple
	// callers can await safely. Duplicate requests will join the same
	// in-flight future so disk I/O is never duplicated.
	std::shared_future<std::shared_ptr<TerraEngineMaterialFile>> LoadMaterialAsync(const std::filesystem::path& path);
	std::shared_future<std::shared_ptr<TerraEngineMeshFile>> LoadMeshAsync(const std::filesystem::path& path);
	std::shared_future<std::shared_ptr<TerraEngineShaderFile>> LoadShaderAsync(const std::filesystem::path& path);
	std::shared_future<std::shared_ptr<TerraEngineTextureFile>> LoadTextureAsync(const std::filesystem::path& path);

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
		// Loaded instances keyed by normalized absolute paths. Weak pointers
		// avoid keeping assets alive forever if no callers retain them.
		std::unordered_map<std::string, std::weak_ptr<Resource>> loaded;
		// Futures for tasks that are actively loading. Shared futures let
		// multiple threads wait without spawning redundant tasks.
		std::unordered_map<std::string, std::shared_future<std::shared_ptr<Resource>>> inFlight;
		mutable std::shared_mutex mutex;
	};

	template <typename Resource>
	using LoaderFunction = std::function<std::shared_ptr<Resource>(const std::filesystem::path&)>;

	// Wrap a fully loaded resource in a ready future so synchronous results
	// can be reused by asynchronous callers without extra copies.
	template <typename Resource>
	std::shared_future<std::shared_ptr<Resource>> MakeReadyFuture(std::shared_ptr<Resource> resource);

	// Resolve any cached resource or wait for an in-flight future if one
	// exists. Returns nullptr when the resource has not been requested yet.
	template <typename Resource>
	std::shared_ptr<Resource> ResolveCached(const std::string& key, ResourceCache<Resource>& cache);

	// Internal synchronous load wrapper shared by each format-specific
	// loader. The provided loader is only executed when the cache does not
	// already contain the requested asset.
	template <typename Resource>
	std::shared_ptr<Resource> LoadOrGetCached(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader);

	// Internal asynchronous wrapper that enqueues the loader on the shared
	// thread pool. If the resource already exists or is in-flight, the
	// existing shared_future is returned instead of scheduling new work.
	template <typename Resource>
	std::shared_future<std::shared_ptr<Resource>> LoadOrSchedule(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader);

	// Finalize a successful load by updating caches and clearing any
	// in-flight tracking entries. This runs on both synchronous and
	// asynchronous code paths to keep cache semantics consistent.
	template <typename Resource>
	std::shared_ptr<Resource> FinalizeLoad(const std::string& key, ResourceCache<Resource>& cache, std::shared_ptr<Resource> resource);

	// Collapse relative paths against the configured asset root so caching
	// keys remain stable even when callers run in different directories.
	std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const;

	// Produce a normalized string key used by the cache maps. Paths are
	// stored using generic separators to avoid Windows/Linux mismatches.
	std::string NormalizeKey(const std::filesystem::path& path) const;

	ThreadPool& threadPool;
	std::filesystem::path assetRoot;

	ResourceCache<TerraEngineMaterialFile> materialCache;
	ResourceCache<TerraEngineMeshFile> meshCache;
	ResourceCache<TerraEngineShaderFile> shaderCache;
	ResourceCache<TerraEngineTextureFile> textureCache;
};

template <typename Resource>
std::shared_future<std::shared_ptr<Resource>> ResourceManager::MakeReadyFuture(std::shared_ptr<Resource> resource)
{
	std::promise<std::shared_ptr<Resource>> promise;
	promise.set_value(std::move(resource));
	return promise.get_future().share();
}

template <typename Resource>
std::shared_ptr<Resource> ResourceManager::ResolveCached(const std::string& key, ResourceCache<Resource>& cache)
{
	std::shared_lock lock(cache.mutex);
	if (auto existing = cache.loaded.find(key); existing != cache.loaded.end())
	{
		if (auto locked = existing->second.lock())
		{
			return locked;
		}
	}

	if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
	{
		return pending->second.get();
	}

	return nullptr;
}

template <typename Resource>
std::shared_ptr<Resource> ResourceManager::FinalizeLoad(const std::string& key, ResourceCache<Resource>& cache, std::shared_ptr<Resource> resource)
{
	std::unique_lock lock(cache.mutex);
	if (resource)
	{
		cache.loaded[key] = resource;
	}
	cache.inFlight.erase(key);
	return resource;
}

template <typename Resource>
std::shared_ptr<Resource> ResourceManager::LoadOrGetCached(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);

	if (auto cached = ResolveCached(key, cache))
	{
		return cached;
	}

	const auto resource = loader(resolvedPath);
	return FinalizeLoad(key, cache, resource);
}

template <typename Resource>
std::shared_future<std::shared_ptr<Resource>> ResourceManager::LoadOrSchedule(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);

	{
		std::shared_lock lock(cache.mutex);
		if (auto existing = cache.loaded.find(key); existing != cache.loaded.end())
		{
			if (auto locked = existing->second.lock())
			{
				return MakeReadyFuture(locked);
			}
		}

		if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
		{
			return pending->second;
		}
	}

	// Schedule the expensive file I/O work on the thread pool so callers
	// can continue executing immediately.
	auto task = threadPool.createTask([this, key, resolvedPath, &cache, loader]() -> std::shared_ptr<Resource> {
		const auto resource = loader(resolvedPath);
		return FinalizeLoad(key, cache, resource);
	});

	auto future = task->getFuture().share();
	{
		std::unique_lock lock(cache.mutex);
		cache.inFlight[key] = future;
	}
	threadPool.enqueue(task);

	return future;
}
