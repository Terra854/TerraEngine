// SceneManager.cpp — Scene Lifecycle Manager Implementation
//
// Implements the core logic for managing the engine's scene lifecycle and active document state.
// It handles the asynchronous loading of scene files and coordinates with the ECS and Resource systems to populate the world.
// This module ensures that scene transitions are handled safely and that all runtime systems are correctly synchronized.

#include "Scene/SceneManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include "Input/InputSystem.h"
#include "PrefabComposition.h"
#include "SceneGraphRuntime.h"
#include "Serializer.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/SkyComponent.h"

namespace
{
	// LoadSceneRenderSettingsFromDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SceneRenderSettings LoadSceneRenderSettingsFromDocument(const SceneDocument& document)
	{
		if (const auto sceneSettingsIt = document.parsedJson.find("sceneSettings"); sceneSettingsIt != document.parsedJson.end())
		{
			return Serializer::DeserializeSceneRenderSettings(*sceneSettingsIt);
		}

		return MakeDefaultSceneRenderSettings();
	}

	// RemoveValidationLights is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void RemoveValidationLights(ObjectManager& objects)
	{
		// Validation fixtures need deterministic light ownership, so remove any
		// prior light-only objects before spawning the new arrangement.
		const std::vector<entt::entity> existingLights = objects.getEntitiesWithComponents<LightComponent>();
		for (entt::entity lightObject : existingLights)
		{
			objects.removeObject(lightObject);
		}
	}

	// ConfigureValidationPointLight is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ConfigureValidationPointLight(ObjectManager& objects, entt::entity lightObject, const glm::vec3& position, const glm::vec3& color, float intensity, float range)
	{
		if (Transform* transform = objects.GetComponent<Transform>(lightObject))
		{
			SetComponentProperty(*transform, "Position", position);
			objects.MarkObjectTransformDirty(lightObject);
		}

		if (LightComponent* light = objects.AddComponent<LightComponent>(lightObject))
		{
			SetComponentProperty(*light, "type", static_cast<std::uint32_t>(LightType::Point));
			SetComponentProperty(*light, "color", color);
			SetComponentProperty(*light, "intensity", std::max(intensity, 0.0f));
			SetComponentProperty(*light, "range", std::max(range, 0.1f));
			SetComponentProperty(*light, "castsShadows", false);
		}
	}
}

// SceneManager::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SceneManager& SceneManager::Instance()
{
	static SceneManager instance;
	return instance;
}

	SceneManager::~SceneManager() = default;

SceneManager::SceneManager()
	// threadPool is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: threadPool(ThreadPool::instance())
{
	// Default to the process working directory so authored scene paths can be
	// relative to the project root until a caller overrides the location.
	sceneRoot = std::filesystem::current_path();
}

// SceneManager::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::Initialize()
{
	// Register SkyComponent before any scene data is deserialized so reflected
	// component JSON can create the component through the normal object path.
	SkyComponent::registerType();

	// Clear any stale tracking in case the manager is restarted during a hot
	// reload. At this stage nothing is loaded yet so the reset is cheap and
	// keeps stale cache entries from leaking across sessions.
	UnloadAll();
	
	ResourceManager::Instance().LoadDefaultResources();

	// If a startup scene was configured (e.g., via the export manifest), load 
	// and activate it immediately so the engine boots directly into gameplay.
	if (!m_StartupScene.empty())
	{
		const auto id = LoadScene(m_StartupScene);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (id != TerraEngine::EMPTY_UUID)
		{
			SetActiveScene(id);
			TERRAENGINE_LOG(LogLevel::Info, "Booted into startup scene: " << m_StartupScene);
		}
		else
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to load configured startup scene: " << m_StartupScene);
		}
	}

	//PreparePreviewAssets();
	// Initialization is logged at debug level so tooling can trace startup ordering.
	TERRAENGINE_LOG(LogLevel::Debug, "SceneManager initialized with root " << sceneRoot);
}

// SceneManager::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// SceneManager::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string SceneManager::SystemName()
{
	return "SceneManager";
}

// SceneManager::SetSceneRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SetSceneRoot(const std::filesystem::path& root)
{
	// Normalize immediately so repeated calls with different relative forms map
	// to the same cache keys.
	sceneRoot = root.lexically_normal();
	// Emit debug output so asset lookups can be traced back to the configured root.
	TERRAENGINE_LOG(LogLevel::Debug, "Scene root set to " << sceneRoot);
}

