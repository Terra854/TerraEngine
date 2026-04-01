#pragma once

#include "Utilities/UUID.h"
#include "FileWatcher.h"
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <future>
#include <nlohmann/json.hpp>

struct AssetMetadata {
    TerraEngine::UUID guid;
    std::filesystem::file_time_type lastModified;
    std::string importerName; 
    std::filesystem::path sourcePath;   
    std::filesystem::path compiledPath; 
    nlohmann::json settings; // Importer-specific settings
};

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
    const std::map<std::filesystem::path, AssetMetadata>& GetAssets() const { return assets; }
    std::filesystem::path GetAssetRoot() const { return assetRoot; }
    std::filesystem::path GetBuildRoot() const { return buildRoot; }

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
    struct PendingEvent {
        FileStatus status;
        float timer; // Seconds until process
    };
    std::map<std::filesystem::path, PendingEvent> pendingEvents;

    struct AsyncCompileJob {
        AssetMetadata metadata;
        std::future<bool> future;
        bool needsImport;
        std::filesystem::path absSourcePath;
    };
    std::vector<AsyncCompileJob> asyncJobs;
};
