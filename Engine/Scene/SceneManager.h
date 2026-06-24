// SceneManager.h — Scene Lifecycle Manager Definition
//
// Defines the SceneManager class, which oversees the loading, activation, and management of engine scenes.
// It maintains the active scene document and coordinates transitions between different scene states.
// The manager acts as the primary interface for scene-level operations, including asset resolution and document lifecycle.

#pragma once

#include <filesystem>
#include <functional>
#include <future>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include "Core/Export.h"
#include "Core/Object/Object.h"
#include "Core/SystemBase.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "Renderer/RenderTypes.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

class ResourceManager;
struct SkyComponent;
class Transform;

// Lightweight holder for a scene or prefab definition. The manager stores the
// original JSON text so higher level systems can deserialize into game-specific
// structures without reading from disk more than once.
struct SceneDocument
{
	std::filesystem::path sourcePath;
	std::string name;
	std::string jsonContents;
	// Cached parsed representation to avoid repeating JSON work in callers.
	nlohmann::json parsedJson;
	bool isPrefab = false;
};

// Centralized scene manager that handles loading authored .tescene and .teprefab
// JSON files, caches them by normalized path, and offers synchronous and
// asynchronous APIs backed by the shared thread pool. The class participates in
// the SystemsBase lifecycle so it can be ticked and cleaned up alongside other
// engine systems.
class TERRAENGINE_API SceneManager : public SystemsBase
{
public:
	static SceneManager& Instance();

	SceneManager(const SceneManager&) = delete;
	SceneManager& operator=(const SceneManager&) = delete;
	~SceneManager() override;

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	// Configure the root directory used to resolve relative scene and prefab
	// paths. This mirrors ResourceManager::SetAssetRoot so authored content can
	// live under a single asset tree.
	void SetSceneRoot(const std::filesystem::path& root);
	std::filesystem::path GetSceneRoot() const;

	// Sets the scene that should be loaded and activated during Initialize().
	// This is primarily used by exported builds to boot into the main menu or level.
	void SetStartupScene(const std::filesystem::path& path);
	std::filesystem::path GetStartupScene() const;

	// Synchronous load helpers. These block the calling thread until the JSON
	// file is read and lightly validated, but they reuse cached data if the
	// same path was requested earlier.
	TerraEngine::UUID LoadScene(const std::filesystem::path& path);
	TerraEngine::UUID LoadPrefab(const std::filesystem::path& path);

	// Asynchronous load helpers that schedule disk I/O on the shared thread
	// pool. Duplicate requests join the same future to avoid redundant work.
	std::shared_future<TerraEngine::UUID> LoadSceneAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadPrefabAsync(const std::filesystem::path& path);

	// Accessors let callers retrieve cached data by UUID instead of passing raw
	// filesystem paths around. Returned pointers remain valid until UnloadAll is
	// invoked or a future reload overwrites the entry.
	const SceneDocument* GetScene(const TerraEngine::UUID& id) const;
	const SceneDocument* GetPrefab(const TerraEngine::UUID& id) const;

	// Track a single active scene so gameplay systems can query which authored
	// file should be instantiated. The setter returns false when the requested
	// scene is not loaded yet.
	bool SetActiveScene(const TerraEngine::UUID& id);
	const SceneDocument* GetActiveScene() const;

	// Commits the current state of the ObjectManager to a .tescene file. If a path
	// is omitted, the document's original source location is overwritten. This 
	// triggers a dirty flag reset upon successful write.
	bool SaveScene(std::optional<std::filesystem::path> path = std::nullopt);

	// Scene render settings now cover only non-sky scene metadata. Sky authoring
	// lives on SkyComponent and is resolved from the active object graph.
	const SceneRenderSettings& GetActiveSceneRenderSettings() const;
	void SetActiveSceneRenderSettings(const SceneRenderSettings& settings);
	const SkyComponent* GetActiveSkyComponent() const;
	std::optional<Object> GetActiveSkyObject() const;
	bool HasActiveSkyComponent() const;
	void MarkActiveSceneSkyStateDirty();

	// Reports whether the active scene has received modifications since it was 
	// last saved or loaded.
	bool IsActiveSceneDirty() const;

	// Manually flag the active scene as containing unsaved changes. Should be 
	// called by editor systems after any mutation to the scene graph.
	void ForceMarkActiveSceneDirty();

