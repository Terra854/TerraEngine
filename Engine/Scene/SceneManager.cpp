#include "Scene/SceneManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>
#include <rttr/variant.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include "Input/InputSystem.h"
#include "Serializer.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "Scene/CameraComponent.h"
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
	
	ResourceManager::Instance().LoadDefaultResources();

	//PreparePreviewAssets();
	// Initialization is logged at debug level so tooling can trace startup ordering.
	TERRAENGINE_LOG(LogLevel::Debug, "SceneManager initialized with root " << sceneRoot);
}

void SceneManager::Update(float deltaTime)
{
	// No per-frame updates currently required for the static scene management
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
	const SceneDocument* doc = GetScene(id);
	if (doc == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Attempted to activate scene that is not loaded");
		return false;
	}

	ObjectManager& objManager = ObjectManager::instance();
	objManager.clear();

	if (doc->parsedJson.contains("objects")) {
		for (const auto& objJson : doc->parsedJson["objects"]) {
			Serializer::DeserializeObject(objJson);
		}
	}
	else if (doc->parsedJson.contains("entities")) {
		for (const auto& objJson : doc->parsedJson["entities"]) {
			Serializer::DeserializeObject(objJson);
		}
	}

	activeSceneId = id;
	// New scenes start clean to avoid showing unsaved changes indicators
	// immediately after a successful load or swap.
	activeSceneDirty = false;
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

bool SceneManager::IsActiveSceneDirty() const
{
	return activeSceneDirty;
}

void SceneManager::MarkActiveSceneDirty()
{
	activeSceneDirty = true;
}

bool SceneManager::SaveScene(std::optional<std::filesystem::path> path)
{
	const SceneDocument* activeScene = GetActiveScene();
	if (!activeScene)
	{
		TERRAENGINE_LOG(LogLevel::Error, "No active scene to save.");
		return false;
	}

	// Default to overwriting the original source if no alternative path is
	// provided by the caller (e.g. a "Save As" operation).
	std::filesystem::path targetPath;
	if (path.has_value())
	{
		targetPath = *path;
	}
	else
	{
		targetPath = activeScene->sourcePath;
	}
	
	if (!targetPath.is_absolute()) {
		targetPath = ResolveScenePath(targetPath);
	}

	nlohmann::json sceneJson;
	sceneJson["name"] = activeScene->name;
	sceneJson["objects"] = nlohmann::json::array();

	ObjectManager& objManager = ObjectManager::instance();
	auto roots = objManager.getRootObjects();

	// Leverage the centralized Serializer to convert the live object graph 
	// into a JSON structure suitable for disk storage.
	for (auto& root : roots) {
		sceneJson["objects"].push_back(Serializer::SerializeObject(root));
	}

	std::ofstream file(targetPath);
	if (!file.is_open()) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open file for writing: " << targetPath);
		return false;
	}

	file << sceneJson.dump(4);
	file.close();

	TERRAENGINE_LOG(LogLevel::Info, "Saved scene to " << targetPath);
	
	// Update the cache if we overwrote the currently active scene file so subsequent
	// reloads (or SetActiveScene calls) see the fresh data.
	if (!path.has_value() || *path == activeScene->sourcePath)
	{
		std::unique_lock lock(sceneCache.mutex);
		if (auto it = sceneCache.resources.find(activeSceneId); it != sceneCache.resources.end())
		{
			if (it->second)
			{
				it->second->parsedJson = sceneJson;
				it->second->jsonContents = sceneJson.dump(4);
			}
		}
	}

	// Resetting the dirty flag after a successful disk write ensures the editor
	// UI accurately reflects that the current state is safely persisted.
	activeSceneDirty = false;
	
	return true;
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
	// Remove the generated preview entity so repeated initializes do not leave
	// stray objects in the registry when the manager is reused.
	if (previewObject)
	{
		ObjectManager::instance().removeObject(*previewObject);
		previewObject.reset();
	}
	if (previewCamera)
	{
		ObjectManager::instance().removeObject(*previewCamera);
		previewCamera.reset();
	}
	isDraggingPreview = false;
	lastCursorPosition = glm::vec2(0.0f, 0.0f);
	accumulatedYaw = 0.0f;
	accumulatedPitch = 0.0f;
	previewZoomOffset = 0.0f;
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
	// editors do not need to generate shader source at runtime. These come from
	// Engine/Shaders/Default and serve as the engine's embedded fallback set.
	previewShaderCollectionId = resources.GetDefaultShaderCollectionID();
	if (previewShaderCollectionId == TerraEngine::EMPTY_UUID)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Preview shader collection is incomplete; rendering components will not reference a shader set.");
	}

	previewMaterialId = CreateGeneratedMaterial(resources, "GeneratedTextureMaterial", previewTextureId, previewShaderCollectionId);

	// Instantiate an object with the bindings so future renderer work can query
	// exactly which authored assets should be drawn.
	SpawnPreviewObject(previewMeshId, previewMaterialId, previewTextureId);
	// Seed the scene with a camera component that mirrors the old hardcoded
	// view so the renderer can source matrices from authored data instead.
	SpawnPreviewCamera();
}

