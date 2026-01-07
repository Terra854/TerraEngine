#include "AssetDatabase.h"
#include "Engine/Core/Logger/Logger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cstdlib>

#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "Editor/AssetCompilers/ShaderCompiler.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Renderer/RenderSystem.h"

using json = nlohmann::json;

namespace {
    std::string UuidToString(const TerraEngine::UUID& uuid) {
        std::ostringstream oss;
        for (const auto& byte : uuid) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
        }
        return oss.str();
    }

    TerraEngine::UUID StringToUuid(const std::string& str) {
        TerraEngine::UUID uuid;
        if (str.length() != uuid.size() * 2) return TerraEngine::EMPTY_UUID;
        for (size_t i = 0; i < uuid.size(); ++i) {
            uuid[i] = static_cast<std::uint8_t>(std::stoul(str.substr(i * 2, 2), nullptr, 16));
        }
        return uuid;
    }
}

AssetDatabase& AssetDatabase::Instance() {
    static AssetDatabase instance;
    return instance;
}

AssetDatabase::AssetDatabase() {}
AssetDatabase::~AssetDatabase() {}

void AssetDatabase::Initialize(const std::filesystem::path& projectRootPath) {
    this->projectRoot = std::filesystem::absolute(projectRootPath);
    this->assetRoot = this->projectRoot / "Assets";
    this->buildRoot = this->projectRoot / "Build" / "Assets";

    if (!std::filesystem::exists(assetRoot)) std::filesystem::create_directories(assetRoot);
    if (!std::filesystem::exists(buildRoot)) std::filesystem::create_directories(buildRoot);

    TERRAENGINE_LOG(LogLevel::Info, "AssetDatabase initialized. Watching: " << assetRoot);
    
    // Initial Scan
    ScanProject();
    
    // Start Watcher
    fileWatcher.Start(assetRoot);
}

void AssetDatabase::Shutdown() {
    fileWatcher.Stop();
    assets.clear();
}

void AssetDatabase::Update() {
    // Process Watcher Events
    auto events = fileWatcher.PopEvents();
    for (const auto& e : events) {
        // Debounce: update or add entry in pending
        // Debounce time: 200ms
        pendingEvents[e.path] = { e.status, 0.2f };
    }

    // Process Pending
    // We assume Update is called with some frequency. 
    float dt = 0.016f; 
    
    auto it = pendingEvents.begin();
    while (it != pendingEvents.end()) {
        it->second.timer -= dt;
        if (it->second.timer <= 0.0f) {
            OnFileEvent(it->first, it->second.status);
            it = pendingEvents.erase(it);
        } else {
            ++it;
        }
    }
}

void AssetDatabase::OnFileEvent(const std::filesystem::path& path, FileStatus status) {
    // Ignore .meta files and directories
    if (path.extension() == ".meta") return;
    // Check directory only if it exists (for created/modified)
    if ((status == FileStatus::Created || status == FileStatus::Modified) && std::filesystem::is_directory(path)) return;

    if (status == FileStatus::Created || status == FileStatus::Modified) {
        if (std::filesystem::exists(path)) {
            ImportAsset(path);
        }
    } else if (status == FileStatus::Erased) {
        RemoveAsset(path);
    }
}

void AssetDatabase::ScanProject() {
    // 1. Cleanup orphaned .meta files
    if (std::filesystem::exists(assetRoot)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
            if (entry.path().extension() == ".meta") {
                std::filesystem::path sourcePath = entry.path();
                sourcePath.replace_extension(""); // Remove .meta extension to get source
                
                if (!std::filesystem::exists(sourcePath)) {
                    // Orphaned meta found
                    AssetMetadata tempMeta;
                    // Try to load to delete compiled asset
                    if (LoadMetadata(entry.path(), tempMeta)) {
                        if (!tempMeta.compiledPath.empty()) {
                            std::filesystem::path fullCompiled = buildRoot / tempMeta.compiledPath;
                            if (std::filesystem::exists(fullCompiled)) std::filesystem::remove(fullCompiled);
                        }
                    }
                    // Delete the meta file
                    std::filesystem::remove(entry.path());
                }
            }
        }
    }

    // 2. Import existing assets (Sync DB with Disk)
    std::map<std::filesystem::path, AssetMetadata> currentAssets;
    if (std::filesystem::exists(assetRoot)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
            if (entry.is_regular_file()) {
                if (entry.path().extension() == ".meta") continue;
                ImportAsset(entry.path());
            }
        }
    }
}

