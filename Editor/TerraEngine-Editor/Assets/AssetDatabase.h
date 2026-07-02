// AssetDatabase.h — Editor asset database management
//
// Defines the interface for the editor's asset database, responsible for tracking source assets, managing their metadata, and coordinating compilation. it handles asset GUID lookups, importer mapping, and provides a central registry for all project resources.

#pragma once

#include "Utilities/UUID.h"
#include "FileWatcher.h"
#include <filesystem>
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
	bool coneSafetyMetadataValid = false;
	std::string rebuildGuidance;
	std::vector<std::string> warnings;
	std::vector<std::string> coneDisabledReasons;
};

struct VirtualGeometryAdaptiveSettingsMetadata {
	bool valid = true;
	bool overridePresent = false;
	std::string settingsSource = "ProjectDefaults";
	std::vector<float> targetRatios;
	std::uint32_t minimumSourceTriangles = 512u;
	std::uint32_t minimumGeneratedTriangles = 64u;
	std::uint32_t minimumMeshletsPerGeneratedLevel = 2u;
	float minimumUsefulTriangleReduction = 0.15f;
	float minimumUsefulMeshletReduction = 0.15f;
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

	// Delete an asset from disk and unregister it
	void DeleteAsset(const std::filesystem::path& sourcePath);

	// Getters
	// GetAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::map<std::filesystem::path, AssetMetadata>& GetAssets() const { return assets; }
	std::filesystem::path GetAssetRoot() const { return assetRoot; }
	std::filesystem::path GetBuildRoot() const { return buildRoot; }
	const AssetMetadata* FindAssetByGuid(const TerraEngine::UUID& guid) const;
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

	// IsCompiling is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsCompiling() const;
	bool IsAssetCompiling(const std::filesystem::path& absSourcePath) const;
	std::vector<AssetImportProgress> GetImportProgressSnapshot() const;
	std::optional<AssetImportProgress> GetImportProgress(const std::filesystem::path& absSourcePath) const;
	bool CancelImport(const std::filesystem::path& absSourcePath, bool removeAssetAfterCancel = true);
	void RequestCancelAllImports(bool removeAssetsAfterCancel = true);
	void CancelAllImportsAndWait(bool removeAssetsAfterCancel = true);
	size_t GetCompileJobsCount() const;

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
	bool RunCompilerProcess(const std::filesystem::path& compilerPath, const std::wstring& commandLine, const std::shared_ptr<AsyncImportProgressState>& progressState, std::string_view logPrefix, const std::filesystem::path& expectedOutputPath);
	void FinishImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport);
	void RegisterGeneratedModelPackageMaterialMetadata(const std::filesystem::path& absSourcePath);
	void CleanupModelPackageGeneratedAssets(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath);
	void CleanupCancelledImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool removeSourceAsset);
	void RemoveAsset(const std::filesystem::path& sourcePath);
	void SetImportProgress(const std::shared_ptr<AsyncImportProgressState>& progressState, std::string stage, std::string message, float progress, bool indeterminate, bool failed) const;
	void TerminateCompilerProcess(const std::shared_ptr<AsyncImportProgressState>& progressState) const;
	AssetImportProgress SnapshotImportProgress(const std::shared_ptr<AsyncImportProgressState>& progressState) const;

	std::filesystem::path projectRoot;
	std::filesystem::path assetRoot; 
	std::filesystem::path buildRoot; 

	std::map<std::filesystem::path, AssetMetadata> assets;
	
	// File Watcher
	FileWatcher fileWatcher;
	
	// Debouncing
	// struct PendingEvent groups related state and operations so callers have one documented contract.
	struct PendingEvent {
		FileStatus status;
		float timer; // Seconds until process
	};
	std::map<std::filesystem::path, PendingEvent> pendingEvents;
	std::map<std::filesystem::path, bool> deferredImports;

	// struct AsyncCompileJob groups related state and operations so callers have one documented contract.
	struct AsyncImportProgressState {
		mutable std::mutex mutex;
		AssetImportProgress snapshot;
		std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
		std::atomic_bool cancelRequested = false;
		std::atomic_bool removeAssetAfterCancel = false;
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
	std::vector<AsyncCompileJob> asyncJobs;

	// struct RecentlyRemovedAsset groups related state and operations so callers have one documented contract.
	struct RecentlyRemovedAsset {
		AssetMetadata metadata;
		std::filesystem::path absSourcePath;
		std::chrono::steady_clock::time_point expiresAt;
	};
	std::vector<RecentlyRemovedAsset> recentlyRemovedAssets;
};
