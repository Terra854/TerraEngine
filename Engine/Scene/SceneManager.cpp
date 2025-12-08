#include "Scene/SceneManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>
#include <nlohmann/json.hpp>

#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "Scene/MeshRenderComponent.h"

SceneManager& SceneManager::Instance()
{
	static SceneManager instance;
	return instance;
}

SceneManager::~SceneManager() = default;

SceneManager::SceneManager()
	: threadPool(ThreadPool::instance())
{
	// Default to the process working directory so authored scene paths can be
	// relative to the project root until a caller overrides the location.
	sceneRoot = std::filesystem::current_path();
}

void SceneManager::Initialize()
{
	// Clear any stale tracking in case the manager is restarted during a hot
	// reload. At this stage nothing is loaded yet so the reset is cheap and
	// keeps stale cache entries from leaking across sessions.
	UnloadAll();
	PreparePreviewAssets();
	// Initialization is logged at debug level so tooling can trace startup ordering.
	TERRAENGINE_LOG(LogLevel::Debug, "SceneManager initialized with root " << sceneRoot);
}

void SceneManager::Update()
{
	// Scene loading is handled via explicit requests and shared futures, so the
	// update step is intentionally empty. Keeping it in place preserves the
	// SystemsBase contract for future streaming logic.
}

void SceneManager::Shutdown()
{
	// Release cached JSON blobs and let any outstanding futures drop their
	// references so the thread pool can finish without holding scene data.
	UnloadAll();
}

std::string SceneManager::SystemName()
{
	return "SceneManager";
}

void SceneManager::SetSceneRoot(const std::filesystem::path& root)
{
	// Normalize immediately so repeated calls with different relative forms map
	// to the same cache keys.
	sceneRoot = root.lexically_normal();
	// Emit debug output so asset lookups can be traced back to the configured root.
	TERRAENGINE_LOG(LogLevel::Debug, "Scene root set to " << sceneRoot);
}

std::filesystem::path SceneManager::GetSceneRoot() const
{
	return sceneRoot;
}

TerraEngine::UUID SceneManager::LoadScene(const std::filesystem::path& path)
{
	// Trace synchronous scene requests to help diagnose cache behavior during authoring.
	TERRAENGINE_LOG(LogLevel::Debug, "Request to load scene " << path);
	return LoadOrGetCached<SceneDocument>(path, sceneCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Scene);
}

TerraEngine::UUID SceneManager::LoadPrefab(const std::filesystem::path& path)
{
	// Trace synchronous prefab requests so missing assets are obvious at debug verbosity.
	TERRAENGINE_LOG(LogLevel::Debug, "Request to load prefab " << path);
	return LoadOrGetCached<SceneDocument>(path, prefabCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Prefab);
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadSceneAsync(const std::filesystem::path& path)
{
	// Debug logging clarifies when background scene streaming is requested.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async load for scene " << path);
	return LoadOrSchedule<SceneDocument>(path, sceneCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Scene);
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadPrefabAsync(const std::filesystem::path& path)
{
	// Debug logging clarifies when background prefab streaming is requested.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async load for prefab " << path);
	return LoadOrSchedule<SceneDocument>(path, prefabCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Prefab);
}

const SceneDocument* SceneManager::GetScene(const TerraEngine::UUID& id) const
{
	return GetResourceById(id, sceneCache);
}

const SceneDocument* SceneManager::GetPrefab(const TerraEngine::UUID& id) const
{
	return GetResourceById(id, prefabCache);
}

bool SceneManager::SetActiveScene(const TerraEngine::UUID& id)
{
	// The active scene must already be loaded; callers can invoke the async
	// load helpers beforehand and wait on the returned future.
	if (GetScene(id) == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Attempted to activate scene that is not loaded");
		return false;
	}

	activeSceneId = id;
	// Log the swap so editors can track which scene is currently bound.
	TERRAENGINE_LOG(LogLevel::Debug, "Active scene set to id " << ToHexString(id));
	return true;
}

const SceneDocument* SceneManager::GetActiveScene() const
{
	if (activeSceneId == TerraEngine::EMPTY_UUID)
	{
		return nullptr;
	}

	return GetScene(activeSceneId);
}

void SceneManager::UnloadAll()
{
	// Acquire locks in a stable order to avoid deadlocks if multiple threads
	// request clears simultaneously. Each cache is isolated so scene/prefab
	// data can be reclaimed independently in the future.
	{
		std::unique_lock lock(sceneCache.mutex);
		sceneCache.pathToId.clear();
		sceneCache.resources.clear();
		sceneCache.inFlight.clear();
	}
	{
		std::unique_lock lock(prefabCache.mutex);
		prefabCache.pathToId.clear();
		prefabCache.resources.clear();
		prefabCache.inFlight.clear();
	}

	activeSceneId = TerraEngine::EMPTY_UUID;
	// Log cache flushes so teardown/hot-reload steps are easy to correlate in logs.
	TERRAENGINE_LOG(LogLevel::Debug, "SceneManager caches cleared");
}

std::unique_ptr<SceneDocument> SceneManager::LoadSceneDocument(const std::filesystem::path& path, SceneFileType type)
{
	const auto jsonText = ReadJsonFile(path, type);
	if (!jsonText)
	{
		return nullptr;
	}

	nlohmann::json parsed;
	try
	{
		// Parse once during load so downstream systems do not need to repeat
		// JSON work every time the scene data is accessed.
		parsed = nlohmann::json::parse(*jsonText);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Scene file failed JSON parse at byte " << parseError.byte << ": " << parseError.what() << " Path: " << path);
		return nullptr;
	}

	auto document = std::make_unique<SceneDocument>();
	document->sourcePath = path;
	document->name = path.stem().string();
	document->jsonContents = *jsonText;
	document->parsedJson = std::move(parsed);
	document->isPrefab = type == SceneFileType::Prefab;
	// Announce successful parse to make asset churn visible at debug verbosity.
	TERRAENGINE_LOG(LogLevel::Debug, "Loaded " << (type == SceneFileType::Prefab ? "prefab " : "scene ") << document->name << " from " << path);
	return document;
}

std::optional<std::string> SceneManager::ReadJsonFile(const std::filesystem::path& path, SceneFileType type) const
{
	if (!ValidateExtension(path, type))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Rejected load for " << path << " because the extension does not match the expected scene or prefab format.");
		return std::nullopt;
	}

	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open scene file: " << path);
		return std::nullopt;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string jsonText = buffer.str();

	if (jsonText.empty())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Scene file is empty: " << path);
		return std::nullopt;
	}

	return jsonText;
}

