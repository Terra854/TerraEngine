#include "Resource/ResourceManager.h"
#include "Renderer/RenderSystem.h"

#include <fstream>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

#include "Core/Logger/Logger.h"
#include "Runtime/ContentMount/ContentMount.h"
// Concrete resource definitions are required here so the cache-owned
// std::unique_ptr instances have complete types when the deleters compile.
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "Resource/ShaderCollection.h"
#include "basisu_transcoder.h"
#include "basisu_transcoder_uastc.h"

namespace
{
	constexpr std::string_view kDefaultManifestProductName = "TerraEngine";
	constexpr std::string_view kDefaultManifestVersion = "1.0.0";

	std::vector<std::string_view> SupportedExtensionsForAssetType(AssetType type)
	{
		switch (type)
		{
		case AssetType::Mesh:
			return { ".temf" };
		case AssetType::Texture:
			return { ".tetf" };
		case AssetType::Material:
			return { ".tmat" };
		case AssetType::ShaderCollection:
			return { ".tspf" };
		}

		return {};
	}

	bool AssetReferenceLooksLikePath(std::string_view assetReference)
	{
		const std::filesystem::path queryPath(assetReference);
		return queryPath.is_absolute()
			|| queryPath.has_extension()
			|| assetReference.find('/') != std::string_view::npos
			|| assetReference.find('\\') != std::string_view::npos;
	}

	bool HasNativeExtensionForAssetType(const std::filesystem::path& path, AssetType type)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		for (std::string_view nativeExtension : SupportedExtensionsForAssetType(type))
		{
			if (extension == nativeExtension)
			{
				return true;
			}
		}

		return false;
	}

	std::optional<std::filesystem::path> FindEditorProjectRoot(const std::filesystem::path& assetRoot)
	{
		if (assetRoot.empty())
		{
			return std::nullopt;
		}

		std::filesystem::path cursor = assetRoot.lexically_normal();
		for (int depth = 0; depth < 6; ++depth)
		{
			if (std::filesystem::exists(cursor / "ProjectSettings" / "ProjectSettings.terra")
				|| std::filesystem::exists(cursor / "game.json"))
			{
				return cursor;
			}

			const std::filesystem::path parent = cursor.parent_path();
			if (parent.empty() || parent == cursor)
			{
				break;
			}

			cursor = parent;
		}

		return std::nullopt;
	}
}

ResourceManager& ResourceManager::Instance()
{
	static ResourceManager instance;
	return instance;
}

// Out-of-line destructor ensures the concrete resource types are complete when
// the cache members are destroyed, keeping unique_ptr deleters well-defined.
ResourceManager::~ResourceManager() = default;

ResourceManager::ResourceManager()
	: threadPool(ThreadPool::instance())
{
	assetRoot = std::filesystem::current_path();
	engineRoot = std::filesystem::current_path();

	basist::basisu_transcoder_init();
	basist::uastc_init();
}

