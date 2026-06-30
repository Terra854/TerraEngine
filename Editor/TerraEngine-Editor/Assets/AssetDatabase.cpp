// AssetDatabase.cpp — Editor asset database implementation
//
// Implements the asset lifecycle management including automated project scanning, metadata persistence, and asynchronous asset compilation. It integrates with the filesystem watcher to detect external changes and syncs the database with the engine's resource manager.

#include "AssetDatabase.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "Engine/Core/Logger/Logger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <regex>

#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "Editor/AssetCompilers/ShaderCompiler.h"
#include "Editor/AssetCompilers/ModelPackageImporter.h"
#include "AssetImportResult.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"

using json = nlohmann::json;

namespace {
	constexpr auto kRecentlyRemovedAssetRetention = std::chrono::seconds(5);

	// UuidToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string UuidToString(const TerraEngine::UUID& uuid) {
		std::ostringstream oss;
		for (const auto& byte : uuid) {
			oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
		}
		return oss.str();
	}

	// StringToUuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	TerraEngine::UUID StringToUuid(const std::string& str) {
		TerraEngine::UUID uuid;
		if (str.length() != uuid.size() * 2) return TerraEngine::EMPTY_UUID;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t i = 0; i < uuid.size(); ++i) {
			uuid[i] = static_cast<std::uint8_t>(std::stoul(str.substr(i * 2, 2), nullptr, 16));
		}
		return uuid;
	}

	// ReadScriptMetadataString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string ReadScriptMetadataString(const AssetMetadata& metadata, const char* key) {
		const auto settingIt = metadata.settings.find(key);
		if (settingIt == metadata.settings.end() || !settingIt->is_string()) {
			return {};
		}

		return settingIt->get<std::string>();
	}

	// Import sidecars are editor bookkeeping written next to source assets. They
	// must remain readable for MCP/package diagnostics, but they are not user
	// assets and should not receive their own importer, GUID, or browser entry.
	bool IsImportSidecarPath(const std::filesystem::path& path) {
		const std::string filename = path.filename().string();
		return filename.size() >= std::string(".import.json").size()
			&& filename.compare(filename.size() - std::string(".import.json").size(), std::string(".import.json").size(), ".import.json") == 0;
	}

	bool IsIgnoredAssetDatabasePath(const std::filesystem::path& path) {
		if (path.extension() == ".meta") {
			std::filesystem::path sourcePath = path;
			sourcePath.replace_extension("");
			return IsImportSidecarPath(sourcePath);
		}
		return IsImportSidecarPath(path);
	}

	bool PublishCompiledAssetAtomically(const std::filesystem::path& stagedPath, const std::filesystem::path& finalPath)
	{
		std::error_code ec;
		std::filesystem::create_directories(finalPath.parent_path(), ec);
		if (ec)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create compiled asset directory " << finalPath.parent_path() << ": " << ec.message());
			return false;
		}

		if (!std::filesystem::exists(stagedPath))
		{
			TERRAENGINE_LOG(LogLevel::Error, "Compiled asset staging file is missing: " << stagedPath);
			return false;
		}

		const std::filesystem::path backupPath = finalPath.string() + ".previous";
		std::filesystem::remove(backupPath, ec);
		ec.clear();

		const bool hadPreviousOutput = std::filesystem::exists(finalPath);
		if (hadPreviousOutput)
		{
			std::filesystem::rename(finalPath, backupPath, ec);
			if (ec)
			{
				TERRAENGINE_LOG(LogLevel::Error, "Failed to preserve previous compiled asset before publish: " << finalPath << " (" << ec.message() << ")");
				return false;
			}
		}

		std::filesystem::rename(stagedPath, finalPath, ec);
		if (ec)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to publish staged compiled asset " << stagedPath << " to " << finalPath << ": " << ec.message());
			if (hadPreviousOutput)
			{
				std::error_code restoreError;
				std::filesystem::rename(backupPath, finalPath, restoreError);
				if (restoreError)
				{
					TERRAENGINE_LOG(LogLevel::Error, "Failed to restore previous compiled asset " << finalPath << ": " << restoreError.message());
				}
			}
			return false;
		}

		if (hadPreviousOutput)
		{
			std::filesystem::remove(backupPath, ec);
			if (ec)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Published compiled asset but could not remove backup " << backupPath << ": " << ec.message());
			}
		}
		return true;
	}

	// RegisterGeneratedModelPackageResources bridges package-owned sub-assets into the
	// runtime resource table. Some generated package outputs, especially
	// textures, are not standalone source assets in the browser, but materials
	// still reference their stable package GUIDs and need ResourceManager paths.
	void RegisterGeneratedModelPackageResources(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& buildRoot)
	{
		const std::filesystem::path sidecarPath = TerraEngine::AssetPipeline::GetImportResultPath(sourcePath);
		if (!std::filesystem::exists(sidecarPath))
		{
			return;
		}

		std::ifstream in(sidecarPath);
		if (!in)
		{
			return;
		}

		json result;
		try
		{
			in >> result;
		}
		catch (const json::exception& ex)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping generated model package resource registration for malformed import sidecar "
				<< sidecarPath << ": " << ex.what());
			return;
		}
		for (const json& mesh : result.value("generatedMeshes", json::array()))
		{
			const auto guid = TerraEngine::ParseUUID(mesh.value("guid", ""));
			const json virtualGeometry = mesh.value("virtualGeometry", json::object());
			const std::string packageStatus = result.value("status", std::string());
			const std::string virtualGeometryStatus = virtualGeometry.value("status", std::string());
			if (!guid
				|| packageStatus == "Failed"
				|| !mesh.contains("compiledPath")
				|| virtualGeometryStatus == "Failed"
				|| virtualGeometryStatus == "NeedsRebuild")
			{
				continue;
			}
			ResourceManager::Instance().RegisterAssetPath(*guid, buildRoot / mesh["compiledPath"].get<std::string>(), AssetType::Mesh);
		}

		for (const json& material : result.value("generatedMaterials", json::array()))
		{
			const auto guid = TerraEngine::ParseUUID(material.value("guid", ""));
			if (!guid || !material.contains("compiledPath"))
			{
				continue;
			}
			ResourceManager::Instance().RegisterAssetPath(*guid, buildRoot / material["compiledPath"].get<std::string>(), AssetType::Material);
		}

		for (const json& texture : result.value("generatedTextures", json::array()))
		{
			const auto guid = TerraEngine::ParseUUID(texture.value("guid", ""));
			if (!guid || !texture.contains("compiledPath"))
			{
				continue;
			}
			ResourceManager::Instance().RegisterAssetPath(*guid, buildRoot / texture["compiledPath"].get<std::string>(), AssetType::Texture);
		}
	}
}

