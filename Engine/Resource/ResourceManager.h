// ResourceManager.h — Resource Manager System Definition
//
// Defines the ResourceManager class, the central hub for managing the engine's asset lifecycle.
// It handles loading, caching, and unloading of various resource types, including meshes, textures, and shaders.
// The system ensures efficient memory utilization through deduplication and provides a unified interface for asset access.

#pragma once

#include <atomic>
#include <array>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <span>
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
#include "Animation/AnimationTypes.h"
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
namespace TerraEngine::Animation
{
	struct AnimationClipAsset;
	struct AnimationControllerAsset;
	struct AnimationMaskAsset;
	struct MotionDatabaseAsset;
	struct RagdollProfileAsset;
	struct RetargetProfileAsset;
	struct SkeletonAsset;
}
namespace TerraEngine::Audio
{
	class AudioClipAsset;
}
namespace TerraEngine::Navigation
{
	class NavigationGeometrySnapshot;
	struct NavigationGeometrySnapshotDescriptor;
}
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
	CookedCollider = 5U,
	// Animation assets append after every pre-existing value. These identities
	// are persisted by scenes, import manifests, resource diagnostics, and later
	// TEPAK dependency records, so they must never be reordered or reused.
	Skeleton = 6U,
	AnimationClip = 7U,
	AnimationController = 8U,
	AnimationMask = 9U,
	RetargetProfile = 10U,
	MotionDatabase = 11U,
	RagdollProfile = 12U,
	// Audio follows persisted animation identities so existing scenes and asset
	// indexes retain their numeric meanings when audio support is introduced.
	AudioClip = 13U,
	// Terrain keeps one persisted identity while TerrainAssetStore owns its areas.
	Terrain = 14U
};

constexpr bool IsAnimationAssetType(AssetType type) noexcept
{
	return type >= AssetType::Skeleton && type <= AssetType::RagdollProfile;
}

constexpr bool IsRendererResourceAssetType(AssetType type) noexcept
{
	// These persisted asset identities can create or replace backend draw state.
	// Their first successful load must invalidate warm renderer caches just like
	// a later hot reload; physics, animation, audio, and terrain publish through
	// their own typed runtime boundaries instead.
	return type == AssetType::Mesh
		|| type == AssetType::Texture
		|| type == AssetType::Material
		|| type == AssetType::ShaderCollection;
}

// Validation commands return structured evidence instead of only logging a
// parser failure. Tooling can display diagnosticCode/message directly, while
// runtime callers can branch on the stable AnimationStatus value.
struct AnimationResourceValidationResult
{
	TerraEngine::Animation::AnimationStatus status = TerraEngine::Animation::AnimationStatus::InvalidArgument;
	AssetType assetType = AssetType::Skeleton;
	// The identity comes from the checked cooked envelope, not editor metadata.
	// Reimport uses it to reject a file that was copied over the wrong GUID.
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	std::filesystem::path resolvedPath;
	std::uint64_t fileSizeBytes = 0U;
	std::uint16_t detectedMajorVersion = 0U;
	std::uint16_t detectedMinorVersion = 0U;
	std::uint64_t expectedCompatibilityFingerprint = 0U;
	std::uint64_t actualCompatibilityFingerprint = 0U;
	// The compact prefix remains useful in UI diagnostics, while transactional
	// publication compares contentHash in full to prevent prefix collisions.
	std::uint64_t contentFingerprint = 0U;
	std::array<std::uint8_t, 32U> contentHash{};
	bool contentHashPresent = false;
	bool compatibilityFingerprintPresent = false;
	std::uint32_t jointCount = 0U;
	std::uint32_t clipSegmentCount = 0U;
	std::uint32_t clipTrackCount = 0U;
	std::uint32_t deformableVertexCount = 0U;
	std::uint32_t palettePartitionCount = 0U;
	std::uint32_t morphTargetCount = 0U;
	std::uint32_t zeroWeightIdentityVertexCount = 0U;
	std::uint32_t sourceSkeletonJointCount = 0U;
	std::string diagnosticCode;
	std::string message;

	[[nodiscard]] bool Passed() const noexcept
	{
		return status == TerraEngine::Animation::AnimationStatus::Ok;
	}
};

enum class AnimationRangeReadStatus : std::uint8_t
{
	Ok,
	InvalidRequest,
	Cancelled,
	NotFound,
	OutOfRange,
	ReadLimitExceeded,
	NotAnimationAsset,
	IoError
};

// Streaming jobs return owned bytes plus a stable failure category. The
// evaluator can therefore keep its prior generation/reference pose without
// parsing log text or treating an empty vector as an ambiguous success.
struct AnimationRangeReadResult
{
	AnimationRangeReadStatus status = AnimationRangeReadStatus::InvalidRequest;
	std::filesystem::path resolvedPath;
	std::vector<std::uint8_t> bytes;
	std::string message;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == AnimationRangeReadStatus::Ok;
	}
};

struct NavigationGeometryLoadResult
{
	bool accepted = false;
	std::string reason;
	std::filesystem::path resolvedPath;
	std::size_t sourceBytes = 0U;
	std::shared_ptr<const TerraEngine::Navigation::NavigationGeometrySnapshot> snapshot;
};

