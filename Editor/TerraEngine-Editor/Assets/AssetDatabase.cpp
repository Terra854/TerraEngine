#include "AssetDatabase.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "Engine/Core/Logger/Logger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <regex>

#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "Editor/AssetCompilers/ShaderCompiler.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/SceneManager.h"

using json = nlohmann::json;

namespace {
	constexpr auto kRecentlyRemovedAssetRetention = std::chrono::seconds(5);

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

	std::string ReadScriptMetadataString(const AssetMetadata& metadata, const char* key) {
		const auto settingIt = metadata.settings.find(key);
		if (settingIt == metadata.settings.end() || !settingIt->is_string()) {
			return {};
		}

		return settingIt->get<std::string>();
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

	assets.clear();
	recentlyRemovedAssets.clear();
	TERRAENGINE_LOG(LogLevel::Info, "AssetDatabase initialized. Watching: " << assetRoot);
	
	// Initial Scan
	ScanProject();
	
	// Start Watcher
	fileWatcher.Start(assetRoot);
}

void AssetDatabase::Shutdown() {
	fileWatcher.Stop();
	assets.clear();
	recentlyRemovedAssets.clear();
}

void AssetDatabase::Update() {
	PruneRecentlyRemovedAssets();

	// Process Watcher Events
	auto events = fileWatcher.PopEvents();
	for (const auto& e : events) {
		// Debounce: update or add entry in pending
		// Debounce time: 200ms
		pendingEvents[std::filesystem::absolute(e.path)] = { e.status, 0.2f };
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

	// Process Async Jobs
	for (auto jobIt = asyncJobs.begin(); jobIt != asyncJobs.end(); ) {
		if (jobIt->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			bool compiled = jobIt->future.get();
			if (!compiled) {
				TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << jobIt->absSourcePath);
			} else {
				FinishImport(jobIt->metadata, jobIt->absSourcePath, jobIt->needsImport);
			}
			jobIt = asyncJobs.erase(jobIt);
		} else {
			++jobIt;
		}
	}
}
bool AssetDatabase::IsAssetCompiling(const std::filesystem::path& absSourcePath) const {
	for (const auto& job : asyncJobs) {
		if (job.absSourcePath == absSourcePath) {
			return true;
		}
	}
	return false;
}

void AssetDatabase::OnFileEvent(const std::filesystem::path& path, FileStatus status) {
	// Ignore .meta files and directories
	if (path.extension() == ".meta") return;
	// Check directory only if it exists (for created/modified)
	if ((status == FileStatus::Created || status == FileStatus::Modified) && std::filesystem::is_directory(path)) return;

	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension == ".cs") {
		ScriptCompilationSubsystem::Instance().NotifyScriptSourceChanged(path);
	}

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
	if (std::filesystem::exists(assetRoot)) {
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
			if (entry.is_regular_file()) {
				if (entry.path().extension() == ".meta") continue;
				ImportAsset(std::filesystem::absolute(entry.path()));
			}
		}
	}
}