// AssetDatabase::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
AssetDatabase& AssetDatabase::Instance() {
	static AssetDatabase instance;
	return instance;
}

AssetDatabase::AssetDatabase() {}
AssetDatabase::~AssetDatabase() {}

// AssetDatabase::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Initialize(const std::filesystem::path& projectRootPath) {
	this->projectRoot = std::filesystem::absolute(projectRootPath);
	this->assetRoot = this->projectRoot / "Assets";
	this->buildRoot = this->projectRoot / "Build" / "Assets";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// AssetDatabase::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Shutdown() {
	fileWatcher.Stop();
	assets.clear();
	deferredImports.clear();
	recentlyRemovedAssets.clear();
}

// AssetDatabase::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
	std::vector<std::pair<std::filesystem::path, bool>> readyDeferredImports;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto jobIt = asyncJobs.begin(); jobIt != asyncJobs.end(); ) {
		if (jobIt->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			const std::filesystem::path completedPath = jobIt->absSourcePath;
			bool compiled = jobIt->future.get();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!compiled) {
				TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << jobIt->absSourcePath);
			} else {
				FinishImport(jobIt->metadata, jobIt->absSourcePath, jobIt->needsImport);
			}
			jobIt = asyncJobs.erase(jobIt);
			if (const auto deferredIt = deferredImports.find(completedPath); deferredIt != deferredImports.end()) {
				readyDeferredImports.push_back(*deferredIt);
				deferredImports.erase(deferredIt);
			}
		} else {
			++jobIt;
		}
	}
	for (const auto& [path, force] : readyDeferredImports) {
		if (std::filesystem::exists(path)) {
			ImportAsset(path, force);
		}
	}
}
// AssetDatabase::IsAssetCompiling is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::IsAssetCompiling(const std::filesystem::path& absSourcePath) const {
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& job : asyncJobs) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (job.absSourcePath == absSourcePath) {
			return true;
		}
	}
	return false;
}

