// ResourceManager.h — Resource Manager System Definition
//
// Defines the ResourceManager class, the central hub for managing the engine's asset lifecycle.
// It handles loading, caching, and unloading of various resource types, including meshes, textures, and shaders.
// The system ensures efficient memory utilization through deduplication and provides a unified interface for asset access.

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
#include "Renderer/RenderTypes.h"
#include "Renderer/RendererRecoveryPolicy.h"
#include "Scene/SpatialBounds.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

// Forward declarations keep this header lightweight and avoid forcing every
// includer to pull the binary format structs; the concrete loaders include
// the definitions they need inside their translation units.
struct Material;
struct Mesh;
struct Shader;
// struct ShaderCollection groups related state and operations so callers have one documented contract.
struct ShaderCollection;
// struct Texture groups related state and operations so callers have one documented contract.
struct Texture;
// ShaderStage names the supported states explicitly so serialization and switches stay readable.
enum class ShaderStage : std::uint32_t;

enum class AssetType : std::uint32_t {
	Mesh = 0U,
	Texture = 1U,
	Material = 2U,
	ShaderCollection = 3U,
	// Append-only serialized identities for physics project assets. These have
	// no generic render-resource cache because physics owns their typed runtime
	// loading; the values still let paths and tooling preserve asset identity.
	PhysicsMaterial = 4U,
	CookedCollider = 5U
};

#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"