void AssetDatabase::ImportAsset(const std::filesystem::path& sourcePath, bool force) {
	const auto absSourcePath = std::filesystem::absolute(sourcePath);
	if (absSourcePath.extension() == ".meta") return;
	if (std::filesystem::is_directory(absSourcePath)) return;

	const std::string importerName = GetImporterName(absSourcePath);
	AssetMetadata metadata;
	std::filesystem::path metaFilePath = absSourcePath.string() + ".meta";
	
	bool needsImport = force;
	bool metadataValid = false;

	// 1. Prefer in-memory metadata if we are forcing or it already exists
	if (assets.count(absSourcePath)) {
		metadata = assets[absSourcePath];
		metadataValid = true;
	} 
	// 2. Otherwise try loading from disk
	else if (std::filesystem::exists(metaFilePath)) {
		metadataValid = LoadMetadata(metaFilePath, metadata);
	}

	if (!metadataValid) {
		if (!TryReuseRecentlyRemovedScriptMetadata(absSourcePath, metadata)) {
			GenerateDefaultMetadata(absSourcePath, metadata);
		}
		needsImport = true;
	} else if (!force) {
		// Check timestamp only if not forcing
		auto currentModTime = std::filesystem::last_write_time(absSourcePath);
		if (currentModTime > metadata.lastModified) {
			needsImport = true;
			metadata.lastModified = currentModTime;
		}
	}

	// Always normalize the metadata path back to the file that actually exists
	// on disk so inspector labels and meta persistence follow asset renames.
	metadata.sourcePath = std::filesystem::relative(absSourcePath, assetRoot);
	metadata.importerName = importerName;

	if (metadata.importerName == "ScriptImporter") {
		UpdateScriptMetadata(absSourcePath, metadata);
	}

	// Determine Compiled Path (Relative)
	std::filesystem::path relativeSourcePath = std::filesystem::relative(absSourcePath, assetRoot);
	std::filesystem::path compiledRelativePath = relativeSourcePath;
	
	if (metadata.importerName == "MeshImporter") compiledRelativePath.replace_extension(".temf");
	else if (metadata.importerName == "TextureImporter") compiledRelativePath.replace_extension(".tetf");
	else if (metadata.importerName == "ShaderImporter") compiledRelativePath.replace_extension(".tspf");
	else if (metadata.importerName == "MaterialImporter") compiledRelativePath.replace_extension(".tmat");
	else if (metadata.importerName == "ScriptImporter") compiledRelativePath.clear();
	else if (metadata.importerName == "GenericImporter") compiledRelativePath.clear(); 
	
	// Store relative path in metadata
	metadata.compiledPath = compiledRelativePath;

	if (!metadata.compiledPath.empty()) {
		// Resolve Absolute Paths for checking/compiling
		// Use buildRoot to ensure path includes Assets/
		std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;

		if (needsImport || !std::filesystem::exists(fullCompiledPath)) {
			if (!std::filesystem::exists(fullCompiledPath.parent_path())) {
				std::filesystem::create_directories(fullCompiledPath.parent_path());
			}
			
			if (metadata.importerName == "TextureImporter") {
				auto future = std::async(std::launch::async, [this, metadata]() {
					return CompileAsset(metadata);
				});
				asyncJobs.push_back({metadata, std::move(future), needsImport, absSourcePath});
				return; // Defer registration until async job finishes
			} else {
				bool compiled = CompileAsset(metadata);
				if (!compiled) {
					TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << absSourcePath);
				}
			}
		}
	}
	
	FinishImport(metadata, absSourcePath, needsImport);
}

void AssetDatabase::FinishImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport) {
	// Always save metadata
	SaveMetadata(metadata);

	// Asset GUIDs must stay one-to-one with their live source path. Rename
	// operations can briefly surface the same GUID under both the old and new
	// path, and keeping the stale entry makes GUID lookups return outdated
	// script names after a rename.
	for (auto it = assets.begin(); it != assets.end(); ) {
		if (it->first != absSourcePath && it->second.guid == metadata.guid) {
			it = assets.erase(it);
		} else {
			++it;
		}
	}

	assets[absSourcePath] = metadata;

	// Register with Engine ResourceManager so it can resolve GUIDs to compiled paths
	if (!metadata.compiledPath.empty()) {
		AssetType type;
		bool known = false;
		if (metadata.importerName == "MeshImporter") { type = AssetType::Mesh; known = true; }
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; known = true; }
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; known = true; }
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; known = true; }

		if (known) {
			// Register source path as an alias so the engine can resolve things like "viking_room.png" to the same GUID.
			// We do this first so that idToPath (the preferred load path) is eventually overwritten by the compiled path.
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, absSourcePath, type);

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

void AssetDatabase::DeleteAsset(const std::filesystem::path& sourcePath) {
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	if (it == assets.end()) return;

	AssetMetadata metadata = it->second;
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;

	// Clean up Scene references ONLY during explicit Delete (user intent)
	SceneManager::Instance().NullifyAssetReferences(metadata.guid);

	// 1. Delete source file from disk.
	std::error_code ec;
	if (std::filesystem::exists(fullSourcePath)) {
		std::filesystem::remove(fullSourcePath, ec);
	}

	// 2. Delegate everything else (DB cleanup, engine notification, meta/compiled deletion) to RemoveAsset
	RemoveAsset(absPath);
	
	if (!ec) {
		TERRAENGINE_LOG(LogLevel::Info, "Deleted asset: " << fullSourcePath.filename().string());
	} else {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to delete source file " << fullSourcePath << ": " << ec.message());
	}
}

