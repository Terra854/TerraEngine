// AssetDatabase.h — Editor asset database management
//
// Defines the interface for the editor's asset database, responsible for tracking source assets, managing their metadata, and coordinating compilation. it handles asset GUID lookups, importer mapping, and provides a central registry for all project resources.

#pragma once

#include "Utilities/UUID.h"
#include "FileWatcher.h"
#include "Animation/Streaming/AnimationHotReload.h"
#include "Physics/Assets/PhysicsAssetChangeJournal.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <future>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <deque>
#include <utility>

struct AssetMetadata {
	TerraEngine::UUID guid;
	std::filesystem::file_time_type lastModified;
	std::string importerName; 
	std::filesystem::path sourcePath;   
	std::filesystem::path compiledPath; 
	nlohmann::json settings; // Importer-specific settings
};

struct VirtualGeometryAssetMetadata {
	bool readable = false;
	bool hasVirtualGeometry = false;
	bool needsRebuild = false;
	std::uint32_t payloadGeneration = 0u;
	std::uint32_t detailLevelCount = 0u;
	std::uint32_t meshletCount = 0u;
	std::uint32_t sourceTriangleCount = 0u;
	std::uint32_t requestedTargetCount = 0u;
	std::uint32_t skippedDetailCount = 0u;
	std::uint32_t clusterCount = 0u;
	std::uint32_t materialSectionCount = 0u;
	std::uint32_t pageCount = 0u;
	std::uint32_t coneSafetyRecordCount = 0u;
	std::uint32_t coneEligibleClusterCount = 0u;
	std::uint32_t coneDisabledClusterCount = 0u;
	bool pageStreamingReady = false;
	bool smallMeshStreamingSkipped = false;
	bool coneSafetyMetadataValid = false;
	std::string skippedStreamingReason;
	std::string rebuildGuidance;
	std::vector<std::string> warnings;
	std::vector<std::string> pageIntegrityErrors;
	std::vector<std::string> coneDisabledReasons;
};

struct VirtualGeometryAdaptiveSettingsMetadata {
	bool valid = true;
	bool overridePresent = false;
	std::string settingsSource = "ProjectDefaults";
	std::vector<float> targetRatios;
	// Project defaults preserve every valid LOD. The inspector can persist
	// stricter percentage or count thresholds for an individual mesh.
	std::uint32_t minimumSourceTriangles = 1u;
	std::uint32_t minimumGeneratedTriangles = 1u;
	std::uint32_t minimumMeshletsPerGeneratedLevel = 1u;
	float minimumUsefulTriangleReduction = 0.0f;
	float minimumUsefulMeshletReduction = 0.0f;
	float maximumGeometricError = 0.1f;
	std::vector<std::string> validationErrors;
};

struct VirtualGeometryDetailDiagnosticMetadata {
	std::uint32_t targetIndex = 0u;
	float requestedRatio = 1.0f;
	std::string status = "Requested";
	std::string reason;
	std::uint32_t desiredTriangleCount = 0u;
	std::uint32_t desiredMeshletCount = 0u;
	std::uint32_t actualTriangleCount = 0u;
	std::uint32_t actualMeshletCount = 0u;
	std::uint32_t nearestGeneratedLevelIndex = 0u;
	std::uint32_t triangleCount = 0u;
	std::uint32_t meshletCount = 0u;
	std::string message;
};

struct VirtualGeometryClusterSummaryMetadata {
	std::uint32_t clusterCount = 0u;
	std::uint32_t selectedClusterCount = 0u;
	std::uint32_t fallbackClusterCount = 0u;
	std::uint32_t materialSectionCount = 0u;
	std::uint32_t clusterMaterialSectionCount = 0u;
	bool fallbackCoverageValid = false;
	bool materialCoverageValid = false;
};

struct VirtualGeometryPageSummaryMetadata {
	std::uint32_t pageCount = 0u;
	std::uint32_t residentPageCount = 0u;
	std::uint32_t pinnedFallbackPageCount = 0u;
	std::uint64_t estimatedBytes = 0u;
	bool pageStreamingReady = false;
	bool smallMeshStreamingSkipped = false;
	std::string skippedStreamingReason;
	std::vector<std::string> integrityErrors;
};