// struct ShaderStagePath groups related state and operations so callers have one documented contract.
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

	// Resolve the authored project directory that owns the current asset root.
	// Editor sessions commonly point assetRoot at <project>/Assets, while
	// project-level data such as ProjectSettings and game.json live beside it.
	std::filesystem::path GetProjectRoot() const;

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
	std::optional<std::string> GetLastStartupError() const;

	// Returns the manifest-backed product metadata that Application.cs exposes to
	// gameplay scripts. These values come from project-root or export game.json.
	std::string GetProductName() const;
	std::string GetVersion() const;

	// Returns the resolved render profile summary read from exported metadata.
	std::string GetRenderProfileSummary() const;
	bool IsRenderValidationEnabled() const;
	bool IsRenderDebugEnabled() const;
	void SetRenderValidationEnabled(bool enabled);
	void SetRenderDebugEnabled(bool enabled);
	TerraEngine::Renderer::RendererRecoveryPolicy GetRendererRecoveryPolicy() const;
	std::optional<RenderSettings> GetConfiguredRenderSettings() const;
	// Editor bootstrap resolves this read-only profile before Engine::Run. It is
	// reapplied by Initialize instead of being erased by session-default resets.
	void ConfigureEditorStartupProfile(const RenderSettings& settings, bool validationEnabled,
		bool debugEnabled, std::optional<std::filesystem::path> projectRoot, std::string origin);
	[[nodiscard]] bool HasEditorStartupProfile() const noexcept { return m_EditorStartupProfileConfigured; }

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

	// Configure how loaded RAM/VRAM resources leave residency. This policy is
	// intentionally about live cache contents only; persistent Vulkan or shader
	// disk caches are backend-owned and stay outside this resource manager.
	void SetResidencyPolicy(const TerraEngine::Resource::ResidencyPolicy& policy);
	TerraEngine::Resource::ResidencyPolicy GetResidencyPolicy() const;
	void SetResidencyDiagnosticsEnabled(bool enabled);
	TerraEngine::Resource::ResidencyDiagnosticsSummary GetResidencyDiagnosticsSummary() const;
	bool IsResourceResident(const TerraEngine::UUID& id) const;
	void TouchResource(
		const TerraEngine::UUID& id,
		AssetType type,
		std::uint64_t frameId,
		TerraEngine::Resource::ResidencyPinReason reason = TerraEngine::Resource::ResidencyPinReason::None,
		std::string_view detail = {});
	void PinResource(
		const TerraEngine::UUID& id,
		AssetType type,
		TerraEngine::Resource::ResidencyPinReason reason,
		std::string_view detail = {});
	void UnpinResource(const TerraEngine::UUID& id);
	// MarkResourceUploadedToGpu is called by render backends after they create
	// real GPU buffers/images for an asset. ResourceManager owns the user-facing
	// diagnostics, so this keeps RAM-only load state separate from VRAM state.
	void MarkResourceUploadedToGpu(
		const TerraEngine::UUID& id,
		AssetType type,
		std::uint64_t frameId,
		std::uint64_t gpuBytes,
		std::string_view detail = {},
		std::uint32_t textureResidentMipCount = 0u,
		std::uint32_t textureSourceMipCount = 0u,
		std::uint32_t textureResidentMipOffset = 0u);
	TerraEngine::Resource::ResidencySnapshot BuildResidencySnapshot(std::uint64_t frameId) const;
	std::vector<TerraEngine::Resource::ReleaseRecord> RunResidencyCleanup(std::uint64_t frameId);

	// Configure visibility streaming. Streaming decides which registered scene
	// payloads are needed; residency cleanup still owns the actual RAM/VRAM
	// release once streaming marks an asset eligible.
	void SetStreamingPolicy(const TerraEngine::Resource::StreamingPolicy& policy);
	TerraEngine::Resource::StreamingPolicy GetStreamingPolicy() const;
	void SetStreamingDiagnosticsEnabled(bool enabled);
	TerraEngine::Resource::StreamingDiagnosticsSummary GetStreamingDiagnosticsSummary() const;
	TerraEngine::Resource::StreamableAssetRecord RegisterStreamableAsset(
		const TerraEngine::UUID& id,
		AssetType type,
		const std::filesystem::path& path,
		bool referencedByScene = true);
	std::optional<TerraEngine::Resource::StreamableAssetRecord> GetStreamableAsset(const TerraEngine::UUID& id) const;
	void SetStreamableAssetPolicy(
		const TerraEngine::UUID& id,
		AssetType type,
		const TerraEngine::Resource::StreamingAssetPolicy& policy);
	TerraEngine::Resource::StreamingAssetPolicy GetStreamableAssetPolicy(const TerraEngine::UUID& id, AssetType type) const;
	void MarkTextureStreamingDistance(
		const TerraEngine::UUID& id,
		double viewDistance,
		std::uint64_t frameId,
		std::string_view reason = {});
	TerraEngine::Resource::TextureMipResidency ResolveTextureMipResidencyForTexture(
		const TerraEngine::UUID& id,
		std::uint32_t sourceMipCount) const;
	void MarkStreamingNeed(
		const TerraEngine::UUID& id,
		AssetType type,
		TerraEngine::Resource::StreamingNeedType needType,
		std::uint64_t frameId,
		std::string_view reason = {},
		std::int32_t priority = 0);
	void PublishVisibleStreamingTouches(
		const TerraEngine::Renderer::VisibleStreamingTouchSet& touchSet,
		std::uint64_t frameId,
		std::string_view reason = {});
	std::shared_future<TerraEngine::UUID> RequestStreamableLoad(
		const TerraEngine::UUID& id,
		AssetType type,
		TerraEngine::Resource::StreamingNeedType needType,
		std::uint64_t frameId,
		std::string_view reason = {});
	// Explicit managed/runtime unloads are executed only at the engine's protected
	// frame-end boundary. Persistent asset registrations remain intact, so a
	// later preload can safely recreate the released CPU/GPU payload.
	bool ReleaseStreamableAssetForRuntime(const TerraEngine::UUID& id, AssetType type, std::uint64_t frameId);

	// Check if a resource with the given UUID exists in any of the manager's caches.
	bool HasResource(const TerraEngine::UUID& id) const;

	// Register a mapping between a UUID and a filesystem path so that GetMesh/GetTexture
	// can trigger on-demand loading if the resource is not yet in the cache.
	void RegisterAssetPath(const TerraEngine::UUID& id, const std::filesystem::path& path, AssetType type);
	// Physics owns typed decoding and immutable publication, but still needs the
	// central GUID registry to resolve a cooked/material asset without a path in
	// scene data. The returned path is the exact registered runtime artifact.
	std::filesystem::path GetRegisteredAssetPath(const TerraEngine::UUID& id, AssetType type) const;
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
	TerraEngine::Scene::SpatialAABB GetMeshLocalBounds(const TerraEngine::UUID& id);
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

	// Collapse relative paths against the configured project asset root. Export
	// runtimes use the archive folder as asset root so the result can still map
	// into TEPAK when ReadFileBytes probes the active ContentMount.
	std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const;

	// Collapse engine-owned runtime paths against the installed engine root.
	// Editor builds resolve these to loose files; export builds resolve them to
	// archive-relative paths because engineRoot is the export runtime folder.
	std::filesystem::path ResolveEngineAssetPath(const std::filesystem::path& path) const;