	// Manually clear the dirty flag for the active scene.
	void ClearActiveSceneDirty();
	void ClearAllDirty();

	// Clear all cached documents and abandon in-flight futures. Use during
	// teardown to release memory when no other threads are reading scenes.
	void UnloadAll();

	// Scan the active scene and nullify any references to the given asset ID.
	void NullifyAssetReferences(const TerraEngine::UUID& assetId);

	// Spawns a deterministic point-light grid used for multithreaded
	// renderer stress tests and perf regression checks.
	void SpawnLightStressTestGrid(std::uint32_t gridWidth = 20u, std::uint32_t gridDepth = 20u, float spacing = 2.0f, float intensity = 25.0f);

	// Creates a dense clustered-light validation layout that intentionally
	// overlaps many point lights so at least one cluster exceeds legacy caps.
	void SpawnClusterBinDenseFixture();

	// Creates a sparse clustered-light validation layout with mostly empty
	// clusters so logical shared-list usage can be measured against reality.
	void SpawnClusterBinSparseFixture();

private:
	// SceneFileType names the supported states explicitly so serialization and switches stay readable.
	enum class SceneFileType
	{
		Scene,
		Prefab
	};

	SceneManager();

	template <typename Resource>
	// struct SceneCache groups related state and operations so callers have one documented contract.
	struct SceneCache
	{
		std::unordered_map<std::string, TerraEngine::UUID> pathToId;
		std::unordered_map<TerraEngine::UUID, std::unique_ptr<Resource>> resources;
		std::unordered_map<std::string, std::shared_future<TerraEngine::UUID>> inFlight;
		mutable std::shared_mutex mutex;
	};

	template <typename Resource>
	using LoaderFunction = std::function<std::unique_ptr<Resource>(const std::filesystem::path&, SceneFileType)>;

	std::shared_future<TerraEngine::UUID> MakeReadyFuture(const TerraEngine::UUID& id);

	template <typename Resource>
	std::optional<TerraEngine::UUID> ResolveCached(const std::string& key, SceneCache<Resource>& cache);

	template <typename Resource>
	TerraEngine::UUID EnsureResourceId(const std::string& key, SceneCache<Resource>& cache);

	template <typename Resource>
	const Resource* GetResourceById(const TerraEngine::UUID& id, const SceneCache<Resource>& cache) const;

	template <typename Resource>
	TerraEngine::UUID LoadOrGetCached(const std::filesystem::path& path, SceneCache<Resource>& cache, const LoaderFunction<Resource>& loader, SceneFileType type);

	template <typename Resource>
	std::shared_future<TerraEngine::UUID> LoadOrSchedule(const std::filesystem::path& path, SceneCache<Resource>& cache, const LoaderFunction<Resource>& loader, SceneFileType type);

	template <typename Resource>
	TerraEngine::UUID FinalizeLoad(const std::string& key, SceneCache<Resource>& cache, const TerraEngine::UUID& id, std::unique_ptr<Resource> resource);

	std::unique_ptr<SceneDocument> LoadSceneDocument(const std::filesystem::path& path, SceneFileType type);
	std::optional<std::string> ReadJsonFile(const std::filesystem::path& path, SceneFileType type) const;
	bool ValidateExtension(const std::filesystem::path& path, SceneFileType type) const;

	std::filesystem::path ResolveScenePath(const std::filesystem::path& path) const;
	std::string NormalizeKey(const std::filesystem::path& path) const;
	void PreparePreviewAssets();

	TerraEngine::UUID CreateGeneratedMaterial(ResourceManager& resources, const std::string& name, const TerraEngine::UUID& textureId, const TerraEngine::UUID& shaderCollectionId);
	void SpawnPreviewObject(const TerraEngine::UUID& meshId, const TerraEngine::UUID& materialId, const TerraEngine::UUID& textureId);
	void SpawnPreviewCamera();
	Transform* GetPreviewTransform();

	ThreadPool& threadPool;

	std::filesystem::path sceneRoot;
	std::filesystem::path m_StartupScene;

	SceneCache<SceneDocument> sceneCache;
	SceneCache<SceneDocument> prefabCache;

	TerraEngine::UUID activeSceneId = TerraEngine::EMPTY_UUID;

	bool activeSceneDirty = false;
	bool activeSceneSettingsDirty = false;
	SceneRenderSettings activeSceneRenderSettings = MakeDefaultSceneRenderSettings();