// Animation binding reconciliation needs only a compact proof of a mesh's
// immutable deformable layout. Copying these values under the cache lock keeps
// raw Mesh pointers out of protected frame-boundary work, while a missing
// optional from the query below explicitly means a publisher currently owns
// the cache lock and the caller should retry on the next frame.
struct MeshAnimationBindingSnapshot
{
	bool present = false;
	bool hasDeformablePayload = false;
	std::uint32_t runtimeLayoutVersion = 0U;
	std::uint64_t deformablePayloadGeneration = 0U;
	std::uint32_t skinningVertexCount = 0U;
	std::uint32_t meshletSkinningDataCount = 0U;
	std::size_t meshletSkinningDataBytes = 0U;
	std::uint32_t palettePartitionCount = 0U;
	std::size_t paletteRecordCount = 0U;
	std::uint32_t singlePaletteJointCount = 0U;
	std::uint32_t morphTargetCount = 0U;
	std::size_t morphRecordCount = 0U;
	std::size_t vertexPaletteIndexCount = 0U;
	std::uint32_t sourceSkeletonJointCount = 0U;
	TerraEngine::Animation::AnimationCompatibilityFingerprint
		skeletonCompatibilityFingerprint = 0U;
	std::uint32_t flags = 0U;
	std::array<float, 3U> deformationBoundsMinimum{};
	std::array<float, 3U> deformationBoundsMaximum{};
	std::vector<std::uint32_t> paletteJoints;
	std::vector<std::uint64_t> morphStableIds;
};

// A block range is an absolute byte interval inside one checked cooked asset.
// ResourceManager derives these intervals from the same published generation
// that supplied the immutable typed asset, so streaming callers never guess
// offsets from in-memory structures whose layout differs from the disk format.
struct AnimationCookedBlockRange
{
	std::uint32_t blockIndex = 0U;
	std::uint64_t byteOffset = 0U;
	std::uint64_t byteSize = 0U;
};

enum class AnimationCookedRangeStatus : std::uint8_t
{
	Ok,
	InvalidRequest,
	MissingAsset,
	StaleGeneration,
	InvalidFormat,
	UnsupportedLayout,
	IoError
};

// The table is a value snapshot captured under the animation cache lock. Its
// path, generation, and ranges can safely cross into worker-owned IO requests.
struct AnimationCookedRangeTable
{
	AnimationCookedRangeStatus status = AnimationCookedRangeStatus::InvalidRequest;
	std::filesystem::path logicalPath;
	TerraEngine::Animation::AnimationGeneration generation = 0U;
	std::vector<AnimationCookedBlockRange> blocks;
	std::string message;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == AnimationCookedRangeStatus::Ok;
	}
};

// One request names an already-published live animation cache entry. Batch
// preparation reparses every requested cooked file before any cache changes,
// then the protected commit validates that these exact generations still own
// their paths. This gives editor hot reload one all-or-nothing cache boundary.
struct AnimationAssetReloadRequest
{
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	AssetType assetType = AssetType::Skeleton;
	// These fingerprints bind a resource-cache replacement to the exact
	// publisher candidate that AssetDatabase validated. Zero preserves the
	// standalone ResourceManager reload API's legacy unchecked behavior.
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U;
	std::uint64_t expectedContentFingerprint = 0U;
	// The publisher's currently active generation, captured before preparing its
	// replacement. It is intentionally independent from ResourceManager's
	// cache-local counter and identifies the payload that publisher retirement
	// will later authorize for destruction.
	std::uint64_t publisherRetirementGeneration = 0U;
	std::array<std::uint8_t, 32U> expectedContentHash{};
	bool expectedContentHashPresent = false;
};

// PreparedAnimationAssetReloadBatch owns parsed replacement payloads behind an
// opaque state so this public header does not expose ResourceManager's private
// cache representation. The value is move-only because a prepared transaction
// may be committed at most once.
class TERRAENGINE_API PreparedAnimationAssetReloadBatch final
{
	struct State;

public:
	PreparedAnimationAssetReloadBatch() noexcept;
	~PreparedAnimationAssetReloadBatch();
	PreparedAnimationAssetReloadBatch(PreparedAnimationAssetReloadBatch&& other) noexcept;
	PreparedAnimationAssetReloadBatch& operator=(PreparedAnimationAssetReloadBatch&& other) noexcept;
	PreparedAnimationAssetReloadBatch(const PreparedAnimationAssetReloadBatch&) = delete;
	PreparedAnimationAssetReloadBatch& operator=(const PreparedAnimationAssetReloadBatch&) = delete;