void ResourceManager::Initialize()
{
	// Each engine session starts with a fresh startup-scene override so stale
	// export manifest data cannot leak into the next launch.
	m_ConfiguredStartupScene.reset();
	ResetManifestMetadata();

	if (!m_ConfiguredExportArchivePath.has_value()) {
		return;
	}

	try {
		const std::filesystem::path archivePath = *m_ConfiguredExportArchivePath;
		if (!std::filesystem::exists(archivePath)) {
			throw std::runtime_error("Configured TEPAK archive does not exist: " + archivePath.string());
		}

		// Mount the export archive now, inside engine startup, so scene and
		// resource loading can depend on the mounted content root without the
		// launcher performing content I/O before Engine::Run().
		if (!ContentMount::Instance().Mount(archivePath)) {
			throw std::runtime_error("Failed to mount TEPAK archive: " + archivePath.string());
		}

		if (!ContentMount::Instance().Exists("game.json")) {
			throw std::runtime_error("Mounted TEPAK archive is missing game.json: " + archivePath.string());
		}

		const std::vector<uint8_t> manifestBytes = ContentMount::Instance().ReadAll("game.json");
		if (manifestBytes.empty()) {
			throw std::runtime_error("game.json inside TEPAK archive is empty: " + archivePath.string());
		}

		const std::string manifestText(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
		const nlohmann::json manifest = nlohmann::json::parse(manifestText);
		ApplyManifestMetadata(manifest, archivePath.stem().string());

		if (manifest.contains("startupScene")) {
			m_ConfiguredStartupScene = manifest["startupScene"].get<std::string>();
		}

		// Register the exported asset lookup table before SceneManager loads the
		// startup scene so UUID references inside scenes and prefabs can resolve
		// back to logical archive paths immediately.
		if (manifest.contains("assets")) {
			for (const auto& assetEntry : manifest["assets"]) {
				try {
					const std::string guidStr = assetEntry.value("guid", "");
					const std::string path = assetEntry.value("path", "");
					const uint32_t typeInt = assetEntry.value("type", 0u);

					if (auto guid = TerraEngine::ParseUUID(guidStr)) {
						RegisterAssetPath(*guid, path, static_cast<AssetType>(typeInt));
					}
				}
				catch (...) {
					// Skip malformed entries so one bad manifest record does not
					// prevent every other exported asset from being registered.
				}
			}
		}
	}
	catch (...) {
		// Leave the manager in a clean state if export startup fails halfway
		// through so the engine's higher-level exception handling can retry or
		// report the failure without a half-mounted archive lingering around.
		Shutdown();
		throw;
	}
}

void ResourceManager::Shutdown()
{
	m_ConfiguredStartupScene.reset();
	ResetManifestMetadata();
	ClearCaches();

	if (ContentMount::Instance().IsMounted()) {
		ContentMount::Instance().Unmount();
	}
}

void ResourceManager::SetEngineRoot(const std::filesystem::path& engineRootPath)
{
	engineRoot = engineRootPath.lexically_normal();
	TERRAENGINE_LOG(LogLevel::Debug, "Engine root set to " << engineRoot);
}

void ResourceManager::SetAssetRoot(const std::filesystem::path& assetRootPath)
{
	// Normalize immediately so the cache keys stay predictable even if
	// callers provide paths with redundant separators or relative jumps.
	assetRoot = assetRootPath.lexically_normal();
	if (!IsExportRuntime())
	{
		RefreshEditorProjectManifest();
	}
	// Verbose debug logging makes it obvious which asset tree subsequent loads will pull from.
	TERRAENGINE_LOG(LogLevel::Debug, "Asset root set to " << assetRoot);
}

std::filesystem::path ResourceManager::GetAssetRoot() const
{
	return assetRoot;
}

void ResourceManager::ConfigureExportArchive(const std::filesystem::path& archivePath)
{
	if (archivePath.empty()) {
		m_ConfiguredExportArchivePath.reset();
		return;
	}

	// Anchor both engine and asset roots to the export folder so path-derived
	// cache keys and default shader lookups stay correct even if the process is
	// launched from a different working directory.
	m_ConfiguredExportArchivePath = std::filesystem::absolute(archivePath);
	const std::filesystem::path runtimeRoot = m_ConfiguredExportArchivePath->parent_path();
	SetEngineRoot(runtimeRoot);
	SetAssetRoot(runtimeRoot);
}

std::optional<std::filesystem::path> ResourceManager::GetConfiguredStartupScene() const
{
	return m_ConfiguredStartupScene;
}

std::string ResourceManager::GetProductName() const
{
	return m_ProductName;
}

std::string ResourceManager::GetVersion() const
{
	return m_Version;
}

bool ResourceManager::IsExportRuntime() const
{
	// Export sessions always configure a TEPAK archive before engine startup.
	// Editor/game-project sessions leave this unset and continue using the
	// authored on-disk project layout instead.
	return m_ConfiguredExportArchivePath.has_value();
}

void ResourceManager::ResetManifestMetadata(std::string_view fallbackProductName)
{
	m_ProductName = fallbackProductName.empty()
		? std::string(kDefaultManifestProductName)
		: std::string(fallbackProductName);
	m_Version = std::string(kDefaultManifestVersion);
}

void ResourceManager::ApplyManifestMetadata(const nlohmann::json& manifest, std::string_view fallbackProductName)
{
	ResetManifestMetadata(fallbackProductName);

	if (auto nameIt = manifest.find("name"); nameIt != manifest.end() && nameIt->is_string())
	{
		const std::string manifestName = nameIt->get<std::string>();
		if (!manifestName.empty())
		{
			m_ProductName = manifestName;
		}
	}

	if (auto versionIt = manifest.find("version"); versionIt != manifest.end() && versionIt->is_string())
	{
		const std::string manifestVersion = versionIt->get<std::string>();
		if (!manifestVersion.empty())
		{
			m_Version = manifestVersion;
		}
	}
}

void ResourceManager::RefreshEditorProjectManifest()
{
	const std::optional<std::filesystem::path> projectRoot = FindEditorProjectRoot(assetRoot);
	if (!projectRoot.has_value())
	{
		ResetManifestMetadata();
		return;
	}

	const std::string fallbackProductName = projectRoot->filename().string();
	ResetManifestMetadata(fallbackProductName);

	const std::filesystem::path manifestPath = *projectRoot / "game.json";
	if (!std::filesystem::exists(manifestPath))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Project manifest is missing at " << manifestPath << "; using fallback product metadata.");
		return;
	}

	try
	{
		std::ifstream manifestStream(manifestPath);
		if (!manifestStream)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Project manifest could not be opened at " << manifestPath << "; using fallback product metadata.");
			return;
		}

		const nlohmann::json manifest = nlohmann::json::parse(manifestStream);
		ApplyManifestMetadata(manifest, fallbackProductName);
	}
	catch (const std::exception& exception)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Project manifest could not be parsed at " << manifestPath << ": " << exception.what());
	}
}