	TerraEngine::UUID previewMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID previewTextureId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID previewShaderCollectionId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID previewMaterialId = TerraEngine::EMPTY_UUID;

	// Track the generated preview object and the user's interaction state so
	// mouse motion/scroll can manipulate the on-screen demo model.
	std::optional<Object> previewObject;

	// Preview camera that replaces the hardcoded view/projection used by the
	// renderer so authored camera settings can be reflected into the frame.
	std::optional<Object> previewCamera;

	glm::vec2 lastCursorPosition{0.0f, 0.0f};
	bool isDraggingPreview = false;
	float accumulatedYaw = 0.0f;
	float accumulatedPitch = 0.0f;
	float previewZoomOffset = 0.0f;

	// Tunable interaction parameters keep drag/scroll sensitivity readable from
	// a single location instead of scattering literals through the update loop.
	static constexpr float kPreviewRotationSpeed = 0.005f;
	static constexpr float kPreviewZoomStep = 0.25f;
	static constexpr float kPreviewMaxZoomIn = 2.5f;
	static constexpr float kPreviewMaxZoomOut = -2.5f;
};

// SceneManager::MakeReadyFuture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline std::shared_future<TerraEngine::UUID> SceneManager::MakeReadyFuture(const TerraEngine::UUID& id)
{
	std::promise<TerraEngine::UUID> promise;
	promise.set_value(id);
	return promise.get_future().share();
}

template <typename Resource>
// SceneManager::ResolveCached is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<TerraEngine::UUID> SceneManager::ResolveCached(const std::string& key, SceneCache<Resource>& cache)
{
	std::shared_lock lock(cache.mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
	{
		const TerraEngine::UUID& id = existing->second;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cache.resources.find(id) != cache.resources.end())
		{
			return id;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
	{
		auto future = pending->second;
		lock.unlock();
		return future.get();
	}

	return std::nullopt;
}

template <typename Resource>
// SceneManager::EnsureResourceId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID SceneManager::EnsureResourceId(const std::string& key, SceneCache<Resource>& cache)
{
	std::unique_lock lock(cache.mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
	{
		return existing->second;
	}

	const auto id = TerraEngine::GenerateUUID();
	cache.pathToId.emplace(key, id);
	return id;
}

template <typename Resource>
// SceneManager::GetResourceById is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const Resource* SceneManager::GetResourceById(const TerraEngine::UUID& id, const SceneCache<Resource>& cache) const
{
	std::shared_lock lock(cache.mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto existing = cache.resources.find(id); existing != cache.resources.end())
	{
		return existing->second.get();
	}

	return nullptr;
}

template <typename Resource>
// SceneManager::FinalizeLoad is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID SceneManager::FinalizeLoad(const std::string& key, SceneCache<Resource>& cache, const TerraEngine::UUID& id, std::unique_ptr<Resource> resource)
{
	std::unique_lock lock(cache.mutex);
	cache.pathToId[key] = id;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
// SceneManager::LoadOrGetCached is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID SceneManager::LoadOrGetCached(const std::filesystem::path& path, SceneCache<Resource>& cache, const LoaderFunction<Resource>& loader, SceneFileType type)
{
	const std::filesystem::path resolvedPath = ResolveScenePath(path);
	const std::string key = NormalizeKey(resolvedPath);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto cached = ResolveCached(key, cache))
	{
		return *cached;
	}

	const TerraEngine::UUID id = EnsureResourceId(key, cache);
	auto resource = loader(resolvedPath, type);
	return FinalizeLoad(key, cache, id, std::move(resource));
}

template <typename Resource>
// SceneManager::LoadOrSchedule is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> SceneManager::LoadOrSchedule(const std::filesystem::path& path, SceneCache<Resource>& cache, const LoaderFunction<Resource>& loader, SceneFileType type)
{
	const std::filesystem::path resolvedPath = ResolveScenePath(path);
	const std::string key = NormalizeKey(resolvedPath);

	{
		std::shared_lock lock(cache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
		{
			if (cache.resources.find(existing->second) != cache.resources.end())
			{
				return MakeReadyFuture(existing->second);
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
		{
			return pending->second;
		}
	}

	const TerraEngine::UUID id = EnsureResourceId(key, cache);

	auto task = threadPool.createTask([this, key, resolvedPath, &cache, id, loader, type]() -> TerraEngine::UUID {
		auto resource = loader(resolvedPath, type);
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