private:
	ResourceManager();

	template <typename Resource>
	// struct ResourceCache groups related state and operations so callers have one documented contract.
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
		// Residency metadata lets the generic loader path report the resource
		// type without each format-specific loader duplicating registration code.
		AssetType assetType = AssetType::Material;
		bool tracksResidency = false;
		mutable std::shared_mutex mutex;
	};

	struct AssetPathRegistry
	{
		std::unordered_map<std::string, TerraEngine::UUID> pathToId;
		std::unordered_map<TerraEngine::UUID, std::string> idToPath;
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

	// RegisterResidencyResource records a loaded cache entry after parsing
	// succeeds. The source key is kept even after release so future references
	// can reload the same asset without changing the stable UUID.
	void RegisterResidencyResource(
		const TerraEngine::UUID& id,
		AssetType type,
		const std::string& sourceKey,
		std::uint64_t cpuBytes,
		std::uint64_t gpuBytes,
		bool reloadable,
		TerraEngine::Resource::ResidencyState state);
	void CacheLoadedMeshBounds(const TerraEngine::UUID& id, const Mesh* mesh);
	void CacheLoadedMeshBounds(const TerraEngine::UUID& id, const void* resource);
	TerraEngine::Resource::StreamingAssetPolicy ResolveStreamingAssetPolicy(const TerraEngine::UUID& id, AssetType type) const;

	// EstimateLoadedResourceBytes uses conservative fallback byte counts because
	// residency cleanup only needs deterministic ordering before specialized
	// per-format accounting is available.
	template <typename Resource>
	std::uint64_t EstimateLoadedResourceBytes(const std::unique_ptr<Resource>& resource) const;
	// ComputeLoadedResourceBytes reports the exact CPU-owned payload bytes kept
	// alive by each cache entry. The generic fallback is only for resource types
	// that do not publish a dedicated payload structure yet; concrete formats
	// below count their owned vectors so Resource Residency never displays an
	// estimate in the Stream CPU Bytes column.
	template <typename Resource>
	std::uint64_t ComputeLoadedResourceBytes(const Resource& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const Material& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const Mesh& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const Shader& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const ShaderCollection& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const Texture& resource) const;

	bool ReleaseResidentResource(
		const TerraEngine::UUID& id,
		AssetType type,
		TerraEngine::Resource::ResidencyReleaseReason reason,
		TerraEngine::Resource::ResidencyReleaseDomain domain,
		std::uint64_t frameId,
		std::uint64_t candidateBytes);

	void ClearResidencyState();
	void ClearStreamingState();
	void UpdateStreamableLoadResult(
		const TerraEngine::UUID& id,
		AssetType type,
		const std::string& sourceKey,
		bool success,
		std::uint64_t cpuBytes,
		std::uint64_t gpuBytes,
		std::string_view failureReason = {});
	void MarkStreamableReleased(const TerraEngine::UUID& id, std::uint64_t frameId);
	TerraEngine::Resource::StreamingDiagnosticsSummary BuildStreamingDiagnosticsLocked(std::uint64_t frameId) const;

	ThreadPool& threadPool;
	std::filesystem::path assetRoot;
	std::filesystem::path engineRoot;
	std::optional<std::filesystem::path> m_ConfiguredStartupScene;
	std::optional<std::filesystem::path> m_ConfiguredExportArchivePath;
	std::optional<std::string> m_LastStartupError;
	std::optional<RenderSettings> m_ConfiguredRenderSettings;
	bool m_EditorStartupProfileConfigured = false;
	RenderSettings m_EditorStartupRenderSettings{};
	bool m_EditorStartupValidationEnabled = false;
	bool m_EditorStartupDebugEnabled = false;
	std::optional<std::filesystem::path> m_EditorStartupProjectRoot;
	std::string m_EditorStartupOrigin = "HubDefaults";
	std::string m_RenderProfileSummary = "profile=Default";
	bool m_RenderValidationEnabled = false;
	bool m_RenderDebugEnabled = false;
	TerraEngine::Renderer::RendererRecoveryPolicy m_RendererRecoveryPolicy = TerraEngine::Renderer::MakeEditorRendererRecoveryPolicy();
	std::string m_ProductName = "TerraEngine";
	std::string m_Version = "1.0.0";
	TerraEngine::Resource::ResidencyPolicy m_ResidencyPolicy;
	TerraEngine::Resource::ResidencyDiagnosticsSummary m_ResidencyDiagnostics;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::ResidentAsset> m_ResidentAssets;
	std::unordered_map<TerraEngine::UUID, AssetType> m_ResidentAssetTypes;
	mutable std::shared_mutex m_ResidencyMutex;
	std::uint64_t m_LastResidencyCleanupFrame = 0u;
	double m_LastResidencyCleanupSeconds = 0.0;
	TerraEngine::Resource::StreamingPolicy m_StreamingPolicy;
	TerraEngine::Resource::StreamingDiagnosticsSummary m_StreamingDiagnostics;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::StreamableAssetRecord> m_StreamableAssets;
	mutable std::shared_mutex m_StreamingMutex;
	std::uint64_t m_StreamingLoadBudgetFrame = 0u;
	std::uint32_t m_StreamingLoadsIssuedThisFrame = 0u;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB> m_MeshLocalBoundsCache;
	mutable std::shared_mutex m_MeshLocalBoundsMutex;

	ResourceCache<Material> materialCache;
	ResourceCache<Mesh> meshCache;
	ResourceCache<Shader> shaderCache;
	ResourceCache<ShaderCollection> shaderCollectionCache;
	ResourceCache<Texture> textureCache;
	AssetPathRegistry physicsMaterialPaths;
	AssetPathRegistry cookedColliderPaths;
};

