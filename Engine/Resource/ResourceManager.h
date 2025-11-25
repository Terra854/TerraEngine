#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <mutex>
#include <utility>
#include <vector>

#include "Core/Export.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

// Forward declarations keep this header lightweight while avoiding the
// kMagicValue naming collisions in the file format headers. Individual
// loader translation units include the concrete definitions they need.
struct Material;
struct Mesh;
struct Shader;
struct Texture;

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
	TerraEngine::UUID LoadMaterial(const std::filesystem::path& path);
	TerraEngine::UUID LoadMesh(const std::filesystem::path& path);
	TerraEngine::UUID LoadShader(const std::filesystem::path& path);
	TerraEngine::UUID LoadTexture(const std::filesystem::path& path);

	// Asynchronous load helpers. Each call schedules a background task on
	// the shared thread pool and returns a shared_future that multiple
	// callers can await safely. Duplicate requests will join the same
	// in-flight future so disk I/O is never duplicated.
	std::shared_future<TerraEngine::UUID> LoadMaterialAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadMeshAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadShaderAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadTextureAsync(const std::filesystem::path& path);

	// Fetch a loaded resource using its UUID so callers can pass lightweight
	// identifiers around without holding shared ownership. Returned pointers
	// are non-owning views into the cache and remain valid until ClearCaches
	// or a subsequent reload replaces the resource.
	Material* GetMaterial(const TerraEngine::UUID& id);
	Mesh* GetMesh(const TerraEngine::UUID& id);
	Shader* GetShader(const TerraEngine::UUID& id);
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
	Resource* GetResourceById(const TerraEngine::UUID& id, ResourceCache<Resource>& cache);

	// Internal synchronous load wrapper shared by each format-specific
	// loader. The provided loader is only executed when the cache does not
	// already contain the requested asset.
	template <typename Resource>
	TerraEngine::UUID LoadOrGetCached(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader);

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

	ThreadPool& threadPool;
	std::filesystem::path assetRoot;

	ResourceCache<Material> materialCache;
	ResourceCache<Mesh> meshCache;
	ResourceCache<Shader> shaderCache;
	ResourceCache<Texture> textureCache;
};

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
			return id;
		}
	}

	if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
	{
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
	return id;
}

template <typename Resource>
Resource* ResourceManager::GetResourceById(const TerraEngine::UUID& id, ResourceCache<Resource>& cache)
{
	std::shared_lock lock(cache.mutex);
	if (auto existing = cache.resources.find(id); existing != cache.resources.end())
	{
		return existing->second.get();
	}

	return nullptr;
}

template <typename Resource>
TerraEngine::UUID ResourceManager::FinalizeLoad(const std::string& key, ResourceCache<Resource>& cache, const TerraEngine::UUID& id, std::unique_ptr<Resource> resource)
{
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
	const auto resource = loader(resolvedPath);
	return FinalizeLoad(key, cache, id, resource);
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
	auto task = threadPool.createTask([this, key, resolvedPath, &cache, id, loader]() -> TerraEngine::UUID {
		const auto resource = loader(resolvedPath);
		return FinalizeLoad(key, cache, id, resource);
	});

	auto future = task->getFuture().share();
	{
		std::unique_lock lock(cache.mutex);
		cache.inFlight[key] = future;
	}
	threadPool.enqueue(task);

	return future;
}
