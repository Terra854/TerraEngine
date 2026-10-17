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
#include <mutex>
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
#include "Scene/SceneMutationQueue.h"
#include "Scene/BulkScenePublication.h"
#include "Scene/Serializer.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"
#include "Utilities/AtomicFileWrite.h"

class ResourceManager;
struct SkyComponent;
class Transform;

// Per-call checkpoints run without manager locks. They expose native load
// ordering to deterministic clients/tests without changing default requests.
enum class SceneLoadCheckpoint { Admission, Submission, BeforeRead, AfterInterpretation, Publication };
struct SceneLoadSourceSnapshot {
	std::filesystem::path absolutePath;
	std::filesystem::path root;
	std::string mountedLogicalPath;
	std::uint64_t requestId = 0;
	std::uint64_t generation = 0;
	bool isPrefab = false;
};
struct SceneLoadOptions {
	std::function<void(SceneLoadCheckpoint, const SceneLoadSourceSnapshot&)> checkpoint;
};
enum class SceneLoadDisposition {
	Pending, Published, InvalidDocument, Retired, ClosedAdmission,
	AdmissionFailure, SubmissionFailure, LoaderException, PublicationFailure
};
// Cumulative scalar outcomes plus current ownership keep diagnostics bounded.
// The newest request's disposition cannot be relabeled by an older completion.
struct SceneLoadDiagnostics {
	bool admissionOpen = true;
	std::uint64_t sceneGeneration = 0, prefabGeneration = 0;
	std::uint64_t latestRequestId = 0;
	SceneLoadDisposition latestDisposition = SceneLoadDisposition::Pending;
	std::size_t pendingScenes = 0, pendingPrefabs = 0, ownedLoads = 0;
	std::uint64_t admitted = 0, published = 0, invalid = 0, retired = 0;
	std::uint64_t closedAdmissions = 0, admissionFailures = 0, submissionFailures = 0;
	std::uint64_t loaderExceptions = 0, publicationFailures = 0, physicallySettled = 0;
};

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
	// ProjectManager may supply a validated derived/source preparation so scene
	// activation does not repeat the recursive object-table decode on the
	// protected editor boundary.
	std::optional<TerraEngine::Scene::PreparedSceneDecode> preparedScene;
	bool isPrefab = false;
	// A retained identity distinguishes a reloaded document even when its path
	// and UUID are reused. Save tickets never keep a raw document pointer.
	std::shared_ptr<const std::uint8_t> saveIdentity = std::make_shared<const std::uint8_t>(0);
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
	void PrepareShutdown() override;
	// Start an empty load lifetime without resource startup or scene activation.
	// Protected project/hub owners and isolated tests use this after retirement.
	void BeginLoadSession();
	// Snapshot terminal outcomes and current physical ownership without waiting
	// for workers; lifecycle owners can expose this in their diagnostics.
	SceneLoadDiagnostics GetLoadDiagnostics();
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
	// Explicit open/reload requests must read the current authoritative bytes even
	// when this path already has a resident document. This deliberately avoids a
	// timestamp heuristic: an external tool can replace a scene with the same
	// timestamp or byte length, and stale authored state must never be activated.
	TerraEngine::UUID ReloadScene(const std::filesystem::path& path);
	TerraEngine::UUID LoadPreparedScene(
		const std::filesystem::path& path,
		nlohmann::json sceneDocument,
		TerraEngine::Scene::PreparedSceneDecode preparedScene);
	TerraEngine::UUID LoadPrefab(const std::filesystem::path& path);
	TerraEngine::UUID LoadScene(const std::filesystem::path& path, SceneLoadOptions options);
	TerraEngine::UUID ReloadScene(const std::filesystem::path& path, SceneLoadOptions options);
	TerraEngine::UUID LoadPrefab(const std::filesystem::path& path, SceneLoadOptions options);

	// Asynchronous load helpers that schedule disk I/O on the shared thread
	// pool. Duplicate requests join the same future to avoid redundant work.
	std::shared_future<TerraEngine::UUID> LoadSceneAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadPrefabAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadSceneAsync(const std::filesystem::path& path, SceneLoadOptions options);
	std::shared_future<TerraEngine::UUID> LoadPrefabAsync(const std::filesystem::path& path, SceneLoadOptions options);

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
	TerraEngine::UUID GetActiveSceneId() const { return activeSceneId; }
	std::vector<TerraEngine::UUID> GetLoadedSceneIds() const;

	// Commits the current state of the ObjectManager to a .tescene file. If a path
	// is omitted, the document's original source location is overwritten. This 
	// triggers a dirty flag reset upon successful write.
	bool SaveScene(std::optional<std::filesystem::path> path = std::nullopt);
	// Native filesystem checkpoints exercise actual staging failures per call.
	bool SaveScene(std::optional<std::filesystem::path> path, const TerraEngine::Utilities::AtomicFileWriteOptions& options);
	const TerraEngine::Utilities::AtomicFileWriteResult& GetLastSaveResult() const { return lastSaveResult; }
	// Editor automation uses these filesystem-only helpers during preflight and
	// protected lifecycle commits. Callers must pass canonical project-contained
	// paths; the manager preserves source files unless the requested operation is
	// explicitly a rename.
	std::vector<std::filesystem::path> ListSceneFiles(const std::filesystem::path& projectRoot) const;
	bool CreateSceneFile(const std::filesystem::path& path, std::string_view name, std::string* outError = nullptr);
	bool DuplicateSceneFile(const std::filesystem::path& source, const std::filesystem::path& destination, std::string* outError = nullptr);
	bool RenameSceneFile(const std::filesystem::path& source, const std::filesystem::path& destination, std::string* outError = nullptr);

	// Scene render settings now cover only non-sky scene metadata. Sky authoring
	// lives on SkyComponent and is resolved from the active object graph.
	const SceneRenderSettings& GetActiveSceneRenderSettings() const;
	void SetActiveSceneRenderSettings(const SceneRenderSettings& settings);
	const SkyComponent* GetActiveSkyComponent() const;
	std::optional<entt::entity> GetActiveSkyObject() const;
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
	TerraEngine::Scene::TransactionDiagnostics GetTransactionDiagnostics() const;
	TerraEngine::Scene::TransactionResult PreflightTransaction(const TerraEngine::Scene::Transaction& transaction) const;
	TerraEngine::Scene::SceneMutationBatchResult ApplySceneMutations(std::string commitName = {});
	// Region streaming hands this method a fully prepared value packet. The
	// actual ObjectManager materialization is intentionally retained at the
	// protected scene boundary; workers never receive a live manager reference.
	TerraEngine::Scene::BulkScenePublicationResult PublishPreparedRegionPacket(
		const TerraEngine::Scene::BulkScenePublicationPacket& packet,
		const std::function<bool()>& cancellationRequested = {});

	// Clear visible documents and revoke pending publication authority. Physical
	// readers remain owned until completion; PrepareShutdown closes and drains
	// them before their dependencies or mounted sources are released.
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
	struct LoadRequest {
		SceneLoadSourceSnapshot source;
		std::string key;
		TerraEngine::UUID resourceId = TerraEngine::EMPTY_UUID;
		SceneLoadOptions options;
		std::promise<TerraEngine::UUID> completion;
		std::shared_future<TerraEngine::UUID> future = completion.get_future().share();
		// Public completion can precede the task's final return. This separate
		// future proves the worker physically stopped before dependencies release.
		std::shared_future<bool> physicalCompletion;
		SceneLoadDisposition disposition = SceneLoadDisposition::Pending;
	};

	template <typename Resource>

	struct SceneCache
	{
		std::unordered_map<std::string, TerraEngine::UUID> pathToId;
		std::unordered_map<TerraEngine::UUID, std::unique_ptr<Resource>> resources;
		std::unordered_map<std::string, std::shared_ptr<LoadRequest>> inFlight;
		std::uint64_t generation = 1;
		mutable std::shared_mutex mutex;
	};

	std::shared_future<TerraEngine::UUID> MakeReadyFuture(const TerraEngine::UUID& id);

	template <typename Resource>
	const Resource* GetResourceById(const TerraEngine::UUID& id, const SceneCache<Resource>& cache) const;

	std::shared_future<TerraEngine::UUID> RequestLoad(const std::filesystem::path& path,
		SceneCache<SceneDocument>& cache, SceneFileType type, bool reload, bool asynchronous, SceneLoadOptions options);
	void ExecuteLoad(const std::shared_ptr<LoadRequest>& request, SceneCache<SceneDocument>& cache, SceneFileType type);
	TerraEngine::UUID PublishLoad(const std::shared_ptr<LoadRequest>& request, SceneCache<SceneDocument>& cache, std::unique_ptr<SceneDocument> resource);
	void CleanupLoad(const std::shared_ptr<LoadRequest>& request, SceneCache<SceneDocument>& cache) noexcept;
	void ReportLoadTerminal(const std::shared_ptr<LoadRequest>& request, SceneLoadDisposition disposition) noexcept;
	void RecordLoadTerminalLocked(const std::shared_ptr<LoadRequest>& request, SceneLoadDisposition disposition) noexcept;
	void PruneSettledLoadsLocked();
	void RetireCacheAuthorityLocked(bool clearDocuments);
	void CloseAndDrainLoads();
	std::unique_ptr<SceneDocument> LoadSceneDocument(const SceneLoadSourceSnapshot& source, SceneFileType type);
	std::optional<std::string> ReadJsonFile(const SceneLoadSourceSnapshot& source, SceneFileType type) const;
	bool ValidateExtension(const std::filesystem::path& path, SceneFileType type) const;

	std::filesystem::path ResolveScenePath(const std::filesystem::path& path) const;
	std::string NormalizeKey(const std::filesystem::path& path) const;
	void PreparePreviewAssets();

	TerraEngine::UUID CreateGeneratedMaterial(ResourceManager& resources, const std::string& name, const TerraEngine::UUID& textureId, const TerraEngine::UUID& shaderCollectionId);
	void SpawnPreviewObject(const TerraEngine::UUID& meshId, const TerraEngine::UUID& materialId, const TerraEngine::UUID& textureId);
	void SpawnPreviewCamera();
	Transform* GetPreviewTransform();

	ThreadPool& threadPool;
	// Admission/root authority always precedes a cache lock. Parsing and future
	// waits hold neither, allowing completion and explicit reload to progress.
	mutable std::mutex lifecycleMutex;
	// Only lifecycle callers take this mutex; completion never needs it. It
	// serializes reopen/drain while allowing waits outside the authority gate.
	std::mutex loadSessionTransitionMutex;
	bool loadAdmissionOpen = true;
	std::uint64_t nextLoadRequestId = 1;
	std::vector<std::shared_ptr<LoadRequest>> outstandingLoads;
	std::size_t drainingLoadCount = 0;
	SceneLoadDiagnostics loadDiagnostics;

	std::filesystem::path sceneRoot;
	std::filesystem::path m_StartupScene;

	SceneCache<SceneDocument> sceneCache;
	SceneCache<SceneDocument> prefabCache;

	TerraEngine::UUID activeSceneId = TerraEngine::EMPTY_UUID;

	bool activeSceneDirty = false;
	bool activeSceneSettingsDirty = false;
	// Protected authoring entry points advance this token, including edits to
	// scene settings which are outside ObjectManager's component revisions.
	std::uint64_t activeSceneAuthoredRevision = 0;
	TerraEngine::Utilities::AtomicFileWriteResult lastSaveResult;
	SceneRenderSettings activeSceneRenderSettings = MakeDefaultSceneRenderSettings();

	TerraEngine::UUID previewMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID previewTextureId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID previewShaderCollectionId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID previewMaterialId = TerraEngine::EMPTY_UUID;

	// Track the generated preview object and the user's interaction state so
	// mouse motion/scroll can manipulate the on-screen demo model.
	std::optional<entt::entity> previewObject;

	// Preview camera that replaces the hardcoded view/projection used by the
	// renderer so authored camera settings can be reflected into the frame.
	std::optional<entt::entity> previewCamera;

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


inline std::shared_future<TerraEngine::UUID> SceneManager::MakeReadyFuture(const TerraEngine::UUID& id)
{
	std::promise<TerraEngine::UUID> promise;
	promise.set_value(id);
	return promise.get_future().share();
}

template <typename Resource>

const Resource* SceneManager::GetResourceById(const TerraEngine::UUID& id, const SceneCache<Resource>& cache) const
{
	std::shared_lock lock(cache.mutex);

	if (auto existing = cache.resources.find(id); existing != cache.resources.end())
	{
		return existing->second.get();
	}

	return nullptr;
}