void AssetDatabase::GenerateDefaultMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
	outMetadata.guid = TerraEngine::GenerateUUID();
	outMetadata.lastModified = std::filesystem::last_write_time(sourcePath);
	outMetadata.importerName = GetImporterName(sourcePath);
	outMetadata.sourcePath = std::filesystem::relative(sourcePath, assetRoot);
	outMetadata.settings = json::object(); 

	if (outMetadata.importerName == "TextureImporter") {
		outMetadata.settings["generateMipmaps"] = true;
		outMetadata.settings["filterType"] = "Lanczos";
		outMetadata.settings["quality"] = 1;
		outMetadata.settings["semantic"] = "color";
	}

	if (outMetadata.importerName == "ScriptImporter") {
		outMetadata.settings["compileStatus"] = "Unknown";
		outMetadata.settings["scriptClassName"] = "";
		outMetadata.settings["scriptNamespace"] = "";
		outMetadata.settings["scriptFullName"] = "";
		UpdateScriptMetadata(sourcePath, outMetadata);
	}
}

std::string AssetDatabase::GetImporterName(const std::filesystem::path& filePath) const {
	std::string ext = filePath.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
	
	if (IsSupportedMeshSourceExtension(filePath)) return "MeshImporter";
	if (IsSupportedTextureSourceExtension(filePath)) return "TextureImporter";
	if (ext == ".hlsl" || ext == ".glsl") return "ShaderImporter";
	if (ext == ".tmat") return "MaterialImporter";
	if (ext == ".cs") return "ScriptImporter";
	if (ext == ".tuiml") return "UIImporter";
	if (ext == ".tuss") return "StyleImporter";
	return "GenericImporter";
}

void AssetDatabase::UpdateScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& metadata) {
	std::ifstream input(sourcePath);
	if (!input) {
		metadata.settings["scriptClassName"] = "";
		metadata.settings["scriptNamespace"] = "";
		metadata.settings["scriptFullName"] = "";
		metadata.settings["isAttachable"] = false;
		return;
	}

	std::stringstream buffer;
	buffer << input.rdbuf();
	const std::string sourceText = buffer.str();

	// Keep script asset metadata cheap to refresh from the file watcher. The
	// regex pass only extracts the namespace, class name, and immediate base
	// type needed for inspector labels and attachability checks.
	static const std::regex namespacePattern(R"(\bnamespace\s+([A-Za-z_][A-Za-z0-9_\.]*))");
	static const std::regex classPattern(R"(\bclass\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([A-Za-z_][A-Za-z0-9_\.]*))?)");

	std::smatch namespaceMatch;
	std::smatch classMatch;
	const std::string scriptNamespace = std::regex_search(sourceText, namespaceMatch, namespacePattern) ? namespaceMatch[1].str() : std::string{};
	const std::string className = std::regex_search(sourceText, classMatch, classPattern) ? classMatch[1].str() : std::string{};
	const std::string baseType = classMatch.size() > 2 ? classMatch[2].str() : std::string{};
	const bool derivesFromScript = baseType == "Script" || baseType == "TerraEngine.Script" || baseType.ends_with(".Script");

	metadata.settings["scriptClassName"] = className;
	metadata.settings["scriptNamespace"] = scriptNamespace;
	metadata.settings["scriptFullName"] = scriptNamespace.empty() ? className : (scriptNamespace + "." + className);
	metadata.settings["isAttachable"] = derivesFromScript;
}

void AssetDatabase::PruneRecentlyRemovedAssets() {
	// Remove rename tombstones once the short reconciliation window expires so
	// the cache cannot grow unbounded during long editor sessions.
	const auto now = std::chrono::steady_clock::now();
	recentlyRemovedAssets.erase(
		std::remove_if(
			recentlyRemovedAssets.begin(),
			recentlyRemovedAssets.end(),
			[&](const RecentlyRemovedAsset& removedAsset) {
				return removedAsset.expiresAt <= now;
			}),
		recentlyRemovedAssets.end());
}