// AssetImportProgress is a copyable UI snapshot for one background import.
// Worker threads update the matching internal state, and editor windows read
// this snapshot so the frame loop can display progress without blocking.
struct AssetImportProgress {
	std::filesystem::path sourcePath;
	std::string importerName;
	std::string stage;
	std::string message;
	float progress = 0.0f;
	bool indeterminate = true;
	bool failed = false;
	bool cancelRequested = false;
	double elapsedSeconds = 0.0;
};

// AssetOrganizationImpact is the editor-owned preflight for an asset move,
// rename, or duplication. Keeping canonical paths and the repair inventory in
// AssetDatabase ensures UI and automation callers apply the same safety rules.
struct AssetOrganizationImpact {
	bool valid = false;
	std::string errorCategory;
	std::string error;
	std::string source;
	std::string destination;
	std::string fingerprint;
	std::vector<std::string> dependents;
	std::vector<std::string> generatedChildren;
	std::vector<std::string> repairPlan;
	std::size_t rollbackJournalEstimate = 0u;
};

// AssetOrganizationResult describes protected publication without exposing
// filesystem implementation details to MCP or future editor surfaces.
struct AssetOrganizationResult {
	bool published = false;
	std::string errorCategory;
	std::string error;
	std::string path;
	std::string assetId;
	std::vector<std::string> repairedReferences;
	std::string rollback = "not_started";
};

enum class GeneratedModelPackageChildKind {
	Mesh,
	Material,
	Texture,
	// Append animation package-child kinds so the original mesh/material/texture
	// numeric values remain stable for editor code that persists the selection.
	Skeleton,
	AnimationClip,
	MorphTarget,
	DeformableMesh,
	Prefab
};

enum class AnimationReimportState : std::uint8_t {
	Queued,
	Compiling,
	Registered,
	Published,
	RetainedLastKnownGood,
	Cancelled,
	Failed
};

// This bounded main-thread publication record lets Inspector/automation report
// whether a watched animation source became active or deliberately retained its
// last-known-good generation. It never exposes mutable cache pointers.
struct AnimationReimportStatus {
	std::filesystem::path sourcePath;
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	TerraEngine::Animation::AnimationReloadAssetType assetType =
		TerraEngine::Animation::AnimationReloadAssetType::AnimationClip;
	AnimationReimportState state = AnimationReimportState::Queued;
	std::uint64_t candidateGeneration = 0u;
	std::uint64_t activeGeneration = 0u;
	std::uint64_t publicationBoundary = 0u;
	std::uint64_t sequence = 0u;
	bool lastKnownGoodPreserved = false;
	std::string diagnostic;
};

// GeneratedModelPackageChildMetadata is a read-only view over one package
// manifest record. It deliberately does not become AssetMetadata: package
// children share the source package's lifecycle and must not acquire editable
// loose-asset metadata or independent .meta files.
struct GeneratedModelPackageChildMetadata {
	TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
	GeneratedModelPackageChildKind kind = GeneratedModelPackageChildKind::Mesh;
	std::string displayName;
	std::string semanticKey;
	std::filesystem::path sourcePath;
	std::filesystem::path compiledPath;
	std::uint64_t compatibilityFingerprint = 0u;
	std::uint64_t publishedGeneration = 0u;
	bool readOnlyPackageChild = true;
	std::vector<TerraEngine::UUID> dependencies;
};

// ModelPackageAnimationSummaryMetadata supplies Inspector, Asset Browser, and
// automation with the same generation and validation evidence. Diagnostics are
// concise stable-code messages so callers do not need to parse importer logs.
struct ModelPackageAnimationSummaryMetadata {
	bool readable = false;
	bool publishedAtomically = false;
	bool preservedLastKnownGood = false;
	bool prefabBindingsValid = true;
	std::uint32_t schemaVersion = 0u;
	std::uint64_t candidateGeneration = 0u;
	std::uint64_t publishedGeneration = 0u;
	std::uint64_t lastKnownGoodGeneration = 0u;
	std::size_t generatedChildCount = 0u;
	std::size_t readOnlyChildCount = 0u;
	std::size_t skeletonCount = 0u;
	std::size_t animationClipCount = 0u;
	std::size_t deformableMeshCount = 0u;
	std::size_t morphTargetCount = 0u;
	std::size_t prefabCount = 0u;
	std::size_t infoCount = 0u;
	std::size_t warningCount = 0u;
	std::size_t errorCount = 0u;
	std::string dependencyFingerprint;
	std::vector<std::string> diagnostics;
};