// AssetDatabase::OnFileEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::OnFileEvent(const std::filesystem::path& path, FileStatus status) {
	// Ignore .meta files and directories
	if (path.extension() == ".meta") return;
	if (IsIgnoredAssetDatabasePath(path)) return;
	// Check directory only if it exists (for created/modified)
	if ((status == FileStatus::Created || status == FileStatus::Modified) && std::filesystem::is_directory(path)) return;

	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".cs") {
		ScriptCompilationSubsystem::Instance().NotifyScriptSourceChanged(path);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (status == FileStatus::Created || status == FileStatus::Modified) {
		if (std::filesystem::exists(path)) {
			ImportAsset(path);
		}
	} else if (status == FileStatus::Erased) {
		RemoveAsset(path);
	}
}

// AssetDatabase::ScanProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::ScanProject() {
	// 1. Cleanup orphaned .meta files
	if (std::filesystem::exists(assetRoot)) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (entry.path().extension() == ".meta") {
				std::filesystem::path sourcePath = entry.path();
				sourcePath.replace_extension(""); // Remove .meta extension to get source
				
				// Import sidecar metadata may have been produced by older scans;
				// remove it so bookkeeping files never stay registered as assets.
				if (IsIgnoredAssetDatabasePath(sourcePath)) {
					std::filesystem::remove(entry.path());
					continue;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!std::filesystem::exists(sourcePath)) {
					// Orphaned meta found
					AssetMetadata tempMeta;
					// Try to load to delete compiled asset
					if (LoadMetadata(entry.path(), tempMeta)) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(assetRoot)) {
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
			if (entry.is_regular_file()) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.path().extension() == ".meta") continue;
				if (IsIgnoredAssetDatabasePath(entry.path())) continue;
				ImportAsset(std::filesystem::absolute(entry.path()));
			}
		}
	}
}