void AssetDatabase::ImportAsset(const std::filesystem::path& sourcePath) {
    if (sourcePath.extension() == ".meta") return;
    if (std::filesystem::is_directory(sourcePath)) return;

    AssetMetadata metadata;
    std::filesystem::path metaFilePath = sourcePath.string() + ".meta";
    
    bool needsImport = false;
    
    if (std::filesystem::exists(metaFilePath)) {
        if (LoadMetadata(metaFilePath, metadata)) {
            // Check timestamp
            auto currentModTime = std::filesystem::last_write_time(sourcePath);
            if (currentModTime > metadata.lastModified) {
                needsImport = true;
                metadata.lastModified = currentModTime;
            }
        } else {
            // Meta corrupt, regenerate
            GenerateDefaultMetadata(sourcePath, metadata);
            needsImport = true;
        }
    } else {
        GenerateDefaultMetadata(sourcePath, metadata);
        needsImport = true;
    }

    // Determine Compiled Path (Relative)
    std::filesystem::path relativeSourcePath = std::filesystem::relative(sourcePath, assetRoot);
    std::filesystem::path compiledRelativePath = relativeSourcePath;
    
    if (metadata.importerName == "MeshImporter") compiledRelativePath.replace_extension(".temf");
    else if (metadata.importerName == "TextureImporter") compiledRelativePath.replace_extension(".tetf");
    else if (metadata.importerName == "ShaderImporter") compiledRelativePath.replace_extension(".tspf");
    else if (metadata.importerName == "MaterialImporter") compiledRelativePath.replace_extension(".tmat");
    else if (metadata.importerName == "GenericImporter") compiledRelativePath.clear(); 
    
    // Store relative path in metadata
    metadata.compiledPath = compiledRelativePath;

    if (!metadata.compiledPath.empty()) {
        // Resolve Absolute Paths for checking/compiling
        // Use buildRoot to ensure path includes Assets/
        std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
        std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;

        if (needsImport || !std::filesystem::exists(fullCompiledPath)) {
            if (!std::filesystem::exists(fullCompiledPath.parent_path())) {
                std::filesystem::create_directories(fullCompiledPath.parent_path());
            }
            
            bool compiled = CompileAsset(metadata);
            
            if (!compiled) {
                TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << fullSourcePath);
            }
        }
    }
    
    // Always save metadata
    SaveMetadata(metadata);

    assets[sourcePath] = metadata;

    // Register with Engine ResourceManager so it can resolve GUIDs to compiled paths
    if (!metadata.compiledPath.empty()) {
        AssetType type;
        bool known = false;
        if (metadata.importerName == "MeshImporter") { type = AssetType::Mesh; known = true; }
        else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; known = true; }
        else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; known = true; }
        else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; known = true; }

        if (known) {
            std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
            ResourceManager::Instance().RegisterAssetPath(metadata.guid, fullCompiled, type);

            if (needsImport) {
                if (type == AssetType::Material) {
                    RenderSystem::Instance().ReloadMaterial(metadata.guid);
                } else if (type == AssetType::Mesh) {
                    RenderSystem::Instance().ReloadMesh(metadata.guid);
                } else if (type == AssetType::Texture) {
                    RenderSystem::Instance().ReloadTexture(metadata.guid);
                }
            }
        }
    }
}

void AssetDatabase::GenerateDefaultMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
    outMetadata.guid = TerraEngine::GenerateUUID();
    outMetadata.lastModified = std::filesystem::last_write_time(sourcePath);
    outMetadata.importerName = GetImporterName(sourcePath);
    outMetadata.sourcePath = std::filesystem::relative(sourcePath, assetRoot);
    outMetadata.settings = json::object(); 
}

std::string AssetDatabase::GetImporterName(const std::filesystem::path& filePath) const {
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    
    if (ext == ".obj" || ext == ".fbx") return "MeshImporter";
    if (ext == ".png" || ext == ".jpg" || ext == ".tga" || ext == ".bmp") return "TextureImporter";
    if (ext == ".hlsl" || ext == ".glsl") return "ShaderImporter";
    if (ext == ".tmat") return "MaterialImporter";
    return "GenericImporter";
}

bool AssetDatabase::LoadMetadata(const std::filesystem::path& metaFilePath, AssetMetadata& outMetadata) {
	std::ifstream file(metaFilePath);
	if (!file.is_open()) return false;
	try {
		json j;
		file >> j;
		outMetadata.guid = StringToUuid(j["guid"].get<std::string>());
		outMetadata.importerName = j["importerName"].get<std::string>();
		using FileDuration = std::filesystem::file_time_type::duration;
		// Keep the raw duration units intact so we compare the same clock ticks recorded on disk.
		FileDuration::rep storedTicks = j["lastModified"].get<FileDuration::rep>();
		outMetadata.lastModified = std::filesystem::file_time_type(FileDuration(storedTicks));
		outMetadata.sourcePath = j["sourcePath"].get<std::string>();
		outMetadata.compiledPath = j["compiledPath"].get<std::string>();
		if (j.contains("settings")) outMetadata.settings = j["settings"];
		return true;
	} catch (...) { return false; }
}