// SceneManager::GetSceneRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path SceneManager::GetSceneRoot() const
{
	return sceneRoot;
}

// SceneManager::SetStartupScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SetStartupScene(const std::filesystem::path& path)
{
	m_StartupScene = path;
}

// SceneManager::GetStartupScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path SceneManager::GetStartupScene() const
{
	return m_StartupScene;
}

// SceneManager::LoadScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID SceneManager::LoadScene(const std::filesystem::path& path)
{
	// Trace synchronous scene requests to help diagnose cache behavior during authoring.
	TERRAENGINE_LOG(LogLevel::Debug, "Request to load scene " << path);
	return LoadOrGetCached<SceneDocument>(path, sceneCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Scene);
}

// SceneManager::LoadPrefab is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID SceneManager::LoadPrefab(const std::filesystem::path& path)
{
	// Trace synchronous prefab requests so missing assets are obvious at debug verbosity.
	TERRAENGINE_LOG(LogLevel::Debug, "Request to load prefab " << path);
	const TerraEngine::UUID prefabId = LoadOrGetCached<SceneDocument>(path, prefabCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Prefab);
	if (const SceneDocument* prefab = GetPrefab(prefabId))
	{
		TerraEngine::Scene::PrefabSource source;
		source.sourceId = prefabId;
		source.sourcePath = prefab->sourcePath;
		source.sourceGraph = TerraEngine::Scene::BuildSceneGraphFromJson(prefab->parsedJson);
		TerraEngine::Scene::ValidatePrefabSource(source);
		for (const TerraEngine::Scene::SceneDiagnostic& diagnostic : source.sourceGraph.diagnostics)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Prefab composition diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
		}
	}
	return prefabId;
}

// SceneManager::LoadSceneAsync is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> SceneManager::LoadSceneAsync(const std::filesystem::path& path)
{
	// Debug logging clarifies when background scene streaming is requested.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async load for scene " << path);
	return LoadOrSchedule<SceneDocument>(path, sceneCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Scene);
}

// SceneManager::LoadPrefabAsync is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> SceneManager::LoadPrefabAsync(const std::filesystem::path& path)
{
	// Debug logging clarifies when background prefab streaming is requested.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async load for prefab " << path);
	return LoadOrSchedule<SceneDocument>(path, prefabCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Prefab);
}

// SceneManager::GetScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const SceneDocument* SceneManager::GetScene(const TerraEngine::UUID& id) const
{
	return GetResourceById(id, sceneCache);
}

// SceneManager::GetPrefab is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const SceneDocument* SceneManager::GetPrefab(const TerraEngine::UUID& id) const
{
	return GetResourceById(id, prefabCache);
}

// SceneManager::SetActiveScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	SkyComponent::registerType();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (doc->parsedJson.contains("objects")) {
		for (const auto& objJson : doc->parsedJson["objects"]) {
			Serializer::DeserializeObjectEntity(objJson);
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (doc->parsedJson.contains("entities")) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& objJson : doc->parsedJson["entities"]) {
			Serializer::DeserializeObjectEntity(objJson);
		}
	}

	auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objManager);
	for (const TerraEngine::Scene::SceneDiagnostic& diagnostic : graph.diagnostics)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Scene graph load diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
	}

	activeSceneId = id;
	activeSceneRenderSettings = LoadSceneRenderSettingsFromDocument(*doc);
	// New scenes start clean to avoid showing unsaved changes indicators
	// immediately after a successful load or swap.
	activeSceneDirty = false;
	RenderSystem::Instance().MarkSkyStateDirty();
	// Log the swap so editors can track which scene is currently bound.
	TERRAENGINE_LOG(LogLevel::Debug, "Active scene set to id " << ToHexString(id));
	return true;
}

// SceneManager::GetActiveScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const SceneDocument* SceneManager::GetActiveScene() const
{
	if (activeSceneId == TerraEngine::EMPTY_UUID)
	{
		return nullptr;
	}

	return GetScene(activeSceneId);
}

// SceneManager::IsActiveSceneDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool SceneManager::IsActiveSceneDirty() const
{
	return activeSceneDirty || activeSceneSettingsDirty;
}

// SceneManager::GetActiveSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const SceneRenderSettings& SceneManager::GetActiveSceneRenderSettings() const
{
	return activeSceneRenderSettings;
}

// SceneManager::SetActiveSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SetActiveSceneRenderSettings(const SceneRenderSettings& settings)
{
	activeSceneRenderSettings = SanitizeSceneRenderSettings(settings);
	activeSceneDirty = true;
}