	[[nodiscard]] bool Accepted() const noexcept;
	[[nodiscard]] bool Committed() const noexcept;
	[[nodiscard]] std::size_t Size() const noexcept;
	[[nodiscard]] std::string_view Diagnostic() const noexcept;

private:
	friend class ResourceManager;
	std::unique_ptr<State> m_state;
	std::string m_diagnostic;
	bool m_accepted = false;
	bool m_committed = false;
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
	// Cooked skeleton and clip loaders never inspect glTF/FBX source files. A
	// nonzero expected fingerprint turns loading into an atomic compatibility
	// check; an incompatible payload is rejected before entering the cache.
	TerraEngine::UUID LoadSkeleton(
		const std::filesystem::path& path,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U);
	TerraEngine::UUID LoadAnimationClip(
		const std::filesystem::path& path,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U);
	// Reload replaces the immutable payload under the same durable UUID. It
	// first publishes a tombstone generation, so retained animation handles can
	// never alias the replacement while another system observes the reload.
	bool ReloadSkeleton(const TerraEngine::UUID& id);
	bool ReloadAnimationClip(const TerraEngine::UUID& id);
	// Reload any cooked animation asset through its checked typed reader. The
	// active payload is replaced only after the candidate parses completely and
	// the observed cache generation still matches, preserving last-known-good
	// data when a watcher races another publication or sees a corrupt file.
	bool ReloadAnimationAsset(const TerraEngine::UUID& id, AssetType type);
	// Parse every requested replacement off to the side without changing live
	// caches. CommitPreparedAnimationAssetReloadBatch then locks all animation
	// caches in one fixed order, revalidates every path/generation, and performs
	// only no-fail pointer/generation swaps. A false commit changes no live cache.
	PreparedAnimationAssetReloadBatch PrepareAnimationAssetReloadBatch(
		std::span<const AnimationAssetReloadRequest> requests);
	bool CommitPreparedAnimationAssetReloadBatch(
		PreparedAnimationAssetReloadBatch& prepared) noexcept;
	// Refresh residency and streaming diagnostics only after the coordinating
	// publisher has also committed. Diagnostic bookkeeping is intentionally not
	// part of the no-fail pointer-swap callback.
	void FinalizeCommittedAnimationAssetReloadBatch(
		PreparedAnimationAssetReloadBatch& prepared) noexcept;
	// The publisher invokes this only after its own generation tables become
	// visible. It releases the cache locks retained by the coordinated commit,
	// so no reader can observe a cache-only intermediate state.
	void ReleasePreparedAnimationAssetReloadBatchLocks(
		PreparedAnimationAssetReloadBatch& prepared) noexcept;
	// Old payloads are retained by publisher generation until the publisher
	// proves that its protected boundary has retired the matching generation.
	void RetireAnimationAssetPayloadGeneration(const TerraEngine::UUID& id,
		TerraEngine::Animation::AnimationGeneration generation) noexcept;
	// Project/session teardown is a protected boundary that invalidates every
	// publisher retain. Release all old cache generations before publisher state
	// is reset so no orphaned retirement records survive into the next project.
	void ReleaseAllRetiredAnimationAssetPayloadsAtProtectedBoundary();
	// Diagnostics and lifecycle tests use this bounded count to prove that
	// publisher retirement reclaims real cache ownership, not only metadata.
	[[nodiscard]] std::size_t GetRetiredAnimationPayloadCountForDiagnostics() const;
	// This explicit entry point requires a complete canonical deformable
	// descriptor set. LoadMesh accepts canonical static TEMF v1 assets.
	TerraEngine::UUID LoadDeformableMesh(
		const std::filesystem::path& path,
		std::uint32_t expectedSkeletonJointCount = 0U,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U);
	TerraEngine::UUID LoadShader(const std::filesystem::path& path);
	TerraEngine::UUID LoadShaderCollection(const std::vector<ShaderStagePath>& stagePaths);
	TerraEngine::UUID LoadTexture(const std::filesystem::path& path);
	// Audio loads only the validated provider-neutral TEAF envelope. The returned
	// shared value keeps the encoded bytes alive for backend frame packets.
	TerraEngine::UUID LoadAudioClip(const std::filesystem::path& path);
	bool ReloadAudioClip(const TerraEngine::UUID& id);