void ResourceManager::LoadDefaultResources()
{
	TerraEngine::UUID defaultMeshId{};
	defaultMeshId.fill(0);
	defaultMeshId[15] = 1;

	TerraEngine::UUID defaultFragId{};
	defaultFragId.fill(0);
	defaultFragId[15] = 2;

	TerraEngine::UUID defaultTaskId{};
	defaultTaskId.fill(0);
	defaultTaskId[15] = 3;

	TerraEngine::UUID defaultMeshOnlyCollectionId{};
	defaultMeshOnlyCollectionId.fill(0);
	defaultMeshOnlyCollectionId[15] = 1;

	TerraEngine::UUID defaultTaskCollectionId{};
	defaultTaskCollectionId.fill(0);
	defaultTaskCollectionId[15] = 2;

	auto ResolveEnginePath = [this](const std::filesystem::path& p) {
		if (p.is_absolute()) return p.lexically_normal();
		return (engineRoot / p).lexically_normal();
	};

	// Pre-register Mesh Shader
	const std::string meshPath = "Shaders/Default.mesh.tspf";
	std::string meshKey = NormalizeKey(ResolveEnginePath(meshPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[meshKey] = defaultMeshId;
	}
	
	// LoadShader normally uses assetRoot, so we must pass the absolute path
	LoadShader(ResolveEnginePath(meshPath));

	// Pre-register Task Shader
	const std::string taskPath = "Shaders/Default.task.tspf";
	std::string taskKey = NormalizeKey(ResolveEnginePath(taskPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[taskKey] = defaultTaskId;
	}
	LoadShader(ResolveEnginePath(taskPath));
	const bool taskShaderReady = (GetShader(defaultTaskId) != nullptr);

	// Pre-register Fragment Shader
	const std::string fragPath = "Shaders/Default.frag.tspf";
	std::string fragKey = NormalizeKey(ResolveEnginePath(fragPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[fragKey] = defaultFragId;
	}
	LoadShader(ResolveEnginePath(fragPath));
	// Pre-register mesh-only fallback collection UUID.
	std::ostringstream fallbackCollectionKeyStream;
	fallbackCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Mesh) << ":" << meshKey << ";";
	fallbackCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Fragment) << ":" << fragKey << ";";
	const std::string fallbackCollectionKey = fallbackCollectionKeyStream.str();

	// Pre-register task+mesh+fragment collection UUID.
	std::ostringstream taskCollectionKeyStream;
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Task) << ":" << taskKey << ";";
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Mesh) << ":" << meshKey << ";";
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Fragment) << ":" << fragKey << ";";
	const std::string taskCollectionKey = taskCollectionKeyStream.str();
	{
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId[fallbackCollectionKey] = defaultMeshOnlyCollectionId;
		shaderCollectionCache.pathToId[taskCollectionKey] = defaultTaskCollectionId;
	}

	std::vector<ShaderStagePath> fallbackStages = {
		{ ShaderStage::Mesh, ResolveEnginePath(meshPath) },
		{ ShaderStage::Fragment, ResolveEnginePath(fragPath) }
	};
	LoadShaderCollection(fallbackStages);

	if (taskShaderReady)
	{
		std::vector<ShaderStagePath> taskStages = {
			{ ShaderStage::Task, ResolveEnginePath(taskPath) },
			{ ShaderStage::Mesh, ResolveEnginePath(meshPath) },
			{ ShaderStage::Fragment, ResolveEnginePath(fragPath) }
		};
		LoadShaderCollection(taskStages);
	}
	else
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Default task shader was not available; renderer will use mesh-only fallback collection.");
	}
}