// class AssetDatabase groups related state and operations so callers have one documented contract.
class AssetDatabase {
public:
	static AssetDatabase& Instance();

	void Initialize(const std::filesystem::path& projectRoot);
	void Shutdown();
	void Update(); // Main thread update to process file events

	// Scans project initially
	void ScanProject();

	// Explicitly import an asset
	void ImportAsset(const std::filesystem::path& sourcePath, bool force = false);
	// ImportAssetWithProgress keeps source metadata work on the caller thread,
	// then moves supported expensive compile steps to a worker and publishes
	// their results during Update on the main editor thread.
	void ImportAssetWithProgress(const std::filesystem::path& sourcePath, bool force = false);
	// Explicit automation imports require read-after-write behavior. Call this
	// only at the editor's protected main-thread boundary: it waits for and
	// finalizes the active worker for this one source path without draining jobs
	// for any other asset, then discards this path's coalesced follow-up request.
	void FinalizeActiveImportForExplicitReimport(const std::filesystem::path& sourcePath);

	// Delete an asset from disk and unregister it
	void DeleteAsset(const std::filesystem::path& sourcePath);

	// Getters
	// GetAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::map<std::filesystem::path, AssetMetadata>& GetAssets() const { return assets; }
	std::filesystem::path GetAssetRoot() const { return assetRoot; }
	std::filesystem::path GetBuildRoot() const { return buildRoot; }
	const AssetMetadata* FindAssetByGuid(const TerraEngine::UUID& guid) const;
	// Physics reads this immutable value after the asset database has completed
	// registration. No editor metadata or filesystem handle crosses the boundary.
	TerraEngine::Physics::PhysicsAssetChangeSnapshot GetPhysicsAssetChangesSince(
		TerraEngine::Physics::PhysicsAssetGeneration observedGeneration) const;
	TerraEngine::Physics::PhysicsAssetGeneration GetPhysicsAssetChangeGeneration() const;
	bool TryGetGeneratedModelPackageChildDisplayName(const TerraEngine::UUID& guid, GeneratedModelPackageChildKind kind, std::string& outName) const;
	bool TryGetGeneratedModelPackageChildIds(const TerraEngine::UUID& packageGuid, GeneratedModelPackageChildKind kind, std::vector<TerraEngine::UUID>& outChildIds) const;
	bool TryGetGeneratedModelPackageChildMetadata(const TerraEngine::UUID& guid, GeneratedModelPackageChildKind kind, GeneratedModelPackageChildMetadata& outMetadata) const;
	bool TryGetModelPackageAnimationSummary(const AssetMetadata& metadata, ModelPackageAnimationSummaryMetadata& outSummary) const;
	bool IsMeshAsset(const AssetMetadata& metadata) const;
	bool TryGetVirtualGeometryMetadata(const AssetMetadata& metadata, VirtualGeometryAssetMetadata& outMetadata) const;
	VirtualGeometryAdaptiveSettingsMetadata GetDefaultVirtualGeometryAdaptiveSettings() const;
	bool CanEditVirtualGeometryPerMeshOverride(const AssetMetadata& metadata) const;
	bool TryGetVirtualGeometryAdaptiveSettings(const AssetMetadata& metadata, VirtualGeometryAdaptiveSettingsMetadata& outSettings) const;
	bool HasVirtualGeometryPerMeshOverride(const AssetMetadata& metadata) const;
	bool SaveVirtualGeometryPerMeshOverride(const TerraEngine::UUID& assetGuid, const VirtualGeometryAdaptiveSettingsMetadata& settings, std::string* outError = nullptr);
	bool ClearVirtualGeometryPerMeshOverride(const TerraEngine::UUID& assetGuid, std::string* outError = nullptr);
	bool RebuildVirtualGeometryAsset(const TerraEngine::UUID& assetGuid, std::string* outError = nullptr);
	bool TryGetGeneratedMeshVirtualGeometryAdaptiveSettings(const std::filesystem::path& packagePath, const TerraEngine::UUID& meshGuid, VirtualGeometryAdaptiveSettingsMetadata& outSettings) const;
	bool SaveGeneratedMeshVirtualGeometryPerMeshOverride(const std::filesystem::path& packagePath, const TerraEngine::UUID& meshGuid, const VirtualGeometryAdaptiveSettingsMetadata& settings, std::string* outError = nullptr);
	bool ClearGeneratedMeshVirtualGeometryPerMeshOverride(const std::filesystem::path& packagePath, const TerraEngine::UUID& meshGuid, std::string* outError = nullptr);
	bool RebuildGeneratedMeshVirtualGeometryAsset(const std::filesystem::path& packagePath, std::string* outError = nullptr);
	bool TryGetVirtualGeometryDetailDiagnostics(const AssetMetadata& metadata, std::vector<VirtualGeometryDetailDiagnosticMetadata>& outDiagnostics) const;
	bool TryGetVirtualGeometryClusterSummary(const AssetMetadata& metadata, VirtualGeometryClusterSummaryMetadata& outSummary) const;
	bool TryGetVirtualGeometryPageSummary(const AssetMetadata& metadata, VirtualGeometryPageSummaryMetadata& outSummary) const;
	const AssetMetadata* FindScriptAssetByFullName(std::string_view fullName) const;
	std::string GetScriptFullName(const TerraEngine::UUID& guid) const;
	void SetScriptCompileState(const std::string& compileState, const std::string& summary);

