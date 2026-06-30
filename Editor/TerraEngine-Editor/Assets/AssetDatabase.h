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
#include <nlohmann/json.hpp>
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
	std::string rebuildGuidance;
	std::vector<std::string> warnings;
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

	// Delete an asset from disk and unregister it
	void DeleteAsset(const std::filesystem::path& sourcePath);

	// Getters
	// GetAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::map<std::filesystem::path, AssetMetadata>& GetAssets() const { return assets; }
	std::filesystem::path GetAssetRoot() const { return assetRoot; }
	std::filesystem::path GetBuildRoot() const { return buildRoot; }
	const AssetMetadata* FindAssetByGuid(const TerraEngine::UUID& guid) const;
	bool TryGetVirtualGeometryMetadata(const AssetMetadata& metadata, VirtualGeometryAssetMetadata& outMetadata) const;
	const AssetMetadata* FindScriptAssetByFullName(std::string_view fullName) const;
	std::string GetScriptFullName(const TerraEngine::UUID& guid) const;
	void SetScriptCompileState(const std::string& compileState, const std::string& summary);

	// IsCompiling is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsCompiling() const { return !asyncJobs.empty(); }
	bool IsAssetCompiling(const std::filesystem::path& absSourcePath) const;
	size_t GetCompileJobsCount() const { return asyncJobs.size(); }

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
	
	std::string GetImporterName(const std::filesystem::path& filePath) const;
	bool CompileAsset(const AssetMetadata& metadata);
	void FinishImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport);
	void RemoveAsset(const std::filesystem::path& sourcePath);

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
	struct AsyncCompileJob {
		AssetMetadata metadata;
		std::future<bool> future;
		bool needsImport;
		std::filesystem::path absSourcePath;
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