TerraEngine::UUID ResourceManager::GetDefaultShaderCollectionID() const
{
	TerraEngine::UUID id{};
	id.fill(0);
	id[15] = 1;
	return id;
}

TerraEngine::UUID ResourceManager::GetDefaultTaskShaderCollectionID() const
{
	TerraEngine::UUID id{};
	id.fill(0);
	id[15] = 2;
	return id;
}

void ResourceManager::ClearCaches()
{
	// Acquire exclusive locks in a consistent order to avoid deadlocks if
	// multiple threads ever clear different caches concurrently.
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.pathToId.clear();
		materialCache.idToPath.clear();
		materialCache.resources.clear();
		materialCache.inFlight.clear();
	}
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.pathToId.clear();
		meshCache.idToPath.clear();
		meshCache.resources.clear();
		meshCache.inFlight.clear();
	}
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId.clear();
		shaderCache.idToPath.clear();
		shaderCache.resources.clear();
		shaderCache.inFlight.clear();
	}
	{
		// Clear paired shader lookups so future installs cannot reuse stale stage combinations.
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId.clear();
		shaderCollectionCache.idToPath.clear();
		shaderCollectionCache.resources.clear();
		shaderCollectionCache.inFlight.clear();
	}
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.pathToId.clear();
		textureCache.idToPath.clear();
		textureCache.resources.clear();
		textureCache.inFlight.clear();
	}
}

void ResourceManager::DeleteResource(const TerraEngine::UUID& id, AssetType type)
{
	auto removeFromCache = [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		for (auto it = cache.pathToId.begin(); it != cache.pathToId.end(); ) {
			if (it->second == id) {
				it = cache.pathToId.erase(it);
			}
			else {
				++it;
			}
		}
		cache.idToPath.erase(id);
		cache.resources.erase(id);
	};

	switch (type) {
	case AssetType::Mesh: removeFromCache(meshCache); break;
	case AssetType::Texture: removeFromCache(textureCache); break;
	case AssetType::Material: removeFromCache(materialCache); break;
	case AssetType::ShaderCollection: removeFromCache(shaderCollectionCache); break;
	}

	// Notify renderer to purge any GPU-side references/buffers
	RenderSystem::Instance().DeleteResource(id);
}

bool ResourceManager::HasResource(const TerraEngine::UUID& id) const
{
	auto isAvailable = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		return cache.resources.find(id) != cache.resources.end() || cache.idToPath.find(id) != cache.idToPath.end();
	};

	return isAvailable(meshCache) || isAvailable(textureCache) || isAvailable(materialCache) ||
		isAvailable(shaderCollectionCache) || isAvailable(shaderCache);
}

void ResourceManager::RegisterAssetPath(const TerraEngine::UUID& id, const std::filesystem::path& path, AssetType type)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	const bool canBePreferredLoadPath = HasNativeExtensionForAssetType(resolvedPath, type);

	VisitAssetCache(type, [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		cache.pathToId[key] = id;
		// Source assets such as .obj are useful aliases for resolving authored
		// references, but render-time resource loads must use compiled artifacts
		// such as .temf. Keeping idToPath limited to native formats prevents the
		// renderer from parsing a source file while its compiler job is still
		// producing the real runtime payload.
		if (canBePreferredLoadPath)
		{
			cache.idToPath[id] = key;
		}
	});
}