// AssetDatabase::ImportAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::ImportAsset(const std::filesystem::path& sourcePath, bool force) {
	const auto absSourcePath = std::filesystem::absolute(sourcePath);
	if (absSourcePath.extension() == ".meta") return;
	if (IsIgnoredAssetDatabasePath(absSourcePath)) return;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::is_directory(absSourcePath)) return;
	if (IsAssetCompiling(absSourcePath)) {
		// Texture imports are worker jobs. File watcher events and MCP reimport
		// requests can both target the same source while compression is still in
		// flight, so coalesce them into one follow-up pass instead of starting
		// multiple BasisU pools for the same file.
		deferredImports[absSourcePath] = deferredImports[absSourcePath] || force;
		TERRAENGINE_LOG(LogLevel::Info, "Deferred duplicate asset import while compile is active: " << absSourcePath);
		return;
	}

	const std::string importerName = GetImporterName(absSourcePath);
	AssetMetadata metadata;
	std::filesystem::path metaFilePath = absSourcePath.string() + ".meta";
	
	bool needsImport = force;
	bool metadataValid = false;

	// 1. Prefer in-memory metadata if we are forcing or it already exists
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (assets.count(absSourcePath)) {
		metadata = assets[absSourcePath];
		metadataValid = true;
	} 
	// 2. Otherwise try loading from disk
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (std::filesystem::exists(metaFilePath)) {
		metadataValid = LoadMetadata(metaFilePath, metadata);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadataValid) {
		if (!TryReuseRecentlyRemovedScriptMetadata(absSourcePath, metadata)) {
			GenerateDefaultMetadata(absSourcePath, metadata);
		}
		needsImport = true;
	} else if (!force) {
		// Check timestamp only if not forcing
		auto currentModTime = std::filesystem::last_write_time(absSourcePath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (currentModTime > metadata.lastModified) {
			needsImport = true;
			metadata.lastModified = currentModTime;
		}
	}

	// Always normalize the metadata path back to the file that actually exists
	// on disk so inspector labels and meta persistence follow asset renames.
	metadata.sourcePath = std::filesystem::relative(absSourcePath, assetRoot);
	metadata.importerName = importerName;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (metadata.importerName == "ScriptImporter") {
		UpdateScriptMetadata(absSourcePath, metadata);
	}

	// Determine Compiled Path (Relative)
	std::filesystem::path relativeSourcePath = std::filesystem::relative(absSourcePath, assetRoot);
	std::filesystem::path compiledRelativePath = relativeSourcePath;
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) compiledRelativePath.replace_extension(".temf");
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(fullCompiledPath.parent_path())) {
				std::filesystem::create_directories(fullCompiledPath.parent_path());
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (metadata.importerName == "TextureImporter") {
				auto future = std::async(std::launch::async, [this, metadata]() {
					return CompileAsset(metadata);
				});
				asyncJobs.push_back({metadata, std::move(future), needsImport, absSourcePath});
				return; // Defer registration until async job finishes
			} else {
				bool compiled = CompileAsset(metadata);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!compiled) {
					TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << absSourcePath);
				}
			}
		}
	}
	
	FinishImport(metadata, absSourcePath, needsImport);
}

// AssetDatabase::FinishImport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadata.compiledPath.empty()) {
		AssetType type;
		bool known = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) { type = AssetType::Mesh; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; known = true; }

		if (known) {
			// Register source path as an alias so the engine can resolve things like "viking_room.png" to the same GUID.
			// We do this first so that idToPath (the preferred load path) is eventually overwritten by the compiled path.
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, absSourcePath, type);

			std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, fullCompiled, type);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
	{
		RegisterGeneratedModelPackageResources(absSourcePath, buildRoot);
	}
}

// AssetDatabase::DeleteAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::DeleteAsset(const std::filesystem::path& sourcePath) {
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// AssetDatabase::GenerateDefaultMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::GenerateDefaultMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
	outMetadata.guid = TerraEngine::GenerateUUID();
	outMetadata.lastModified = std::filesystem::last_write_time(sourcePath);
	outMetadata.importerName = GetImporterName(sourcePath);
	outMetadata.sourcePath = std::filesystem::relative(sourcePath, assetRoot);
	outMetadata.settings = json::object(); 

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (outMetadata.importerName == "TextureImporter") {
		outMetadata.settings["generateMipmaps"] = true;
		outMetadata.settings["filterType"] = "Lanczos";
		outMetadata.settings["quality"] = 1;
		outMetadata.settings["semantic"] = "color";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (outMetadata.importerName == "ScriptImporter") {
		outMetadata.settings["compileStatus"] = "Unknown";
		outMetadata.settings["scriptClassName"] = "";
		outMetadata.settings["scriptNamespace"] = "";
		outMetadata.settings["scriptFullName"] = "";
		UpdateScriptMetadata(sourcePath, outMetadata);
	}
}

// AssetDatabase::GetImporterName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string AssetDatabase::GetImporterName(const std::filesystem::path& filePath) const {
	std::string ext = filePath.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (TerraEngine::AssetPipeline::IsModelPackageSourceExtension(filePath)) return TerraEngine::AssetPipeline::GetModelPackageImporterName(filePath);
	if (IsSupportedMeshSourceExtension(filePath)) return "MeshImporter";
	if (IsSupportedTextureSourceExtension(filePath)) return "TextureImporter";
	if (ext == ".hlsl" || ext == ".glsl") return "ShaderImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tmat") return "MaterialImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".cs") return "ScriptImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tuiml") return "UIImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tuss") return "StyleImporter";
	return "GenericImporter";
}