// SceneManager::GetActiveSkyComponent resolves the first active SkyComponent in
// deterministic scene traversal order. Callers must treat the returned pointer
// as a read-only view into the current ObjectManager state.
const SkyComponent* SceneManager::GetActiveSkyComponent() const
{
	std::optional<entt::entity> skyObject = GetActiveSkyObject();
	if (!skyObject.has_value())
	{
		return nullptr;
	}

	return ObjectManager::instance().GetComponent<SkyComponent>(*skyObject);
}

// SceneManager::GetActiveSkyObject resolves the object that owns the first
// active SkyComponent. The owning object matters because its LightComponent is
// now the only source allowed to drive sky sun direction.
std::optional<entt::entity> SceneManager::GetActiveSkyObject() const
{
	ObjectManager& objManager = ObjectManager::instance();
	const TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objManager, false);
	const std::vector<TerraEngine::UUID> traversalOrder = TerraEngine::Scene::BuildTraversalOrder(graph);
	for (const TerraEngine::UUID& objectId : traversalOrder)
	{
		std::optional<entt::entity> object = objManager.getEntity(objectId);
		if (!object.has_value())
		{
			continue;
		}
		if (!objManager.isObjectActive(*object))
		{
			continue;
		}

		const SkyComponent* skyComponent = objManager.GetComponent<SkyComponent>(*object);
		if (skyComponent == nullptr || !skyComponent->active)
		{
			continue;
		}

		return object;
	}

	return std::nullopt;
}

// SceneManager::HasActiveSkyComponent reports whether sky authoring exists on
// an active scene object without exposing any redundant scene-level state.
bool SceneManager::HasActiveSkyComponent() const
{
	return GetActiveSkyObject().has_value();
}

// SceneManager::MarkActiveSceneSkyStateDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::MarkActiveSceneSkyStateDirty()
{
	RenderSystem::Instance().MarkSkyStateDirty();
}

// SceneManager::ForceMarkActiveSceneDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::ForceMarkActiveSceneDirty()
{
	activeSceneDirty = true;
	MarkActiveSceneSkyStateDirty();
}

// SceneManager::ClearActiveSceneDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::ClearActiveSceneDirty()
{
	activeSceneDirty = false;
}

TerraEngine::Scene::TransactionDiagnostics SceneManager::GetTransactionDiagnostics() const
{
	return TerraEngine::Scene::SceneMutationQueue::Instance().GetTransactionDiagnostics();
}

TerraEngine::Scene::TransactionResult SceneManager::PreflightTransaction(const TerraEngine::Scene::Transaction& transaction) const
{
	return TerraEngine::Scene::SceneMutationQueue::Instance().PreflightTransaction(transaction, ObjectManager::instance());
}

TerraEngine::Scene::SceneMutationBatchResult SceneManager::ApplySceneMutations(std::string commitName)
{
	// All transaction commits route through this protected scene-manager entry
	// point so the dirty flag and sky-state invalidation stay coupled to the
	// ObjectManager write that actually made it through validation.
	TerraEngine::Scene::SceneMutationBatchResult result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(ObjectManager::instance(), std::move(commitName));
	if (result.applied && result.acceptedMutationCount > 0)
	{
		ForceMarkActiveSceneDirty();
	}
	return result;
}

// SceneManager::SaveScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool SceneManager::SaveScene(std::optional<std::filesystem::path> path)
{
	const SceneDocument* activeScene = GetActiveScene();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!targetPath.is_absolute()) {
		targetPath = ResolveScenePath(targetPath);
	}

	nlohmann::json sceneJson;
	sceneJson["name"] = activeScene->name;
	sceneJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(activeSceneRenderSettings);
	sceneJson["objects"] = nlohmann::json::array();

	ObjectManager& objManager = ObjectManager::instance();
	auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objManager);
	for (const TerraEngine::Scene::SceneDiagnostic& diagnostic : graph.diagnostics)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Scene graph save diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
	}

	// Leverage the centralized Serializer to convert the live object graph 
	// into a JSON structure suitable for disk storage.
	for (const TerraEngine::UUID& rootId : graph.rootObjects) {
		if (auto root = objManager.getEntity(rootId))
		{
			sceneJson["objects"].push_back(Serializer::SerializeObject(*root));
		}
	}

	std::ofstream file(targetPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	ClearActiveSceneDirty();
	
	return true;
}