TerraEngine::UUID ResourceManager::ResolveAssetReference(std::string_view assetReference, AssetType type)
{
	if (assetReference.empty())
	{
		return TerraEngine::EMPTY_UUID;
	}

	const auto buildCandidatePaths = [&]() -> std::vector<std::filesystem::path> {
		std::vector<std::filesystem::path> candidatePaths;
		const std::filesystem::path queryPath(assetReference);
		if (AssetReferenceLooksLikePath(assetReference))
		{
			// Explicit paths are resolved literally relative to the asset root,
			// then retried with native runtime extensions. This lets authored
			// scene data refer to source assets like Models/cube.obj while the
			// renderer actually loads Models/cube.temf.
			candidatePaths.push_back(queryPath);
			if (queryPath.has_extension())
			{
				for (std::string_view extension : SupportedExtensionsForAssetType(type))
				{
					if (queryPath.extension() != extension)
					{
						std::filesystem::path candidate = queryPath;
						candidate.replace_extension(extension);
						candidatePaths.push_back(std::move(candidate));
					}
				}
			}
			return candidatePaths;
		}

		const std::filesystem::path topLevelName = queryPath.filename();
		if (queryPath.has_extension())
		{
			// A bare filename like "Player.temf" is assumed to live directly
			// under the asset root, not somewhere deeper in the tree.
			candidatePaths.push_back(topLevelName);

			// Also try substituting with native extensions for this asset type
			// if the provided extension is different. This handles cases where
			// a document refers to a source asset (e.g. "viking_room.png") but
			// the engine needs the compiled version ("viking_room.tetf").
			for (std::string_view extension : SupportedExtensionsForAssetType(type))
			{
				if (topLevelName.extension() != extension)
				{
					std::filesystem::path candidate = topLevelName;
					candidate.replace_extension(extension);
					candidatePaths.push_back(std::move(candidate));
				}
			}

			return candidatePaths;
		}

		for (std::string_view extension : SupportedExtensionsForAssetType(type))
		{
			std::filesystem::path candidate = topLevelName;
			candidate += extension;
			candidatePaths.push_back(std::move(candidate));
		}

		return candidatePaths;
	};

	const std::vector<std::filesystem::path> candidatePaths = buildCandidatePaths();

	const auto tryFindRegisteredMatch = [&](auto& cache) -> TerraEngine::UUID {
		std::shared_lock lock(cache.mutex);
		for (const std::filesystem::path& candidatePath : candidatePaths)
		{
			const std::string exactKey = NormalizeKey(ResolveAssetPath(candidatePath));
			if (auto exactMatch = cache.pathToId.find(exactKey); exactMatch != cache.pathToId.end())
			{
				return exactMatch->second;
			}
		}

		return TerraEngine::EMPTY_UUID;
	};

	const auto tryLoadExplicitPath = [&](const std::filesystem::path& candidatePath) -> TerraEngine::UUID {
		if (!HasNativeExtensionForAssetType(candidatePath, type))
		{
			return TerraEngine::EMPTY_UUID;
		}

		switch (type)
		{
		case AssetType::Mesh:
			return LoadMesh(candidatePath);
		case AssetType::Texture:
			return LoadTexture(candidatePath);
		case AssetType::Material:
			return LoadMaterial(candidatePath);
		case AssetType::ShaderCollection:
			return TerraEngine::EMPTY_UUID;
		}

		return TerraEngine::EMPTY_UUID;
	};

	TerraEngine::UUID resolvedId = TerraEngine::EMPTY_UUID;
	VisitAssetCache(type, [&](auto& cache) {
		resolvedId = tryFindRegisteredMatch(cache);
	});
	if (resolvedId != TerraEngine::EMPTY_UUID)
	{
		return resolvedId;
	}

	for (const std::filesystem::path& candidatePath : candidatePaths)
	{
		resolvedId = tryLoadExplicitPath(candidatePath);
		if (resolvedId != TerraEngine::EMPTY_UUID)
		{
			return resolvedId;
		}
	}

	return TerraEngine::EMPTY_UUID;
}

std::filesystem::path ResourceManager::DescribeAssetReference(const TerraEngine::UUID& id, AssetType type) const
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return {};
	}

	std::filesystem::path result;
	const auto capturePath = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		if (auto pathIt = cache.idToPath.find(id); pathIt != cache.idToPath.end())
		{
			result = std::filesystem::path(pathIt->second);
		}
	};

	switch (type)
	{
	case AssetType::Mesh:
		capturePath(meshCache);
		break;
	case AssetType::Texture:
		capturePath(textureCache);
		break;
	case AssetType::Material:
		capturePath(materialCache);
		break;
	case AssetType::ShaderCollection:
		capturePath(shaderCollectionCache);
		break;
	}

	if (result.is_absolute())
	{
		const std::filesystem::path relativePath = result.lexically_relative(assetRoot);
		if (!relativePath.empty())
		{
			return relativePath;
		}
	}

	return result;
}

Material* ResourceManager::GetMaterial(const TerraEngine::UUID& id)
{
	return GetResourceById<Material>(id, materialCache, [this](const std::filesystem::path& p) { return LoadMaterial(p); });
}