// Render a UUID as a hex string for readable debug logging without adding a
// dedicated conversion helper elsewhere in the engine.
inline std::string ToHexString(const TerraEngine::UUID& id)
{
	return TerraEngine::ToHexString(id);
}

// ResourceManager::MakeReadyFuture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline std::shared_future<TerraEngine::UUID> ResourceManager::MakeReadyFuture(const TerraEngine::UUID& id)
{
	std::promise<TerraEngine::UUID> promise;
	promise.set_value(id);
	return promise.get_future().share();
}

template <typename Resource>
std::uint64_t ResourceManager::EstimateLoadedResourceBytes(const std::unique_ptr<Resource>& resource) const
{
	return resource ? ComputeLoadedResourceBytes(*resource) : 0u;
}

template <typename Resource>
std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const Resource&) const
{
	// Unknown resource kinds still need a deterministic value for cleanup
	// ordering, but concrete TerraEngine asset formats override this so the
	// editor-visible byte columns remain exact for meshes, textures, materials,
	// and shader collections.
	return static_cast<std::uint64_t>(sizeof(Resource));
}

template <typename Resource>
// ResourceManager::ResolveCached is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<TerraEngine::UUID> ResourceManager::ResolveCached(const std::string& key, ResourceCache<Resource>& cache)
{
	std::shared_lock lock(cache.mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
	{
		const TerraEngine::UUID& id = existing->second;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cache.resources.find(id) != cache.resources.end())
		{
			// Debug log helps trace cache reuse when resources are requested repeatedly.
			TERRAENGINE_LOG(LogLevel::Debug, "Resource cache hit for " << key << " (id " << ToHexString(id) << ")");
			UpdateStreamableLoadResult(
				id,
				cache.assetType,
				key,
				true,
				cache.tracksResidency ? EstimateLoadedResourceBytes(cache.resources.at(id)) : 0u,
				0u);
			return id;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
// ResourceManager::EnsureResourceId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::EnsureResourceId(const std::string& key, ResourceCache<Resource>& cache)
{
	std::unique_lock lock(cache.mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
// ResourceManager::GetResourceById is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Resource* ResourceManager::GetResourceById(const TerraEngine::UUID& id, ResourceCache<Resource>& cache, const std::function<TerraEngine::UUID(const std::filesystem::path&)>& loader)
{
	{
		std::shared_lock lock(cache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto existing = cache.resources.find(id); existing != cache.resources.end())
		{
			UpdateStreamableLoadResult(
				id,
				cache.assetType,
				cache.idToPath.contains(id) ? cache.idToPath.at(id) : std::string{},
				true,
				cache.tracksResidency ? EstimateLoadedResourceBytes(existing->second) : 0u,
				0u);
			return existing->second.get();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (loader)
		{
			if (auto it = cache.idToPath.find(id); it != cache.idToPath.end())
			{
				const std::string path = it->second;
				lock.unlock(); // Must release shared lock before triggering a load which acquires unique lock
				const TerraEngine::UUID loadedId = loader(path);
				
				// Re-acquire shared lock and return from cache
				std::shared_lock relock(cache.mutex);
				if (auto reloaded = cache.resources.find(id); reloaded != cache.resources.end())
				{
					return reloaded->second.get();
				}
				// Package imports can expose several stable child GUIDs that all
				// reference one cooked runtime file. The path cache has a single
				// canonical UUID for that file, so a load requested through an older
				// sibling GUID may populate the canonical entry instead of the exact
				// requested id. Returning the canonical payload keeps every registered
				// GUID renderable while preserving the source GUID stored on components.
				if (loadedId != TerraEngine::EMPTY_UUID && loadedId != id)
				{
					if (auto aliased = cache.resources.find(loadedId); aliased != cache.resources.end())
					{
						CacheLoadedMeshBounds(id, aliased->second.get());
						UpdateStreamableLoadResult(
							id,
							cache.assetType,
							path,
							true,
							cache.tracksResidency ? EstimateLoadedResourceBytes(aliased->second) : 0u,
							0u);
						return aliased->second.get();
					}
				}
			}
		}
	}

	return nullptr;
}

template <typename Resource>
// ResourceManager::FinalizeLoad is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::FinalizeLoad(const std::string& key, ResourceCache<Resource>& cache, const TerraEngine::UUID& id, std::unique_ptr<Resource> resource)
{
	// Capture success before we move the resource into the cache so logging reflects the actual loader result.
	const bool loadedSuccessfully = static_cast<bool>(resource);

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
	// Log the outcome to tie disk/parsing work back to a UUID for later lookups.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (loadedSuccessfully)
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Finalized load for " << key << " (id " << ToHexString(id) << ")");
		CacheLoadedMeshBounds(id, cache.resources[id].get());
		if (cache.tracksResidency)
		{
			RegisterResidencyResource(
				id,
				cache.assetType,
				key,
				EstimateLoadedResourceBytes(cache.resources[id]),
				0u,
				true,
				TerraEngine::Resource::ResidencyState::Active);
		}
	}
	else
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Load failed for " << key << "; cache entry cleared for id " << ToHexString(id));
	}
	UpdateStreamableLoadResult(
		id,
		cache.assetType,
		key,
		loadedSuccessfully,
		loadedSuccessfully && cache.tracksResidency ? EstimateLoadedResourceBytes(cache.resources[id]) : 0u,
		0u,
		loadedSuccessfully ? std::string_view{} : std::string_view("loader returned no payload"));
	return id;
}

template <typename Resource>
// ResourceManager::LoadOrGetCached is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::LoadOrGetCached(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
// ResourceManager::VisitAssetCache is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
// ResourceManager::LoadOrSchedule is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> ResourceManager::LoadOrSchedule(const std::filesystem::path& path, ResourceCache<Resource>& cache, const LoaderFunction<Resource>& loader)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);

	{
		std::shared_lock lock(cache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
		{
			if (cache.resources.find(existing->second) != cache.resources.end())
			{
				UpdateStreamableLoadResult(
					existing->second,
					cache.assetType,
					key,
					true,
					cache.tracksResidency ? EstimateLoadedResourceBytes(cache.resources.at(existing->second)) : 0u,
					0u);
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