	// Re-parse a material from disk to pick up authored changes without clearing
	// the entire cache. This ensures the renderer sees new parameters immediately.
	void ReloadMaterial(const TerraEngine::UUID& id);
	// Inspector edits are already complete validated TMAT files. Publish that
	// authored file directly into the live cache while keeping the registered
	// cooked path unchanged for later project loads and exports.
	[[nodiscard]] bool ReloadMaterialFromAuthoredPath(
		const TerraEngine::UUID& id,
		const std::filesystem::path& authoredPath);

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
		std::int32_t priority = 0,
		const TerraEngine::UUID& ownerObjectId = TerraEngine::EMPTY_UUID);
	// Object-owned needs let protected scene synchronization replace or remove
	// one animator's retention without disturbing another animator that shares
	// the same immutable asset publication.
	std::size_t ReleaseStreamingNeedsForObject(
		const TerraEngine::UUID& ownerObjectId,
		TerraEngine::Resource::StreamingNeedType needType);
	// Called by WorldStreamingSystem only at the protected residency boundary.
	// It records dependencies as normal asset needs and never owns scene objects.
	void PublishSceneRegionAssetNeeds(
		std::span<const TerraEngine::Resource::SceneRegionAssetNeed> needs,
		std::uint64_t frameId);
	void PublishVisibleStreamingTouches(
		const TerraEngine::Renderer::VisibleStreamingTouchSet& touchSet,
		std::uint64_t frameId,
		std::string_view reason = {});
	std::shared_future<TerraEngine::UUID> RequestStreamableLoad(
		const TerraEngine::UUID& id,
		AssetType type,
		TerraEngine::Resource::StreamingNeedType needType,
		std::uint64_t frameId,
		std::string_view reason = {},
		const TerraEngine::UUID& ownerObjectId = TerraEngine::EMPTY_UUID);
	// Cancellation invalidates the request's private publication token. Backend
	// IO may finish, but its stale completion cannot enter a live cache; a later
	// request receives a fresh token and may safely reuse the durable asset ID.
	bool CancelStreamableLoad(const TerraEngine::UUID& id, AssetType type);
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
	// Navigation companions use a separate typed registry because they are
	// checked CPU geometry attached directly to an eligible mesh/collider asset,
	// not renderer state or authored navigation-source metadata. Logical mounted
	// paths and editor paths share this same loader.
	void RegisterNavigationGeometryPath(const TerraEngine::UUID& id, const std::filesystem::path& path);
	[[nodiscard]] NavigationGeometryLoadResult LoadNavigationGeometryCompanion(
		const TerraEngine::UUID& id,
		const std::filesystem::path& path,
		const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& descriptor,
		std::size_t maximumBytes = 256U * 1024U * 1024U);
	[[nodiscard]] NavigationGeometryLoadResult LoadRegisteredNavigationGeometryCompanion(
		const TerraEngine::UUID& id,
		const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& descriptor,
		std::size_t maximumBytes = 256U * 1024U * 1024U);
	[[nodiscard]] std::shared_ptr<const TerraEngine::Navigation::NavigationGeometrySnapshot>
		GetNavigationGeometrySnapshot(const TerraEngine::UUID& id) const;
	// Advances only after a complete checked companion snapshot is exchanged or
	// released. Automatic navigation capture observes this instead of polling
	// renderer state, so a companion reload wakes the next protected capture.
	[[nodiscard]] std::uint64_t NavigationGeometryPublicationGeneration() const noexcept
	{
		return navigationGeometryPublicationGeneration.load(std::memory_order_acquire);
	}
	bool ReleaseNavigationGeometrySnapshot(const TerraEngine::UUID& id);
	// Resolve a script-facing asset reference into the UUID the renderer uses
	// internally. The lookup accepts either an exact relative path or a simple
	// asset name/stem so gameplay code does not need to memorize UUIDs.
	TerraEngine::UUID ResolveAssetReference(std::string_view assetReference, AssetType type);
	// Convert an internal UUID back into a readable asset path so managed code
	// can display or persist friendly identifiers instead of raw GUID text.
	std::filesystem::path DescribeAssetReference(const TerraEngine::UUID& id, AssetType type) const;

	// These read-only commands use the same checked parsers as publication and
	// expose exact counts, versions, fingerprints, and recovery guidance. They
	// do not reserve UUIDs or mutate a live resource cache.
	AnimationResourceValidationResult ValidateSkeletonAssetFile(
		const std::filesystem::path& path,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U) const;
	AnimationResourceValidationResult ValidateAnimationClipAssetFile(
		const std::filesystem::path& path,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U) const;
	// Validate any of the seven cooked animation asset kinds through its typed
	// parser without reserving a UUID or mutating a live cache. Editor reimport
	// uses this before atomically replacing its last-known-good build artifact.
	AnimationResourceValidationResult ValidateAnimationAssetFile(
		const std::filesystem::path& path,
		AssetType type,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U) const;
	AnimationResourceValidationResult ValidateDeformableMeshFile(
		const std::filesystem::path& path,
		std::uint32_t expectedSkeletonJointCount = 0U,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0U) const;

	// Inject a generated material directly into the cache so tooling can synthesize preview materials
	// without writing TMAT files to disk. The cacheKey should be unique and stable (e.g., "generated/preview").
	TerraEngine::UUID RegisterMaterial(const std::string& cacheKey, std::unique_ptr<Material> material);
	// Generated control maps use the same immutable CPU texture representation as
	// loaded TETF assets, but do not require a temporary file on disk.
	TerraEngine::UUID RegisterTexture(const std::string& cacheKey, std::unique_ptr<Texture> texture);
	// Publish a fully prepared generated mesh under a stable synthetic key. The
	// caller must invalidate the matching backend mesh after replacing an existing
	// payload; this method owns only the atomic CPU cache and bounds publication.
	TerraEngine::UUID RegisterMesh(const std::string& cacheKey, std::unique_ptr<Mesh> mesh);

	// Asynchronous load helpers. Each call schedules a background task on
	// the shared thread pool and returns a shared_future that multiple
	// callers can await safely. Duplicate requests will join the same
	// in-flight future so disk I/O is never duplicated.
	std::shared_future<TerraEngine::UUID> LoadMaterialAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadMeshAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadShaderAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadShaderCollectionAsync(const std::vector<ShaderStagePath>& stagePaths);
	std::shared_future<TerraEngine::UUID> LoadTextureAsync(const std::filesystem::path& path);
	std::shared_future<TerraEngine::UUID> LoadAnimationAssetAsync(
		const std::filesystem::path& path,
		AssetType type);

	// Fetch a loaded resource using its UUID so callers can pass lightweight
	// identifiers around without holding shared ownership. Returned pointers
	// are non-owning views into the cache and remain valid until ClearCaches
	// or a subsequent reload replaces the resource.
	Material* GetMaterial(const TerraEngine::UUID& id);
	Mesh* GetMesh(const TerraEngine::UUID& id);
	// Observe an already-published mesh without starting a synchronous load or
	// waiting behind an importer. A contained snapshot with present=false means
	// the mesh is not currently cached; nullopt means the cache is writer-owned.
	[[nodiscard]] std::optional<MeshAnimationBindingSnapshot>
		TryCopyMeshAnimationBindingSnapshot(const TerraEngine::UUID& id) const;
	TerraEngine::Animation::SkeletonAsset* GetSkeleton(const TerraEngine::UUID& id);
	TerraEngine::Animation::AnimationClipAsset* GetAnimationClip(const TerraEngine::UUID& id);
	[[nodiscard]] std::shared_ptr<const TerraEngine::Audio::AudioClipAsset> GetAudioClip(const TerraEngine::UUID& id);
	// Return the session-local generation currently assigned to a cooked
	// animation publication or tombstone. Zero means the UUID/type has not been
	// published in this manager session (or is not an animation cache type).
	[[nodiscard]] TerraEngine::Animation::AnimationGeneration GetAnimationAssetRuntimeGeneration(
		const TerraEngine::UUID& id,
		AssetType type) const noexcept;
	// Frame-boundary systems must not wait behind a long asset publication. A
	// missing optional means the cache is writer-owned; callers retain their last
	// complete immutable binding and retry at the next protected boundary.
	[[nodiscard]] std::optional<TerraEngine::Animation::AnimationGeneration>
		TryGetAnimationAssetRuntimeGeneration(
			const TerraEngine::UUID& id,
			AssetType type) const noexcept;
	// Animation bindings include both authored animation assets and deformable
	// mesh payloads. This monotonic revision lets the protected binding bridge
	// skip an expensive scene-wide rescan until one of those inputs is republished.
	[[nodiscard]] std::uint64_t GetAnimationBindingResourceRevision() const noexcept
	{
		return m_AnimationBindingResourceRevision.load(std::memory_order_acquire);
	}
	// Renderer snapshots consume this publication token at their protected frame
	// boundary. It changes only after a mesh, material, or texture payload has
	// replaced its cache entry, so failed reloads leave retained GPU plans valid.
	[[nodiscard]] std::uint64_t GetRendererResourcePublicationGeneration() const noexcept
	{
		return m_RendererResourcePublicationGeneration.load(std::memory_order_acquire);
	}
	// A retained runtime generation may describe a deletion or eviction
	// tombstone. This lightweight query distinguishes that state from an actual
	// immutable payload without copying a potentially large clip or database.
	[[nodiscard]] bool IsAnimationAssetPublished(
		const TerraEngine::UUID& id,
		AssetType type) const noexcept;
	// Copy a consistent asset/generation pair while holding the cache lock. The
	// coordinator calls these only when its cheap generation signature changed,
	// avoiding per-frame deep copies and raw-pointer reload races.
	[[nodiscard]] bool CopySkeletonPublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::SkeletonAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
	[[nodiscard]] bool CopyAnimationClipPublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::AnimationClipAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
	[[nodiscard]] bool CopyAnimationControllerPublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::AnimationControllerAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
	[[nodiscard]] bool CopyAnimationMaskPublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::AnimationMaskAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
	// Specialist animation assets are cached as checked immutable bytes. These
	// helpers deserialize while holding the cache read lock and return the exact
	// session generation paired with those bytes, preventing hot reload from
	// combining a new payload with an old generation in AnimationSystem.
	[[nodiscard]] bool CopyRetargetProfilePublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::RetargetProfileAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
	[[nodiscard]] bool CopyMotionDatabasePublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::MotionDatabaseAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
	[[nodiscard]] bool CopyRagdollProfilePublication(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::RagdollProfileAsset& output,
		TerraEngine::Animation::AnimationGeneration& generation) const;
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
	// Repeated resource-streaming callers retain private staging capacity across
	// reads. One-shot callers can continue using the owned-result overload above.
	[[nodiscard]] bool ReadFileBytesInto(
		const std::filesystem::path& path,
		std::vector<std::uint8_t>& output) const;
	// Streaming workers use this checked range path for clip segments and motion
	// database blocks. Export runtimes read only from the mounted TEPAK; editor
	// sessions may use a loose cooked artifact for iteration.
	AnimationRangeReadResult ReadAnimationAssetRange(
		const std::filesystem::path& path,
		std::uint64_t offset,
		std::uint64_t size,
		std::uint64_t maximumReadBytes) const;
	// Resolve every clip segment to its real DATA/stream byte interval after
	// checking the cooked envelope against the currently published clip. This
	// performs only small metadata reads; segment payload bytes stay asynchronous.
	[[nodiscard]] AnimationCookedRangeTable DescribeAnimationClipSegmentRanges(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::AnimationGeneration generation) const;
	// Current motion databases add an optional contiguous STRM chunk whose checked
	// block intervals can be read independently. Valid legacy DATA-only assets
	// still report UnsupportedLayout and keep the complete CPU fallback active.
	[[nodiscard]] AnimationCookedRangeTable DescribeMotionDatabaseBlockRanges(
		const TerraEngine::UUID& id,
		TerraEngine::Animation::AnimationGeneration generation) const;

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
		// Retained tombstones make a delete/reload distinguishable from a first
		// publication of the same durable GUID for the lifetime of this manager.
		std::unordered_map<TerraEngine::UUID, TerraEngine::Animation::AnimationGeneration> runtimeGenerations;
		// Futures for tasks that are actively loading. Shared futures let
		// multiple threads wait without spawning redundant tasks.
		std::unordered_map<std::string, std::shared_future<TerraEngine::UUID>> inFlight;
		// A task captures the token current when it was scheduled. Cancellation,
		// teardown, or a retry removes/replaces that token so FinalizeLoad rejects
		// stale worker output without touching the current immutable payload.
		std::unordered_map<std::string, std::uint64_t> inFlightTokens;
		std::uint64_t nextInFlightToken = 0u;
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

	// Several animation authoring assets are consumed by their specialist
	// evaluators rather than by a renderer-facing typed cache.  The resource
	// manager still owns their byte payload lifetime so streaming, cancellation,
	// and residency policy cannot accidentally treat those assets as free.
	struct OpaqueAnimationResource
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::vector<std::uint8_t> bytes;
	};
	struct RetiredAnimationPayload
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		// Keep both dimensions: cacheGeneration documents the exact resource
		// instance replaced, while publisherGeneration is the authority used by
		// cross-owner retirement callbacks.
		TerraEngine::Animation::AnimationGeneration cacheGeneration = 0U;
		std::uint64_t publisherGeneration = 0U;
		void* payload = nullptr;
		void (*destroyPayload)(void*) noexcept = nullptr;
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
	std::shared_future<TerraEngine::UUID> LoadOrSchedule(
		const std::filesystem::path& path,
		ResourceCache<Resource>& cache,
		const LoaderFunction<Resource>& loader,
		bool deriveIdentityFromResource = false);

	// Finalize a successful load by updating caches and clearing any
	// in-flight tracking entries. This runs on both synchronous and
	// asynchronous code paths to keep cache semantics consistent.
	template <typename Resource>
	TerraEngine::UUID FinalizeLoad(
		const std::string& key,
		ResourceCache<Resource>& cache,
		const TerraEngine::UUID& id,
		std::unique_ptr<Resource> resource,
		std::uint64_t expectedLoadToken = 0u);

	// Advance one cache-owned animation generation while its cache mutex is
	// held. Generation zero is reserved for an invalid/stale publication, and
	// exhaustion fails closed rather than wrapping a stale handle back to one.
	template <typename Resource>
	std::optional<TerraEngine::Animation::AnimationGeneration> AdvanceAnimationGenerationLocked(
		ResourceCache<Resource>& cache,
		const TerraEngine::UUID& id);

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
	std::uint64_t ComputeLoadedResourceBytes(const TerraEngine::Animation::SkeletonAsset& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const TerraEngine::Animation::AnimationClipAsset& resource) const;
	std::uint64_t ComputeLoadedResourceBytes(const OpaqueAnimationResource& resource) const;
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
	// Shutdown, unlike a regular cache clear, ends the ResourceManager session.
	// Only then may the otherwise monotonic animation generation ledgers reset.
	void ClearAnimationRuntimeGenerations();
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
	TerraEngine::UUID LoadOpaqueAnimationAsset(const std::filesystem::path& path, AssetType type);
	std::shared_future<TerraEngine::UUID> LoadOpaqueAnimationAssetAsync(const std::filesystem::path& path, AssetType type);

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
	// Bounded ownership for old immutable payloads. Entries are added while
	// the coordinated cache locks are held and removed by publisher retirement.
	std::vector<RetiredAnimationPayload> m_RetiredAnimationPayloads;
	mutable std::mutex m_RetiredAnimationPayloadMutex;
	// Only mesh and animation publications affect immutable animator bindings.
	// Keeping this narrower than the general resource cache prevents texture and
	// material streaming from waking the expensive binding synchronization path.
	std::atomic_uint64_t m_AnimationBindingResourceRevision{1U};
	// Keep renderer asset publication separate from animation bindings: textures
	// and materials must invalidate draw preparation without waking the expensive
	// animator binding coordinator.
	std::atomic_uint64_t m_RendererResourcePublicationGeneration{1U};

	ResourceCache<Material> materialCache;
	ResourceCache<Mesh> meshCache;
	ResourceCache<TerraEngine::Animation::SkeletonAsset> skeletonCache;
	ResourceCache<TerraEngine::Animation::AnimationClipAsset> animationClipCache;
	ResourceCache<OpaqueAnimationResource> animationControllerCache;
	ResourceCache<OpaqueAnimationResource> animationMaskCache;
	ResourceCache<OpaqueAnimationResource> retargetProfileCache;
	ResourceCache<OpaqueAnimationResource> motionDatabaseCache;
	ResourceCache<OpaqueAnimationResource> ragdollProfileCache;
	ResourceCache<Shader> shaderCache;
	ResourceCache<ShaderCollection> shaderCollectionCache;
	ResourceCache<Texture> textureCache;
	AssetPathRegistry physicsMaterialPaths;
	AssetPathRegistry cookedColliderPaths;
	// Audio uses shared immutable generations rather than raw cache pointers:
	// provider packets can retain a generation past a protected reload boundary.
	AssetPathRegistry audioClipPaths;
	// A terrain path is a root-manifest lookup; its heavy area data never enters
	// the generic ResourceManager cache.
	AssetPathRegistry terrainPaths;
	AssetPathRegistry navigationGeometryPaths;
	std::unordered_map<TerraEngine::UUID,
		std::shared_ptr<const TerraEngine::Navigation::NavigationGeometrySnapshot>> navigationGeometrySnapshots;
	mutable std::shared_mutex navigationGeometryMutex;
	std::atomic_uint64_t navigationGeometryPublicationGeneration{1U};
	std::unordered_map<TerraEngine::UUID, std::shared_ptr<const TerraEngine::Audio::AudioClipAsset>> audioClipAssets;
	std::unordered_map<TerraEngine::UUID, std::uint64_t> audioClipGenerations;
	mutable std::shared_mutex audioClipMutex;
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
std::optional<TerraEngine::Animation::AnimationGeneration> ResourceManager::AdvanceAnimationGenerationLocked(
	ResourceCache<Resource>& cache,
	const TerraEngine::UUID& id)
{
	auto [iterator, inserted] = cache.runtimeGenerations.try_emplace(id, 0U);
	(void)inserted;
	if(iterator->second == std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max()) {
		// A wrapped generation could make a stale handle valid again. The engine
		// cannot safely publish another payload for this UUID in this session.
		return std::nullopt;
	}
	++iterator->second;
	// Every animation generation change is a binding-visible publication,
	// including tombstones created by deletion.
	m_AnimationBindingResourceRevision.fetch_add(1U, std::memory_order_release);
	return iterator->second;
	}