// AssetDatabase::UpdateScriptMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// AssetDatabase::PruneRecentlyRemovedAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// AssetDatabase::TryReuseRecentlyRemovedScriptMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::TryReuseRecentlyRemovedScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
	if (GetImporterName(sourcePath) != "ScriptImporter") {
		return false;
	}

	AssetMetadata currentScriptMetadata;
	currentScriptMetadata.importerName = "ScriptImporter";
	currentScriptMetadata.settings = json::object();
	UpdateScriptMetadata(sourcePath, currentScriptMetadata);

	const std::string currentFullName = ReadScriptMetadataString(currentScriptMetadata, "scriptFullName");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFullName.empty()) {
		return false;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto it = recentlyRemovedAssets.begin(); it != recentlyRemovedAssets.end(); ++it) {
		if (it->metadata.importerName != "ScriptImporter") {
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// AssetDatabase::LoadMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("settings")) outMetadata.settings = j["settings"];
		return true;
	} catch (...) { return false; }
}

// AssetDatabase::SaveMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::SaveMetadata(const AssetMetadata& metadata) {
	std::filesystem::path metaFilePath = (assetRoot / metadata.sourcePath).string() + ".meta";
	std::ofstream file(metaFilePath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// AssetDatabase::CompileAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::CompileAsset(const AssetMetadata& metadata) {
	if (metadata.compiledPath.empty()) return true;

	// Resolve full paths using buildRoot
	std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
	
	fullSourcePath.make_preferred();
	fullCompiledPath.make_preferred();

	TERRAENGINE_LOG(LogLevel::Info, "Compiling Asset via Internal Compiler: " << fullSourcePath);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) {
		TerraEngine::AssetPipeline::ModelPackageImportOptions options{};
		options.sourcePath = fullSourcePath;
		options.assetRoot = assetRoot;
		options.buildRoot = buildRoot;
		options.sourceRelativePath = metadata.sourcePath;
		options.packageGuid = metadata.guid;
		options.shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
		options.forceReimport = true;

		TerraEngine::AssetPipeline::ModelPackageImportResult importResult = TerraEngine::AssetPipeline::ImportModelPackage(options);
		for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials)
		{
			const std::filesystem::path materialSourcePath = assetRoot / material.sourcePath;
			if (!std::filesystem::exists(materialSourcePath))
			{
				continue;
			}

			AssetMetadata materialMetadata;
			materialMetadata.guid = material.guid;
			materialMetadata.importerName = "MaterialImporter";
			materialMetadata.sourcePath = material.sourcePath;
			materialMetadata.compiledPath = material.compiledPath;
			materialMetadata.lastModified = std::filesystem::last_write_time(materialSourcePath);
			materialMetadata.settings = json::object();
			// Package import is already publishing a coherent package sidecar and
			// generated material files. Register the generated material metadata
			// without forcing a renderer hot-reload for every child material; doing
			// that during startup/package import can invalidate renderer resources
			// before editor render passes are ready.
			FinishImport(materialMetadata, std::filesystem::absolute(materialSourcePath), false);
		}
		if (!TerraEngine::AssetPipeline::SaveModelPackageImportResult(fullSourcePath, importResult)) {
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to save model package import result sidecar for " << fullSourcePath);
		}
		if (importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed) {
			return false;
		}

		return std::filesystem::exists(fullCompiledPath);
	}
	else if (metadata.importerName == "MeshImporter") {
		const std::filesystem::path stagedCompiledPath = fullCompiledPath.string() + "." + UuidToString(metadata.guid) + ".tmp";
		std::error_code cleanupError;
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		// Compile into a sibling file first. The live compiled asset is replaced
		// only after the compiler reports success, so failed rebuilds never
		// unregister or corrupt the previous usable mesh.
		if (!CompileMesh(fullSourcePath, stagedCompiledPath))
		{
			std::filesystem::remove(stagedCompiledPath, cleanupError);
			return false;
		}
		if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath))
		{
			std::filesystem::remove(stagedCompiledPath, cleanupError);
			return false;
		}
		return true;
	} 
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (metadata.importerName == "TextureImporter") {
		std::string semanticStr = "color";
		if (metadata.settings.contains("semantic")) semanticStr = metadata.settings["semantic"];
		
		TextureFileSemantic semantic = TextureFileSemantic::Color;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (semanticStr == "data") semantic = TextureFileSemantic::Data;

		std::uint32_t quality = 1; // Balanced
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.settings.contains("quality")) {
			quality = static_cast<std::uint32_t>(metadata.settings["quality"].get<int>());
		}

		bool generateMipmaps = true;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.settings.contains("generateMipmaps")) {
			generateMipmaps = metadata.settings["generateMipmaps"].get<bool>();
		}

		TextureFilterType filter = TextureFilterType::Lanczos;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.settings.contains("filterType")) {
			std::string filterStr = metadata.settings["filterType"];
			if (filterStr == "Box") filter = TextureFilterType::Box;
			else if (filterStr == "Triangle") filter = TextureFilterType::Triangle;
			else if (filterStr == "Lanczos") filter = TextureFilterType::Lanczos;
			else if (filterStr == "Kaiser") filter = TextureFilterType::Kaiser;
		}
		
		return CompileTexture(fullSourcePath, fullCompiledPath, semantic, quality, generateMipmaps, filter);
	} 
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (metadata.importerName == "ShaderImporter") {
		std::string stage = "vs";
		if (metadata.settings.contains("stage")) stage = metadata.settings["stage"];
		else { 
			std::string fn = fullSourcePath.filename().string();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (fn.find("frag") != std::string::npos || fn.find("pixel") != std::string::npos) stage = "ps";
		}
		std::string target = "";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.settings.contains("target")) target = metadata.settings["target"];
		std::string entry = "main";
		if (metadata.settings.contains("entryPoint")) entry = metadata.settings["entryPoint"];

		// Find DXC
		std::filesystem::path toolsDir = projectRoot / "out/build/Build/Debug"; 
		
		// Try relative to project root
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::filesystem::exists(toolsDir / "dxc.exe") && !std::filesystem::exists(toolsDir / "dxc")) {
			toolsDir = std::filesystem::current_path();
		}
		// Try specific VS18 path
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// AssetDatabase::RemoveAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::RemoveAsset(const std::filesystem::path& sourcePath) {
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it != assets.end()) {
		const auto& metadata = it->second;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "ScriptImporter") {
			// Keep a short-lived tombstone so a rename can reattach the script
			// to its previous metadata and preserve the original GUID.
			recentlyRemovedAssets.push_back({ metadata, absPath, std::chrono::steady_clock::now() + kRecentlyRemovedAssetRetention });
		}

		AssetType type;
		bool knownResource = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) { type = AssetType::Mesh; knownResource = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; knownResource = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; knownResource = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; knownResource = true; }

		if (knownResource) {
			ResourceManager::Instance().DeleteResource(metadata.guid, type);
		}

		std::error_code ec;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!metadata.compiledPath.empty()) {
			std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
			if (std::filesystem::exists(fullCompiled)) std::filesystem::remove(fullCompiled, ec);
		}
		
		std::filesystem::path meta = absPath.string() + ".meta";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(meta)) std::filesystem::remove(meta, ec);
		assets.erase(it);
	}
}