// SceneManager::UnloadAll is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	activeSceneDirty = false;
	activeSceneSettingsDirty = false;
	activeSceneRenderSettings = MakeDefaultSceneRenderSettings();
	// Remove the generated preview entity so repeated initializes do not leave
	// stray objects in the registry when the manager is reused.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (previewObject)
	{
		ObjectManager::instance().removeObject(*previewObject);
		previewObject.reset();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// SceneManager::NullifyAssetReferences is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::NullifyAssetReferences(const TerraEngine::UUID& assetId)
{
	// MainEngine mirrors this mesh/material asset scan into EcsQueryTaskGraph metadata so scheduler diagnostics can
	// reason about the component access before this protected scene-maintenance path mutates existing components.
	ObjectManager::instance().forEachEntityWithComponents<MeshRenderComponent>(
		[&](entt::entity object, MeshRenderComponent& meshComp)
		{
			bool changed = false;
			const TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "meshId");
			const TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "materialId");

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (meshId == assetId)
			{
				SetComponentProperty(meshComp, "meshId", TerraEngine::EMPTY_UUID);
				changed = true;
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (materialId == assetId)
			{
				SetComponentProperty(meshComp, "materialId", TerraEngine::EMPTY_UUID);
				changed = true;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (changed)
			{
				activeSceneDirty = true;
			}
		}
	);

	// SkyComponent source assets are stored as friendly asset paths rather than
	// raw UUIDs, so missing textures degrade through renderer fallback instead
	// of being hard-cleared during asset deletion.
}

void SceneManager::SpawnLightStressTestGrid(std::uint32_t gridWidth, std::uint32_t gridDepth, float spacing, float intensity)
{
	ObjectManager& objects = ObjectManager::instance();
	const std::uint32_t safeGridWidth = std::max<std::uint32_t>(1u, gridWidth);
	const std::uint32_t safeGridDepth = std::max<std::uint32_t>(1u, gridDepth);
	const float safeSpacing = std::max(0.1f, spacing);
	const float safeIntensity = std::max(0.0f, intensity);

	const float centerX = static_cast<float>(safeGridWidth - 1u) * 0.5f;
	const float centerZ = static_cast<float>(safeGridDepth - 1u) * 0.5f;

	// Spawn in a deterministic pattern so benchmark runs are directly comparable.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t z = 0; z < safeGridDepth; ++z)
	{
		for (std::uint32_t x = 0; x < safeGridWidth; ++x)
		{
			auto lightObject = objects.createObject("StressPointLight");
			const entt::entity lightEntity = lightObject.EntityId();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (Transform* transform = objects.GetComponent<Transform>(lightEntity))
			{
				const float worldX = (static_cast<float>(x) - centerX) * safeSpacing;
				const float worldZ = (static_cast<float>(z) - centerZ) * safeSpacing;
				const float worldY = 1.5f + std::sin(static_cast<float>(x + z) * 0.37f) * 0.5f;
				SetComponentProperty(*transform, "Position", glm::vec3(worldX, worldY, worldZ));
				objects.MarkObjectTransformDirty(lightEntity);
			}

			if (LightComponent* light = objects.AddComponent<LightComponent>(lightEntity))
			{
				SetComponentProperty(*light, "type", static_cast<std::uint32_t>(LightType::Point));
				SetComponentProperty(*light, "color", glm::vec3(
					0.4f + static_cast<float>((x * 17u + z * 3u) % 60u) / 100.0f,
					0.4f + static_cast<float>((x * 11u + z * 5u) % 60u) / 100.0f,
					0.4f + static_cast<float>((x * 7u + z * 13u) % 60u) / 100.0f));
				SetComponentProperty(*light, "intensity", safeIntensity);
				SetComponentProperty(*light, "range", std::max(3.0f, safeSpacing * 1.75f));
				SetComponentProperty(*light, "castsShadows", false);
			}
		}
	}

	ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Spawned stress test light grid with " << (safeGridWidth * safeGridDepth) << " point lights.");
}

// SceneManager::SpawnClusterBinDenseFixture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SpawnClusterBinDenseFixture()
{
	ObjectManager& objects = ObjectManager::instance();
	RemoveValidationLights(objects);

	// Pack more than 64 lights into a tight volume near the origin so the
	// clustered-light build is forced to produce at least one dense range.
	constexpr std::uint32_t kDenseLightCount = 96u;
	for (std::uint32_t lightIndex = 0u; lightIndex < kDenseLightCount; ++lightIndex)
	{
		const float ringIndex = static_cast<float>(lightIndex % 12u);
		const float stackIndex = static_cast<float>(lightIndex / 12u);
		const float angle = ringIndex * glm::two_pi<float>() / 12.0f;
		const float radius = 0.35f + static_cast<float>(lightIndex % 3u) * 0.08f;
		const glm::vec3 position(
			std::cos(angle) * radius,
			0.75f + stackIndex * 0.18f,
			-5.5f + std::sin(angle) * radius);
		const glm::vec3 color(
			0.55f + static_cast<float>((lightIndex * 7u) % 30u) / 100.0f,
			0.55f + static_cast<float>((lightIndex * 11u) % 25u) / 100.0f,
			0.60f + static_cast<float>((lightIndex * 13u) % 20u) / 100.0f);

		ConfigureValidationPointLight(
			objects,
			objects.createObject("DenseClusterValidationLight").EntityId(),
			position,
			color,
			24.0f,
			8.0f);
	}

	ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Spawned dense cluster-bin validation fixture with " << kDenseLightCount << " point lights.");
}

// SceneManager::SpawnClusterBinSparseFixture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SpawnClusterBinSparseFixture()
{
	ObjectManager& objects = ObjectManager::instance();
	RemoveValidationLights(objects);

	// Spread lights far apart so the validation run observes many empty
	// clusters while still keeping a few populated ranges for correctness.
	constexpr std::uint32_t kSparseWidth = 6u;
	constexpr std::uint32_t kSparseDepth = 5u;
	const float spacing = 12.0f;
	const float centerX = static_cast<float>(kSparseWidth - 1u) * 0.5f;
	const float centerZ = static_cast<float>(kSparseDepth - 1u) * 0.5f;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t z = 0u; z < kSparseDepth; ++z)
	{
		for (std::uint32_t x = 0u; x < kSparseWidth; ++x)
		{
			const glm::vec3 position(
				(static_cast<float>(x) - centerX) * spacing,
				1.25f + static_cast<float>((x + z) % 3u) * 0.2f,
				-12.0f + (static_cast<float>(z) - centerZ) * spacing);
			const glm::vec3 color(
				0.45f + static_cast<float>((x * 13u + z * 5u) % 20u) / 100.0f,
				0.50f + static_cast<float>((x * 7u + z * 11u) % 20u) / 100.0f,
				0.55f + static_cast<float>((x * 3u + z * 17u) % 20u) / 100.0f);

			ConfigureValidationPointLight(
				objects,
				objects.createObject("SparseClusterValidationLight").EntityId(),
				position,
				color,
				8.0f,
				3.5f);
		}
	}

	ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Spawned sparse cluster-bin validation fixture with " << (kSparseWidth * kSparseDepth) << " point lights.");
}