template <typename Resource>
// ResourceManager::FinalizeLoad is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::FinalizeLoad(
	const std::string& key,
	ResourceCache<Resource>& cache,
	const TerraEngine::UUID& id,
	std::unique_ptr<Resource> resource,
	std::uint64_t expectedLoadToken)
{
	// Capture success before we move the resource into the cache so logging reflects the actual loader result.
	bool loadedSuccessfully = static_cast<bool>(resource);
	TerraEngine::UUID publicationId = id;

	std::unique_lock lock(cache.mutex);
	if (expectedLoadToken != 0u)
	{
		const auto currentToken = cache.inFlightTokens.find(key);
		if (currentToken == cache.inFlightTokens.end() || currentToken->second != expectedLoadToken)
		{
			// The worker may still finish physical IO after cancellation. Rejecting
			// here is the protected publication boundary: live payloads, generations,
			// and residency accounting remain completely unchanged.
			return TerraEngine::EMPTY_UUID;
		}
	}
	if (loadedSuccessfully)
	{
		if constexpr (requires(const Resource& value) { value.assetId.value; })
		{
			const TerraEngine::UUID embeddedId = resource->assetId.value;
			if (publicationId == TerraEngine::EMPTY_UUID)
			{
				publicationId = embeddedId;
			}
			if (embeddedId == TerraEngine::EMPTY_UUID || embeddedId != publicationId)
			{
				loadedSuccessfully = false;
				resource.reset();
			}
		}
		else if constexpr (requires(const Resource& value) { value.assetId == id; })
		{
			const TerraEngine::UUID embeddedId = resource->assetId;
			if (publicationId == TerraEngine::EMPTY_UUID)
			{
				publicationId = embeddedId;
			}
			if (embeddedId == TerraEngine::EMPTY_UUID || embeddedId != publicationId)
			{
				loadedSuccessfully = false;
				resource.reset();
			}
		}
		else if (publicationId == TerraEngine::EMPTY_UUID)
		{
			// Only self-identifying resources may defer UUID selection until parsing.
			loadedSuccessfully = false;
			resource.reset();
		}
	}
	if (loadedSuccessfully && IsAnimationAssetType(cache.assetType))
	{
		const std::optional<TerraEngine::Animation::AnimationGeneration> generation =
			AdvanceAnimationGenerationLocked(cache, publicationId);
		if (!generation.has_value())
		{
			loadedSuccessfully = false;
			resource.reset();
		}
		else if constexpr (requires(Resource& value, TerraEngine::Animation::AnimationGeneration runtimeGeneration) {
			value.runtimeGeneration = runtimeGeneration;
		})
		{
			resource->runtimeGeneration = *generation;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (resource)
	{
		cache.pathToId[key] = publicationId;
		cache.idToPath[publicationId] = key;
		cache.resources[publicationId] = std::move(resource);
		if (IsRendererResourceAssetType(cache.assetType))
		{
			// Live Inspector assignments can demand an asset after the renderer has
			// already retained topology and backend resource plans. Publish only after
			// the complete typed payload is visible so the next protected frame rebuilds
			// those plans without requiring an editor restart.
			m_RendererResourcePublicationGeneration.fetch_add(
				1U, std::memory_order_release);
		}
		if (cache.assetType == AssetType::Mesh)
		{
			// Publication happens while the typed cache lock is exclusive. The
			// release operation makes the completed payload visible before a
			// protected binding boundary observes the new revision.
			m_AnimationBindingResourceRevision.fetch_add(1U, std::memory_order_release);
		}
	}
	else
	{
		// A failed first load leaves no payload. A failed retry/reload deliberately
		// keeps any previous immutable generation instead of erasing last-known-good
		// data while the caller is still using it.
	}

	cache.inFlight.erase(key);
	cache.inFlightTokens.erase(key);
	// Log the outcome to tie disk/parsing work back to a UUID for later lookups.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (loadedSuccessfully)
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Finalized load for " << key << " (id " << ToHexString(publicationId) << ")");
		CacheLoadedMeshBounds(publicationId, cache.resources[publicationId].get());
		if (cache.tracksResidency)
		{
			RegisterResidencyResource(
				publicationId,
				cache.assetType,
				key,
				EstimateLoadedResourceBytes(cache.resources[publicationId]),
				0u,
				true,
				TerraEngine::Resource::ResidencyState::Active);
		}
	}
	else
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Load failed for " << key << "; last-known-good cache state was preserved for id " << ToHexString(id));
	}
	UpdateStreamableLoadResult(
		loadedSuccessfully ? publicationId : id,
		cache.assetType,
		key,
		loadedSuccessfully,
		loadedSuccessfully && cache.tracksResidency ? EstimateLoadedResourceBytes(cache.resources[publicationId]) : 0u,
		0u,
		loadedSuccessfully ? std::string_view{} : std::string_view("loader returned no payload"));
	return loadedSuccessfully ? publicationId : TerraEngine::EMPTY_UUID;
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
	case AssetType::Skeleton:
		visitor(skeletonCache);
		return;
	case AssetType::AnimationClip:
		visitor(animationClipCache);
		return;
	case AssetType::AnimationController:
		visitor(animationControllerCache);
		return;
	case AssetType::AnimationMask:
		visitor(animationMaskCache);
		return;
	case AssetType::RetargetProfile:
		visitor(retargetProfileCache);
		return;
	case AssetType::MotionDatabase:
		visitor(motionDatabaseCache);
		return;
	case AssetType::RagdollProfile:
		visitor(ragdollProfileCache);
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
std::shared_future<TerraEngine::UUID> ResourceManager::LoadOrSchedule(
	const std::filesystem::path& path,
	ResourceCache<Resource>& cache,
	const LoaderFunction<Resource>& loader,
	bool deriveIdentityFromResource)
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

	// Cooked animation files own a durable UUID in their checked envelope. When
	// no editor/runtime registration exists yet, leave the identity empty until
	// the worker has parsed that envelope; generating a random path UUID here
	// would make a valid standalone async load fail its identity check.
	TerraEngine::UUID id = deriveIdentityFromResource
		? TerraEngine::EMPTY_UUID : EnsureResourceId(key, cache);
	std::unique_lock lock(cache.mutex);
	// Repeat both checks while holding the publication lock. The earlier shared
	// check is only a fast path; this boundary guarantees concurrent callers see
	// one future instead of briefly creating competing tokens before either task
	// has installed its shared future.
	if (const auto existing = cache.pathToId.find(key); existing != cache.pathToId.end())
	{
		id = existing->second;
		if (cache.resources.contains(existing->second))
		{
			return MakeReadyFuture(existing->second);
		}
	}
	if (const auto pending = cache.inFlight.find(key); pending != cache.inFlight.end())
	{
		return pending->second;
	}
	if (cache.nextInFlightToken == std::numeric_limits<std::uint64_t>::max())
	{
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}
	const std::uint64_t loadToken = ++cache.nextInFlightToken;
	cache.inFlightTokens[key] = loadToken;
	// Schedule the expensive file I/O work on the shared task system so callers
	// can continue immediately. The identity may be empty only for an
	// unregistered, self-identifying animation resource.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async load for " << resolvedPath << " (id " << ToHexString(id) << ")");
	auto task = threadPool.createTask([this, key, resolvedPath, &cache, id, loader, loadToken]() -> TerraEngine::UUID {
		auto resource = loader(resolvedPath);
		return FinalizeLoad(key, cache, id, std::move(resource), loadToken);
	});
	auto future = task->getFuture().share();
	cache.inFlight[key] = future;
	// Enqueueing only publishes work to the shared task system. The worker may
	// begin immediately, but FinalizeLoad waits for this short cache lock before
	// it can publish a payload, so the future/token pair is always fully visible.
	threadPool.enqueue(task);
	return future;
}