TerraEngine::UUID SceneManager::CreateGeneratedMaterial(ResourceManager& resources, const std::string& name, const TerraEngine::UUID& textureId, const TerraEngine::UUID& shaderCollectionId)
{
	// Build a minimal in-memory material that binds the preview texture to the base color slot so
	// render components can reference it through the shared ResourceManager cache without writing TMAT files.
	MaterialHeader header{};
	std::memcpy(header.magic, "TMAT", sizeof(header.magic));
	header.version = kMaterialCurrentVersion;
	header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
	header.flags = 0;
	header.shadingModel = static_cast<std::uint32_t>(ShadingModel::Unlit);
	header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
	header.shaderCollectionId = shaderCollectionId;
	header.baseColorFactor[0] = 1.0f;
	header.baseColorFactor[1] = 1.0f;
	header.baseColorFactor[2] = 1.0f;
	header.baseColorFactor[3] = 1.0f;
	header.emissiveFactor[0] = 0.0f;
	header.emissiveFactor[1] = 0.0f;
	header.emissiveFactor[2] = 0.0f;
	header.metallicFactor = 0.0f;
	header.roughnessFactor = 1.0f;
	header.normalScale = 1.0f;
	header.occlusionStrength = 1.0f;
	header.alphaCutoff = 0.5f;
	header.textureCount = 1;
	header.floatParameterCount = 0;
	header.textureArrayOffset = header.headerSize;
	header.floatParameterOffset = header.headerSize + static_cast<std::uint32_t>(sizeof(TextureBinding));

	TextureBinding binding{};
	binding.semantic = TextureSemantic::BaseColor;
	binding.textureUuid = textureId;
	binding.samplerFilter = SamplerFilter::Trilinear;
	binding.addressU = SamplerAddressMode::Repeat;
	binding.addressV = SamplerAddressMode::Repeat;
	binding.addressW = SamplerAddressMode::Repeat;
	binding.uvSetIndex = 0;
	binding.uvScale[0] = 1.0f;
	binding.uvScale[1] = 1.0f;
	binding.uvOffset[0] = 0.0f;
	binding.uvOffset[1] = 0.0f;

auto material = std::make_unique<Material>();
material->header = header;
material->shaderCollectionId = shaderCollectionId;
material->storage.resize(sizeof(TextureBinding));
std::memcpy(material->storage.data(), &binding, sizeof(TextureBinding));
	material->textures = std::span<const TextureBinding>(reinterpret_cast<const TextureBinding*>(material->storage.data()), header.textureCount);
	material->floats = std::span<const FloatParameter>();

	return resources.RegisterMaterial(name, std::move(material));
}

void SceneManager::SpawnPreviewObject(const TerraEngine::UUID& meshId, const TerraEngine::UUID& materialId, const TerraEngine::UUID& textureId)
{
	ObjectManager& objects = ObjectManager::instance();
	// Persist the preview object so input-driven camera controls can adjust it
	// every frame without re-querying the registry.
	previewObject = objects.createObject("GeneratedPreview");

	// Initialize transform through the generic setter so locks are respected.
	Transform* t = previewObject->getComponent<Transform>();
	if (t)
	{
		SetComponentProperty(*t, "Position", glm::vec3(0.0f, 0.0f, 0.0f));
		SetComponentProperty(*t, "Rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		SetComponentProperty(*t, "Scale", glm::vec3(1.0f, 1.0f, 1.0f));
	}

	// Attach a simple render binding component so downstream renderer work can
	// read which authored assets should be drawn together.
	auto& renderComponent = previewObject->addComponent<MeshRenderComponent>();
	// Bind authored assets via the reflection-based setter to ensure thread-safe writes.
	SetComponentProperty(renderComponent, "meshId", meshId);
	SetComponentProperty(renderComponent, "materialId", materialId);
}

void SceneManager::SpawnPreviewCamera()
{
	ObjectManager& objects = ObjectManager::instance();
	previewCamera = objects.createObject("PreviewCamera");

	// Match the legacy hardcoded camera by positioning it three units away
	// from the origin while keeping an identity rotation and uniform scale.
	Transform* transform = previewCamera->getComponent<Transform>();
	if (transform)
	{
		SetComponentProperty(*transform, "Position", glm::vec3(0.0f, 0.0f, 3.0f));
		SetComponentProperty(*transform, "Rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		SetComponentProperty(*transform, "Scale", glm::vec3(1.0f, 1.0f, 1.0f));
	}

	// Add a camera component with projection defaults that mirror the previous
	// hardcoded parameters so the preview looks identical.
	auto& camera = previewCamera->addComponent<CameraComponent>();
	SetComponentProperty(camera, "verticalFovDegrees", 45.0f);
	SetComponentProperty(camera, "aspectRatio", 16.0f / 9.0f);
	SetComponentProperty(camera, "nearPlane", 0.1f);
	SetComponentProperty(camera, "farPlane", 100.0f);
	SetComponentProperty(camera, "up", glm::vec3(0.0f, 1.0f, 0.0f));
}

Transform* SceneManager::GetPreviewTransform()
{
	if (!previewObject)
	{
		return nullptr;
	}

	return previewObject->getComponent<Transform>();
}