// SceneManager::LoadSceneDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::unique_ptr<SceneDocument> SceneManager::LoadSceneDocument(const std::filesystem::path& path, SceneFileType type)
{
	const auto jsonText = ReadJsonFile(path, type);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	document->name = parsed.value("name", path.stem().string());
	document->jsonContents = *jsonText;
	document->parsedJson = std::move(parsed);
	document->isPrefab = type == SceneFileType::Prefab;
	// Announce successful parse to make asset churn visible at debug verbosity.
	TERRAENGINE_LOG(LogLevel::Debug, "Loaded " << (type == SceneFileType::Prefab ? "prefab " : "scene ") << document->name << " from " << path);
	return document;
}

// SceneManager::ReadJsonFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<std::string> SceneManager::ReadJsonFile(const std::filesystem::path& path, SceneFileType type) const
{
	if (!ValidateExtension(path, type))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Rejected load for " << path << " because the extension does not match the expected scene or prefab format.");
		return std::nullopt;
	}

	// In export builds, attempt to read the JSON document from the TEPAK archive.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ContentMount::Instance().IsMounted())
	{
		const std::filesystem::path absolutePath = ResolveScenePath(path);
		const std::filesystem::path logicalPath = absolutePath.lexically_relative(sceneRoot);
		const std::string logicalPathString = logicalPath.generic_string();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ContentMount::Instance().Exists(logicalPathString))
		{
			const std::vector<uint8_t> bytes = ContentMount::Instance().ReadAll(logicalPathString);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!bytes.empty())
			{
				TERRAENGINE_LOG(LogLevel::Debug, "Reading scene file from mount: " << logicalPathString);
				return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			}
		}

		TERRAENGINE_LOG(LogLevel::Warning, "Scene file not found in mount: " << logicalPathString);
	}

	std::ifstream file(path, std::ios::binary);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open scene file from disk: " << path);
		return std::nullopt;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string jsonText = buffer.str();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (jsonText.empty())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Scene file is empty: " << path);
		return std::nullopt;
	}

	return jsonText;
}