	// Preflight and publication are main-thread protected operations. Expensive
	// reference discovery may be prepared earlier, but callers must publish at
	// the editor automation boundary so file and index state become visible
	// together.
	AssetOrganizationImpact PrepareAssetOrganization(
		const std::filesystem::path& sourceProjectRelative,
		std::string_view operation,
		const std::filesystem::path& destinationProjectRelative = {}) const;
	AssetOrganizationResult PublishAssetOrganization(
		const std::filesystem::path& sourceProjectRelative,
		const std::filesystem::path& destinationProjectRelative,
		std::string_view operation,
		std::string_view approvedImpactFingerprint = {});

	// IsCompiling is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsCompiling() const;
	bool IsAssetCompiling(const std::filesystem::path& absSourcePath) const;
	std::vector<AssetImportProgress> GetImportProgressSnapshot() const;
	std::optional<AssetImportProgress> GetImportProgress(const std::filesystem::path& absSourcePath) const;
	std::vector<AnimationReimportStatus> GetAnimationReimportStatusSnapshot() const;
	std::optional<AnimationReimportStatus> GetLatestAnimationReimportStatus(
		const std::filesystem::path& sourcePath) const;
	bool CancelImport(const std::filesystem::path& absSourcePath, bool removeAssetAfterCancel = true);
	void RequestCancelAllImports(bool removeAssetsAfterCancel = true);
	void CancelAllImportsAndWait(bool removeAssetsAfterCancel = true);
	size_t GetCompileJobsCount() const;

#ifdef TERRAENGINE_EDITOR_TESTS
	// Editor tests install this worker hook to hold an import at a deterministic
	// cancellation boundary. A missing hook keeps the production compiler path.
	using AsyncImportCompileHookForTests = std::function<bool(
		const AssetMetadata& metadata,
		const std::atomic_bool& cancellationRequested)>;
	void SetAsyncImportCompileHookForTests(AsyncImportCompileHookForTests hook);
	// This hook runs after the complete package publisher batch is staged but
	// before ResourceManager reparses live replacements. It lets integration
	// tests introduce a deterministic validation/commit race without adding a
	// production failure switch.
	using AnimationPackagePostPublisherPrepareHookForTests = std::function<void()>;
	void SetAnimationPackagePostPublisherPrepareHookForTests(
		AnimationPackagePostPublisherPrepareHookForTests hook);
	[[nodiscard]] std::optional<TerraEngine::Animation::AnimationReloadGenerationView>
		GetAnimationReloadGenerationForTests(const TerraEngine::UUID& assetId) const;
#endif

private:
	AssetDatabase();
	~AssetDatabase();