bool SceneManager::ValidateExtension(const std::filesystem::path& path, SceneFileType type) const
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (type == SceneFileType::Scene)
	{
		return extension == ".tescene";
	}

	return extension == ".teprefab";
}

std::filesystem::path SceneManager::ResolveScenePath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (sceneRoot / path).lexically_normal();
}

std::string SceneManager::NormalizeKey(const std::filesystem::path& path) const
{
	// Using generic_string keeps path separators consistent on Windows and POSIX
	// so cache keys are stable across different callers.
	return path.lexically_normal().generic_string();
}

void SceneManager::PreparePreviewAssets()
{
	ResourceManager& resources = ResourceManager::Instance();
	resources.SetAssetRoot(sceneRoot);

	// Load the authored mesh/texture assets that the editor will display by default.
	previewMeshId = resources.LoadMesh("model.temf");
	if (resources.GetMesh(previewMeshId) == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Preview mesh (model.temf) failed to load; generated object will be incomplete.");
	}

	previewTextureId = resources.LoadTexture("texture.tetf");
	if (resources.GetTexture(previewTextureId) == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Preview texture (texture.tetf) failed to load; material will fall back to defaults.");
	}

	// Load the precompiled preview shaders that ship alongside the executable so
	// editors do not need to generate shader source at runtime.
	previewShaderCollectionId = LoadPreviewShaderCollection(resources);
	if (previewShaderCollectionId == TerraEngine::EMPTY_UUID)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Preview shader collection is incomplete; rendering components will not reference a shader set.");
	}

	previewMaterialId = CreateGeneratedMaterial("GeneratedTextureMaterial", previewTextureId, previewShaderCollectionId);

	// Instantiate an object with the bindings so future renderer work can query
	// exactly which authored assets should be drawn.
	SpawnPreviewObject(previewMeshId, previewMaterialId, previewTextureId, previewShaderCollectionId);
}

TerraEngine::UUID SceneManager::LoadPreviewShaderCollection(ResourceManager& resources) const
{
	// Build a validated vertex/fragment pairing so preview renders never mix mismatched shader stages.
	std::vector<ShaderStagePath> stages = {
		{ ShaderStage::Vertex, "PreviewTextured.vert.tspf" },
		{ ShaderStage::Fragment, "PreviewTextured.frag.tspf" }
	};
	const TerraEngine::UUID shaderCollectionId = resources.LoadShaderCollection(stages);
	ShaderCollection* shaderCollection = resources.GetShaderCollection(shaderCollectionId);
	if (shaderCollection == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create preview shader collection; verify the compiled TSPF stages are deployed next to the executable.");
		return TerraEngine::EMPTY_UUID;
	}

	bool hasVertex = false;
	bool hasFragment = false;
	for (const ShaderCollection::StageBinding& stage : shaderCollection->stages)
	{
		if (stage.stage == ShaderStage::Vertex)
		{
			hasVertex = stage.shaderId != TerraEngine::EMPTY_UUID;
		}
		else if (stage.stage == ShaderStage::Fragment)
		{
			hasFragment = stage.shaderId != TerraEngine::EMPTY_UUID;
		}
	}

	if (!hasVertex || !hasFragment)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Preview shader collection is missing a stage; rendering components will skip shader binding.");
		return TerraEngine::EMPTY_UUID;
	}

	return shaderCollectionId;
}

TerraEngine::UUID SceneManager::CreateGeneratedMaterial(const std::string& name, const TerraEngine::UUID& textureId, const TerraEngine::UUID& shaderCollectionId)
{
	GeneratedMaterial material{};
	material.id = TerraEngine::GenerateUUID();
	material.textureId = textureId;
	material.shaderCollectionId = shaderCollectionId;
	material.name = name;
	generatedMaterials[material.id] = material;
	return material.id;
}

void SceneManager::SpawnPreviewObject(const TerraEngine::UUID& meshId, const TerraEngine::UUID& materialId, const TerraEngine::UUID& textureId, const TerraEngine::UUID& shaderCollectionId)
{
	ObjectManager& objects = ObjectManager::instance();
	auto preview = objects.createObject("GeneratedPreview");

	auto t = preview.getComponent<Transform>();
	t->position = glm::vec3(0.0f, 0.0f, 0.0f);
	t->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	t->scale = glm::vec3(1.0f, 1.0f, 1.0f);

	// Attach a simple render binding component so downstream renderer work can
	// read which authored assets should be drawn together.
	auto& renderComponent = preview.addComponent<MeshRenderComponent>();
	renderComponent.meshId = meshId;
	renderComponent.materialId = materialId;
	renderComponent.shaderCollectionId = shaderCollectionId;
}