// SceneManager::ValidateExtension is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool SceneManager::ValidateExtension(const std::filesystem::path& path, SceneFileType type) const
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (type == SceneFileType::Scene)
	{
		return extension == ".tescene";
	}

	return extension == ".teprefab";
}

// SceneManager::ResolveScenePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path SceneManager::ResolveScenePath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (sceneRoot / path).lexically_normal();
}

// SceneManager::NormalizeKey is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string SceneManager::NormalizeKey(const std::filesystem::path& path) const
{
	// Using generic_string keeps path separators consistent on Windows and POSIX
	// so cache keys are stable across different callers.
	return path.lexically_normal().generic_string();
}

// SceneManager::PreparePreviewAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// SceneManager::CreateGeneratedMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	binding.semantic = TextureSemantic::Base;
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
	material->shadowBehavior = DeriveMaterialShadowBehavior(header.alphaMode, header.emissiveFactor);
	material->storage.resize(sizeof(TextureBinding));
	std::memcpy(material->storage.data(), &binding, sizeof(TextureBinding));
	material->textures = std::span<const TextureBinding>(reinterpret_cast<const TextureBinding*>(material->storage.data()), header.textureCount);
	material->floats = std::span<const FloatParameter>();

	return resources.RegisterMaterial(name, std::move(material));
}

// SceneManager::SpawnPreviewObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SpawnPreviewObject(const TerraEngine::UUID& meshId, const TerraEngine::UUID& materialId, const TerraEngine::UUID& textureId)
{
	ObjectManager& objects = ObjectManager::instance();
	// Persist the preview object so input-driven camera controls can adjust it
	// every frame without re-querying the registry.
	previewObject = objects.createObject("GeneratedPreview").EntityId();

	// Initialize transform through the generic setter so locks are respected.
	Transform* t = objects.GetComponent<Transform>(*previewObject);
	if (t)
	{
		SetComponentProperty(*t, "Position", glm::vec3(0.0f, 0.0f, 0.0f));
		SetComponentProperty(*t, "Rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		SetComponentProperty(*t, "Scale", glm::vec3(1.0f, 1.0f, 1.0f));
		objects.MarkObjectTransformDirty(*previewObject);
	}

	// Attach a simple render binding component so downstream renderer work can
	// read which authored assets should be drawn together.
	if (MeshRenderComponent* renderComponent = objects.AddComponent<MeshRenderComponent>(*previewObject))
	{
		// Bind authored assets via the reflection-based setter to ensure thread-safe writes.
		SetComponentProperty(*renderComponent, "meshId", meshId);
		SetComponentProperty(*renderComponent, "materialId", materialId);
	}
}

// SceneManager::SpawnPreviewCamera is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SceneManager::SpawnPreviewCamera()
{
	ObjectManager& objects = ObjectManager::instance();
	previewCamera = objects.createObject("PreviewCamera").EntityId();

	// Match the legacy hardcoded camera by positioning it three units away
	// from the origin while keeping an identity rotation and uniform scale.
	Transform* transform = objects.GetComponent<Transform>(*previewCamera);
	if (transform)
	{
		SetComponentProperty(*transform, "Position", glm::vec3(0.0f, 0.0f, 3.0f));
		SetComponentProperty(*transform, "Rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		SetComponentProperty(*transform, "Scale", glm::vec3(1.0f, 1.0f, 1.0f));
		objects.MarkObjectTransformDirty(*previewCamera);
	}

	// Add a camera component with projection defaults that mirror the previous
	// hardcoded parameters so the preview looks identical.
	if (CameraComponent* camera = objects.AddComponent<CameraComponent>(*previewCamera))
	{
		SetComponentProperty(*camera, "verticalFovDegrees", 45.0f);
		SetComponentProperty(*camera, "aspectRatio", 16.0f / 9.0f);
		SetComponentProperty(*camera, "nearPlane", 0.1f);
		SetComponentProperty(*camera, "farPlane", 100.0f);
		SetComponentProperty(*camera, "up", glm::vec3(0.0f, 1.0f, 0.0f));
	}
}

// SceneManager::GetPreviewTransform is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Transform* SceneManager::GetPreviewTransform()
{
	if (!previewObject)
	{
		return nullptr;
	}

	return ObjectManager::instance().GetComponent<Transform>(*previewObject);
}