	// File Event Handling
	void OnFileEvent(const std::filesystem::path& path, FileStatus status);
	
	// Metadata IO
	bool LoadMetadata(const std::filesystem::path& metaFilePath, AssetMetadata& outMetadata);
	bool SaveMetadata(const AssetMetadata& metadata);
	void GenerateDefaultMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata);
	void UpdateScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& metadata);
	void PruneRecentlyRemovedAssets();
	bool TryReuseRecentlyRemovedScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata);
	struct AsyncImportProgressState;
	
	std::string GetImporterName(const std::filesystem::path& filePath) const;
	bool ShouldCompileAssetAsync(const std::string& importerName) const;
	void ImportAssetInternal(const std::filesystem::path& sourcePath, bool force, bool allowAsyncCompile);
	bool CompileAsset(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState = nullptr, std::optional<VirtualGeometryAdaptiveSettingsMetadata> resolvedMeshSettings = std::nullopt);
	bool CompileModelPackageWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState);
	bool CompileMeshWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState, std::optional<VirtualGeometryAdaptiveSettingsMetadata> resolvedMeshSettings);
	bool CompileTextureWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState);
	bool CompileShaderWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState);
	bool CompilePhysicsColliderWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState);
	bool RunCompilerProcess(const std::filesystem::path& compilerPath, const std::wstring& commandLine, const std::shared_ptr<AsyncImportProgressState>& progressState, std::string_view logPrefix, const std::filesystem::path& expectedOutputPath);
	// Publication and cancellation share one short mutex boundary. Whichever
	// operation wins becomes authoritative, so a cancelled worker cannot publish
	// its private staging file after the caller has received a cancellation result.
	bool PublishCompiledAssetFromWorker(
		const std::filesystem::path& stagedPath,
		const std::filesystem::path& finalPath,
		const std::shared_ptr<AsyncImportProgressState>& progressState) const;
	bool MarkImportPublicationComplete(const std::shared_ptr<AsyncImportProgressState>& progressState) const;
	bool RequestImportCancellation(const std::shared_ptr<AsyncImportProgressState>& progressState, bool removeAssetAfterCancel) const;
	// Shutdown drains successful jobs through the normal main-thread publication
	// path. Jobs explicitly cancelled earlier still follow their chosen cleanup mode.
	void DrainAllImportsAndWait();
	void FinishImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport);
	void RegisterImportedAsset(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport);
	void PublishPhysicsAssetChange(
		const AssetMetadata& metadata,
		TerraEngine::Physics::PhysicsAssetChangeKind changeKind);
	void RegisterGeneratedModelPackageMaterialMetadata(const std::filesystem::path& absSourcePath);
	void RegisterGeneratedModelPackageResources(const std::filesystem::path& absSourcePath);
	void UpdateModelPackageAnimationSummary(const std::filesystem::path& absSourcePath);
	void PublishGeneratedAnimationHotReload(const std::filesystem::path& absSourcePath);
	void RecordAnimationReimportStatus(AnimationReimportStatus status);
	void CleanupModelPackageGeneratedAssets(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath);
	void CleanupCancelledImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool removeSourceAsset);
	void RemoveAsset(const std::filesystem::path& sourcePath);
	bool LoadAssetIndex();
	bool SaveAssetIndex() const;
	void RequestAssetIndexSave();
	void StartBackgroundProjectReconcile();
	void PublishBackgroundProjectReconcile();
	void ProcessPendingProjectReconcile();
	void SetImportProgress(const std::shared_ptr<AsyncImportProgressState>& progressState, std::string stage, std::string message, float progress, bool indeterminate, bool failed) const;
	void TerminateCompilerProcess(const std::shared_ptr<AsyncImportProgressState>& progressState) const;
	AssetImportProgress SnapshotImportProgress(const std::shared_ptr<AsyncImportProgressState>& progressState) const;

	std::filesystem::path projectRoot;
	std::filesystem::path assetRoot; 
	std::filesystem::path buildRoot; 

	std::map<std::filesystem::path, AssetMetadata> assets;
	// The sidecar is durable identity; this table only tracks which package-owned
	// runtime aliases this editor session published so a reimport/delete can
	// unregister removed typed children without inventing loose AssetMetadata.
	std::map<TerraEngine::UUID, std::vector<std::pair<TerraEngine::UUID, GeneratedModelPackageChildKind>>> generatedModelPackageResources;
	// This editor-session publisher runs only after the complete generated
	// package alias set is visible, so failed source candidates retain the
	// previous evaluator generation instead of exposing partial output.
	TerraEngine::Animation::AnimationHotReloadPublisher animationHotReloadPublisher;
	std::filesystem::path assetIndexPath;
	// Startup index registration can discover generated package children. Their
	// normal FinishImport path requests persistence, but those requests must be
	// batched until the original index has closed and every entry is registered.
	bool loadingAssetIndex = false;
	bool assetIndexSaveDeferred = false;
	
	// File Watcher
	FileWatcher fileWatcher;
	bool watcherReconcilePending = false;
	mutable std::mutex animationReimportStatusMutex;
	std::deque<AnimationReimportStatus> animationReimportStatuses;
	std::uint64_t nextAnimationReimportStatusSequence = 0u;
	// A publication survives at least one subsequent editor Update before its
	// replaced generation becomes retirement-eligible. Explicit evaluator/GPU
	// retains in the publisher can extend that lifetime further.
	std::uint64_t animationReloadBoundaryToRetireNextUpdate = 0u;
	
	// Debouncing
	// struct PendingEvent groups related state and operations so callers have one documented contract.
	struct PendingEvent {
		FileStatus status;
		float timer; // Seconds until process
	};
	std::map<std::filesystem::path, PendingEvent> pendingEvents;
	std::map<std::filesystem::path, bool> deferredImports;

	struct ProjectReconcileSnapshot {
		std::vector<std::filesystem::path> sourceFiles;
		std::vector<std::filesystem::path> orphanedMetaFiles;
	};
	std::future<ProjectReconcileSnapshot> projectReconcileFuture;
	std::deque<std::filesystem::path> pendingReconcileImports;
	std::vector<std::filesystem::path> pendingReconcileOrphanedMetas;

	// struct AsyncCompileJob groups related state and operations so callers have one documented contract.
	struct AsyncImportProgressState {
		mutable std::mutex mutex;
		AssetImportProgress snapshot;
		std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
		std::atomic_bool cancelRequested = false;
		std::atomic_bool removeAssetAfterCancel = false;
		mutable std::mutex publicationMutex;
		bool publicationCompleted = false;
		mutable std::mutex processMutex;
		void* compilerProcessHandle = nullptr;
	};

	struct AsyncCompileJob {
		AssetMetadata metadata;
		std::future<bool> future;
		bool needsImport;
		std::filesystem::path absSourcePath;
		std::shared_ptr<AsyncImportProgressState> progressState;
	};
	// Update, shutdown drain, and explicit same-path synchronization share the
	// same result handling so failure/cancellation cannot publish differently.
	void FinalizeAsyncImportJob(AsyncCompileJob& job);
	std::vector<AsyncCompileJob> asyncJobs;

#ifdef TERRAENGINE_EDITOR_TESTS
	mutable std::mutex asyncImportCompileHookMutex;
	AsyncImportCompileHookForTests asyncImportCompileHookForTests;
	mutable std::mutex animationPackageTestHookMutex;
	AnimationPackagePostPublisherPrepareHookForTests animationPackagePostPublisherPrepareHookForTests;
#endif

	// struct RecentlyRemovedAsset groups related state and operations so callers have one documented contract.
	struct RecentlyRemovedAsset {
		AssetMetadata metadata;
		std::filesystem::path absSourcePath;
		std::chrono::steady_clock::time_point expiresAt;
	};
	std::vector<RecentlyRemovedAsset> recentlyRemovedAssets;
};