// AssetDatabase::FindAssetByGuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const AssetMetadata* AssetDatabase::FindAssetByGuid(const TerraEngine::UUID& guid) const {
	for (const auto& [path, metadata] : assets) {
		if (metadata.guid == guid) {
			return &metadata;
		}
	}

	return nullptr;
}

bool AssetDatabase::TryGetVirtualGeometryMetadata(const AssetMetadata& metadata, VirtualGeometryAssetMetadata& outMetadata) const {
	outMetadata = {};
	if (metadata.compiledPath.empty()) {
		outMetadata.warnings.push_back("Compiled mesh path is empty.");
		return false;
	}

	const std::filesystem::path compiledPath = buildRoot / metadata.compiledPath;
	std::ifstream input(compiledPath, std::ios::binary);
	if (!input) {
		outMetadata.warnings.push_back("Compiled mesh file is missing.");
		return false;
	}

	MeshHeader header{};
	input.read(reinterpret_cast<char*>(&header), sizeof(MeshHeader));
	if (!input || std::memcmp(header.magic, "TEMF", 4) != 0) {
		outMetadata.warnings.push_back("Compiled mesh header is not a valid TEMF file.");
		return false;
	}

	outMetadata.readable = true;
	outMetadata.meshletCount = header.meshletCount;
	outMetadata.payloadGeneration = header.virtualGeometryPayloadGeneration;
	outMetadata.hasVirtualGeometry = (header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u;
	if (!outMetadata.hasVirtualGeometry) {
		outMetadata.warnings.push_back("Virtual geometry payload is not present; rebuild the mesh to generate it.");
		outMetadata.rebuildGuidance = "Rebuild this mesh from source to generate virtual geometry metadata for the current TEMF v1 contract.";
		return true;
	}

	if (header.virtualGeometryHeaderOffset == 0u
		|| header.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)
		|| header.virtualGeometryPayloadGeneration != kVirtualGeometryPayloadGeneration) {
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Virtual geometry payload metadata is stale or incomplete; rebuild this mesh.");
		outMetadata.rebuildGuidance = "Rebuild this mesh so the compiled artifact is regenerated with the current virtual geometry payload generation.";
		return true;
	}

	VirtualGeometryHeader virtualHeader{};
	input.seekg(static_cast<std::streamoff>(header.virtualGeometryHeaderOffset), std::ios::beg);
	input.read(reinterpret_cast<char*>(&virtualHeader), sizeof(VirtualGeometryHeader));
	if (!input || virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration) {
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Virtual geometry root header could not be read; rebuild this mesh.");
		outMetadata.rebuildGuidance = "Rebuild this mesh because the virtual geometry root header is unreadable or incompatible.";
		return true;
	}

	outMetadata.detailLevelCount = virtualHeader.detailLevelCount;
	outMetadata.sourceTriangleCount = virtualHeader.sourceTriangleCount;
	if (virtualHeader.detailLevelCount == 0u || virtualHeader.cullableGroupCount == 0u) {
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Virtual geometry contains no renderable detail levels or cullable groups.");
		outMetadata.rebuildGuidance = "Rebuild this mesh because the compiled virtual geometry tables contain no renderable detail.";
	}
	return true;
}

// AssetDatabase::FindScriptAssetByFullName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const AssetMetadata* AssetDatabase::FindScriptAssetByFullName(std::string_view fullName) const {
	if (fullName.empty()) {
		return nullptr;
	}

	const AssetMetadata* match = nullptr;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [path, metadata] : assets) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName != "ScriptImporter") {
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ReadScriptMetadataString(metadata, "scriptFullName") != fullName) {
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (match != nullptr) {
			return nullptr;
		}

		match = &metadata;
	}

	return match;
}

// AssetDatabase::GetScriptFullName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string AssetDatabase::GetScriptFullName(const TerraEngine::UUID& guid) const {
	if (const AssetMetadata* metadata = FindAssetByGuid(guid)) {
		if (metadata->settings.contains("scriptFullName")) {
			return metadata->settings["scriptFullName"].get<std::string>();
		}
	}

	return {};
}

// AssetDatabase::SetScriptCompileState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