Mesh* ResourceManager::GetMesh(const TerraEngine::UUID& id)
{
	return GetResourceById<Mesh>(id, meshCache, [this](const std::filesystem::path& p) { return LoadMesh(p); });
}

Shader* ResourceManager::GetShader(const TerraEngine::UUID& id)
{
	return GetResourceById<Shader>(id, shaderCache, [this](const std::filesystem::path& p) { return LoadShader(p); });
}

ShaderCollection* ResourceManager::GetShaderCollection(const TerraEngine::UUID& id)
{
	return GetResourceById<ShaderCollection>(id, shaderCollectionCache);
}

Texture* ResourceManager::GetTexture(const TerraEngine::UUID& id)
{
	return GetResourceById<Texture>(id, textureCache, [this](const std::filesystem::path& p) { return LoadTexture(p); });
}

std::filesystem::path ResourceManager::ResolveAssetPath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (assetRoot / path).lexically_normal();
}

std::string ResourceManager::NormalizeKey(const std::filesystem::path& path) const
{
	// Use generic_string to avoid mismatches between Windows backslashes and
	// POSIX forward slashes when keys are compared across platforms.
	return path.lexically_normal().generic_string();
}

std::vector<std::uint8_t> ResourceManager::ReadFileBytes(const std::filesystem::path& path) const
{
	const std::filesystem::path absolutePath = ResolveAssetPath(path);

	// Only TerraEngine's built-in default task/mesh/fragment shaders are
	// intentionally staged beside the runtime instead of inside the TEPAK.
	// Keep mount probes enabled for every other resource path.
	const bool isInstalledDefaultShader =
		absolutePath == (engineRoot / "Shaders" / "Default.task.tspf").lexically_normal()
		|| absolutePath == (engineRoot / "Shaders" / "Default.mesh.tspf").lexically_normal()
		|| absolutePath == (engineRoot / "Shaders" / "Default.frag.tspf").lexically_normal();

	// In export builds, the ContentMount provides access to assets packed into 
	// a .tepak archive. If a mount is active, we attempt to read through it 
	// using a logical path relative to the asset root.
	if (ContentMount::Instance().IsMounted() && !isInstalledDefaultShader)
	{
		const std::filesystem::path logicalPath = absolutePath.lexically_relative(assetRoot);
		const std::string logicalPathString = ContentMount::NormalizeLogicalPath(logicalPath.generic_string());

		TERRAENGINE_LOG(LogLevel::Debug, "Reading resource file from mount: " << logicalPathString);
		const std::vector<std::uint8_t> mountedBytes = ContentMount::Instance().ReadAll(logicalPathString);
		if (!mountedBytes.empty())
		{
			return mountedBytes;
		}

		// Mounted zero-byte files are valid authored assets. TEPAK returns an
		// empty buffer for them, so probe existence separately before treating
		// the empty read as a cache miss.
		if (ContentMount::Instance().Exists(logicalPathString))
		{
			TERRAENGINE_LOG(LogLevel::Debug, "Mounted resource file is empty: " << logicalPathString);
			return {};
		}

		TERRAENGINE_LOG(LogLevel::Warning, "Resource not found in mount: " << logicalPathString);
	}

	if (!std::filesystem::exists(absolutePath))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Resource file does not exist on disk: " << absolutePath);
		return {};
	}

	std::ifstream file(absolutePath, std::ios::binary | std::ios::ate);
	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open resource file: " << absolutePath);
		return {};
	}

	const std::streamsize length = file.tellg();
	if (length <= 0)
	{
		// Empty authored files are valid for assets such as placeholder UI
		// stylesheets, so return an empty payload without escalating to an
		// error. Callers that require non-empty content must validate it.
		TERRAENGINE_LOG(LogLevel::Debug, "Resource file is empty: " << absolutePath);
		return {};
	}

	// Emit a debug log with the resolved size so callers can correlate reads with disk usage.
	TERRAENGINE_LOG(LogLevel::Debug, "Reading resource file " << absolutePath << " (" << length << " bytes)");

	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length));

	// Sequential read is the simplest and most cache-friendly path because
	// each resource file is authored as a single contiguous blob.
	file.seekg(0, std::ios::beg);
	if (!file.read(reinterpret_cast<char*>(buffer.data()), length))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to read resource file: " << absolutePath);
		return {};
	}

	return buffer;
}