bool AssetDatabase::SaveMetadata(const AssetMetadata& metadata) {
	std::filesystem::path metaFilePath = (assetRoot / metadata.sourcePath).string() + ".meta";
	std::ofstream file(metaFilePath);
	if (!file.is_open()) return false;
	try {
		json j;
		j["guid"] = UuidToString(metadata.guid);
		j["importerName"] = metadata.importerName;
		// Persist the file clock's native tick count so reloads compare identical units and avoid false recompiles.
		j["lastModified"] = metadata.lastModified.time_since_epoch().count();
		j["sourcePath"] = metadata.sourcePath.string();
		j["compiledPath"] = metadata.compiledPath.string();
		j["settings"] = metadata.settings;
		file << std::setw(4) << j << std::endl;
		return true;
    } catch (...) { return false; }
}

bool AssetDatabase::CompileAsset(const AssetMetadata& metadata) {
    if (metadata.compiledPath.empty()) return true;

    // Resolve full paths using buildRoot
    std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
    std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
    
    fullSourcePath.make_preferred();
    fullCompiledPath.make_preferred();

    TERRAENGINE_LOG(LogLevel::Info, "Compiling Asset via Internal Compiler: " << fullSourcePath);

    if (metadata.importerName == "MeshImporter") {
        return CompileMesh(fullSourcePath, fullCompiledPath);
    } 
    else if (metadata.importerName == "TextureImporter") {
        std::string semanticStr = "color";
        if (metadata.settings.contains("semantic")) semanticStr = metadata.settings["semantic"];
        
        TextureFileSemantic semantic = TextureFileSemantic::Color;
        if (semanticStr == "data") semantic = TextureFileSemantic::Data;
        
        return CompileTexture(fullSourcePath, fullCompiledPath, semantic);
    } 
    else if (metadata.importerName == "ShaderImporter") {
        std::string stage = "vs";
        if (metadata.settings.contains("stage")) stage = metadata.settings["stage"];
        else { 
             std::string fn = fullSourcePath.filename().string();
             if (fn.find("frag") != std::string::npos || fn.find("pixel") != std::string::npos) stage = "ps";
        }
        std::string target = "";
        if (metadata.settings.contains("target")) target = metadata.settings["target"];
        std::string entry = "main";
        if (metadata.settings.contains("entryPoint")) entry = metadata.settings["entryPoint"];

        // Find DXC
        std::filesystem::path toolsDir = projectRoot / "out/build/Build/Debug"; 
        
        // Try relative to project root
        if (!std::filesystem::exists(toolsDir / "dxc.exe") && !std::filesystem::exists(toolsDir / "dxc")) {
             toolsDir = std::filesystem::current_path();
        }
        // Try specific VS18 path
        if (!std::filesystem::exists(toolsDir / "dxc.exe") && !std::filesystem::exists(toolsDir / "dxc")) {
             toolsDir = projectRoot / "out/build_vs_18/Build/Editor";
        }

#if defined(_WIN32)
        std::filesystem::path dxcPath = toolsDir / "dxc.exe";
#else
        std::filesystem::path dxcPath = toolsDir / "dxc";
#endif
        return CompileShader(fullSourcePath, fullCompiledPath, stage, target, entry, dxcPath);
    } 
    else if (metadata.importerName == "MaterialImporter") {
        std::error_code ec;
        std::filesystem::copy_file(fullSourcePath, fullCompiledPath, std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }
    else {
        return true;
    }
}

void AssetDatabase::RemoveAsset(const std::filesystem::path& sourcePath) {
    auto it = assets.find(sourcePath);
    if (it != assets.end()) {
        std::error_code ec;
        if (!it->second.compiledPath.empty()) {
            std::filesystem::path fullCompiled = buildRoot / it->second.compiledPath;
            if (std::filesystem::exists(fullCompiled)) std::filesystem::remove(fullCompiled, ec);
        }
        
        std::filesystem::path meta = sourcePath.string() + ".meta";
        if (std::filesystem::exists(meta)) std::filesystem::remove(meta, ec);
        assets.erase(it);
    }
}