bool AssetDatabase::TryReuseRecentlyRemovedScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
	if (GetImporterName(sourcePath) != "ScriptImporter") {
		return false;
	}

	AssetMetadata currentScriptMetadata;
	currentScriptMetadata.importerName = "ScriptImporter";
	currentScriptMetadata.settings = json::object();
	UpdateScriptMetadata(sourcePath, currentScriptMetadata);

	const std::string currentFullName = ReadScriptMetadataString(currentScriptMetadata, "scriptFullName");
	if (currentFullName.empty()) {
		return false;
	}

	for (auto it = recentlyRemovedAssets.begin(); it != recentlyRemovedAssets.end(); ++it) {
		if (it->metadata.importerName != "ScriptImporter") {
			continue;
		}

		if (ReadScriptMetadataString(it->metadata, "scriptFullName") != currentFullName) {
			continue;
		}

		// Preserve the previous GUID when a filesystem rename arrives as an
		// erase + create pair instead of a metadata-preserving move.
		outMetadata = it->metadata;
		outMetadata.lastModified = std::filesystem::last_write_time(sourcePath);
		outMetadata.sourcePath = std::filesystem::relative(sourcePath, assetRoot);
		outMetadata.importerName = "ScriptImporter";
		recentlyRemovedAssets.erase(it);
		TERRAENGINE_LOG(LogLevel::Info, "Reused existing script metadata for renamed asset " << sourcePath << " so its GUID stays stable.");
		return true;
	}

	return false;
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

		std::uint32_t quality = 1; // Balanced
		if (metadata.settings.contains("quality")) {
			quality = static_cast<std::uint32_t>(metadata.settings["quality"].get<int>());
		}

		bool generateMipmaps = true;
		if (metadata.settings.contains("generateMipmaps")) {
			generateMipmaps = metadata.settings["generateMipmaps"].get<bool>();
		}

		TextureFilterType filter = TextureFilterType::Lanczos;
		if (metadata.settings.contains("filterType")) {
			std::string filterStr = metadata.settings["filterType"];
			if (filterStr == "Box") filter = TextureFilterType::Box;
			else if (filterStr == "Triangle") filter = TextureFilterType::Triangle;
			else if (filterStr == "Lanczos") filter = TextureFilterType::Lanczos;
			else if (filterStr == "Kaiser") filter = TextureFilterType::Kaiser;
		}
		
		return CompileTexture(fullSourcePath, fullCompiledPath, semantic, quality, generateMipmaps, filter);
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
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	if (it != assets.end()) {
		const auto& metadata = it->second;

		if (metadata.importerName == "ScriptImporter") {
			// Keep a short-lived tombstone so a rename can reattach the script
			// to its previous metadata and preserve the original GUID.
			recentlyRemovedAssets.push_back({ metadata, absPath, std::chrono::steady_clock::now() + kRecentlyRemovedAssetRetention });
		}

		AssetType type;
		bool knownResource = false;
		if (metadata.importerName == "MeshImporter") { type = AssetType::Mesh; knownResource = true; }
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; knownResource = true; }
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; knownResource = true; }
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; knownResource = true; }

		if (knownResource) {
			ResourceManager::Instance().DeleteResource(metadata.guid, type);
		}

		std::error_code ec;
		if (!metadata.compiledPath.empty()) {
			std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
			if (std::filesystem::exists(fullCompiled)) std::filesystem::remove(fullCompiled, ec);
		}
		
		std::filesystem::path meta = absPath.string() + ".meta";
		if (std::filesystem::exists(meta)) std::filesystem::remove(meta, ec);
		assets.erase(it);
	}
}

const AssetMetadata* AssetDatabase::FindAssetByGuid(const TerraEngine::UUID& guid) const {
	for (const auto& [path, metadata] : assets) {
		if (metadata.guid == guid) {
			return &metadata;
		}
	}

	return nullptr;
}

const AssetMetadata* AssetDatabase::FindScriptAssetByFullName(std::string_view fullName) const {
	if (fullName.empty()) {
		return nullptr;
	}

	const AssetMetadata* match = nullptr;
	for (const auto& [path, metadata] : assets) {
		if (metadata.importerName != "ScriptImporter") {
			continue;
		}

		if (ReadScriptMetadataString(metadata, "scriptFullName") != fullName) {
			continue;
		}

		if (match != nullptr) {
			return nullptr;
		}

		match = &metadata;
	}

	return match;
}

std::string AssetDatabase::GetScriptFullName(const TerraEngine::UUID& guid) const {
	if (const AssetMetadata* metadata = FindAssetByGuid(guid)) {
		if (metadata->settings.contains("scriptFullName")) {
			return metadata->settings["scriptFullName"].get<std::string>();
		}
	}

	return {};
}

void AssetDatabase::SetScriptCompileState(const std::string& compileState, const std::string& summary) {
	for (auto& [path, metadata] : assets) {
		if (metadata.importerName != "ScriptImporter") {
			continue;
		}

		metadata.settings["compileStatus"] = compileState;
		metadata.settings["lastBuildSummary"] = summary;
		SaveMetadata(metadata);
	}
}
