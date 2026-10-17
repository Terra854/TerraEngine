// SceneManager.cpp — Scene Lifecycle Manager Implementation
//
// Implements the core logic for managing the engine's scene lifecycle and active document state.
// It handles the asynchronous loading of scene files and coordinates with the ECS and Resource systems to populate the world.
// This module ensures that scene transitions are handled safely and that all runtime systems are correctly synchronized.

#include "Scene/SceneManager.h"
#include "Scene/BulkScenePublication.h"
#include "Scene/WorldSpatialSystem.h"
#include "Scene/WorldStreamingSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
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

	SceneRenderSettings LoadSceneRenderSettingsFromDocument(const SceneDocument& document)
	{
		if (const auto sceneSettingsIt = document.parsedJson.find("sceneSettings"); sceneSettingsIt != document.parsedJson.end())
		{
			return Serializer::DeserializeSceneRenderSettings(*sceneSettingsIt);
		}

		return MakeDefaultSceneRenderSettings();
	}


	void RemoveValidationLights(ObjectManager& objects)
	{
		// Validation fixtures need deterministic light ownership, so remove any
		// prior light-only objects before spawning the new arrangement.
		const std::vector<Object> existingLights = objects.getObjectsWithComponents<LightComponent>();
		for (Object lightObject : existingLights)
		{
			objects.removeObject(lightObject);
		}
	}

	// Publish a bounded root pose through the canonical transform API. Scene
	// helper objects are created on the protected thread, so they may write live
	// state here; worker producers must use the deferred transaction queue.
	bool SetRootWorldPose(
		ObjectManager& objects,
		const Object& object,
		const glm::dvec3& offsetMetres,
		const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
		const glm::vec3& scale = glm::vec3(1.0f))
	{
		TerraEngine::Scene::WorldPose pose = objects.GetCanonicalWorldPose(object)
			.value_or(TerraEngine::Scene::WorldPose{});
		pose.position = {};
		pose.position.offsetMetres = offsetMetres;
		pose.rotation = rotation;
		pose.scale = scale;
		return objects.SetCanonicalWorldPose(object, pose)
			== TerraEngine::Scene::WorldCoordinateError::None;
	}


	void ConfigureValidationPointLight(ObjectManager& objects, Object lightObject, const glm::vec3& position, const glm::vec3& color, float intensity, float range)
	{
		if (!SetRootWorldPose(objects, lightObject, glm::dvec3(position)))
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Validation point-light canonical pose was rejected.");
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
	// Register SkyComponent before any scene data is deserialized so reflected
	// component JSON can create the component through the normal object path.
	SkyComponent::registerType();

	// Clear any stale tracking in case the manager is restarted during a hot
	// reload. At this stage nothing is loaded yet so the reset is cheap and
	// keeps stale cache entries from leaking across sessions.
	BeginLoadSession();
	
	ResourceManager::Instance().LoadDefaultResources();

	// If a startup scene was configured (e.g., via the export manifest), load 
	// and activate it immediately so the engine boots directly into gameplay.
	if (!m_StartupScene.empty())
	{
		const auto id = LoadScene(m_StartupScene);

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


void SceneManager::Update(float deltaTime)
{
	// No per-frame updates currently required for the static scene management
}

void SceneManager::Shutdown()
{
	std::lock_guard transition(loadSessionTransitionMutex);
	CloseAndDrainLoads();
	UnloadAll();
}

void SceneManager::PrepareShutdown()
{
	std::lock_guard transition(loadSessionTransitionMutex);
	CloseAndDrainLoads();
}

void SceneManager::BeginLoadSession()
{
	std::lock_guard transition(loadSessionTransitionMutex);
	CloseAndDrainLoads();
	UnloadAll();
	std::lock_guard gate(lifecycleMutex);
	loadAdmissionOpen = true;
}

void SceneManager::CloseAndDrainLoads()
{
	std::vector<std::shared_ptr<LoadRequest>> owned;
	{
		std::lock_guard gate(lifecycleMutex);
		loadAdmissionOpen = false;
		RetireCacheAuthorityLocked(false);
		owned.swap(outstandingLoads);
		drainingLoadCount = owned.size();
	}
	// Public completion also covers a submitter which reserved but has not yet
	// enqueued. It must settle that reservation before we inspect physical work.
	// No lock needed by submission, cleanup or publication is held while waiting.
	for(const auto& request : owned) {
		request->future.wait();
		if(request->physicalCompletion.valid()) request->physicalCompletion.wait();
	}
	{
		std::lock_guard gate(lifecycleMutex);
		loadDiagnostics.physicallySettled += owned.size();
		drainingLoadCount = 0;
	}
	// Callback captures are released outside manager locks after physical return.
}

void SceneManager::PruneSettledLoadsLocked()
{
	std::erase_if(outstandingLoads, [this](const auto& request) {
		const bool settled = request->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready
			&& (!request->physicalCompletion.valid()
				|| request->physicalCompletion.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
		if(settled) ++loadDiagnostics.physicallySettled;
		return settled;
	});
}

SceneLoadDiagnostics SceneManager::GetLoadDiagnostics()
{
	std::lock_guard gate(lifecycleMutex);
	PruneSettledLoadsLocked();
	std::shared_lock scenes(sceneCache.mutex);
	std::shared_lock prefabs(prefabCache.mutex);
	auto result = loadDiagnostics;
	result.admissionOpen = loadAdmissionOpen;
	result.sceneGeneration = sceneCache.generation;
	result.prefabGeneration = prefabCache.generation;
	result.pendingScenes = sceneCache.inFlight.size();
	result.pendingPrefabs = prefabCache.inFlight.size();
	result.ownedLoads = outstandingLoads.size() + drainingLoadCount;
	return result;
}

void SceneManager::RecordLoadTerminalLocked(const std::shared_ptr<LoadRequest>& request, SceneLoadDisposition disposition) noexcept
{
	if(request->disposition != SceneLoadDisposition::Pending) return;
	request->disposition = disposition;
	switch(disposition) {
	case SceneLoadDisposition::Published: ++loadDiagnostics.published; break;
	case SceneLoadDisposition::InvalidDocument: ++loadDiagnostics.invalid; break;
	case SceneLoadDisposition::Retired: ++loadDiagnostics.retired; break;
	case SceneLoadDisposition::ClosedAdmission: ++loadDiagnostics.closedAdmissions; break;
	case SceneLoadDisposition::AdmissionFailure: ++loadDiagnostics.admissionFailures; break;
	case SceneLoadDisposition::SubmissionFailure: ++loadDiagnostics.submissionFailures; break;
	case SceneLoadDisposition::LoaderException: ++loadDiagnostics.loaderExceptions; break;
	case SceneLoadDisposition::PublicationFailure: ++loadDiagnostics.publicationFailures; break;
	case SceneLoadDisposition::Pending: break;
	}
	if(request->source.requestId >= loadDiagnostics.latestRequestId) {
		loadDiagnostics.latestRequestId = request->source.requestId;
		loadDiagnostics.latestDisposition = disposition;
	}
}

void SceneManager::ReportLoadTerminal(const std::shared_ptr<LoadRequest>& request, SceneLoadDisposition disposition) noexcept
{
	try {
		std::lock_guard gate(lifecycleMutex);
		RecordLoadTerminalLocked(request, disposition);
	} catch(...) {
		// Diagnostic lock failure cannot replace the initiating future exception.
	}
}

void SceneManager::RetireCacheAuthorityLocked(bool clearDocuments)
{
	std::unique_lock scenes(sceneCache.mutex);
	std::unique_lock prefabs(prefabCache.mutex);
	++sceneCache.generation; ++prefabCache.generation;
	sceneCache.inFlight.clear(); prefabCache.inFlight.clear();
	if(clearDocuments) {
		sceneCache.pathToId.clear(); sceneCache.resources.clear();
		prefabCache.pathToId.clear(); prefabCache.resources.clear();
		activeSceneId = TerraEngine::EMPTY_UUID;
		++activeSceneAuthoredRevision;
		activeSceneDirty = false; activeSceneSettingsDirty = false;
		activeSceneRenderSettings = MakeDefaultSceneRenderSettings();
	}
}


std::string SceneManager::SystemName()
{
	return "SceneManager";
}


void SceneManager::SetSceneRoot(const std::filesystem::path& root)
{
	// Normalize immediately so repeated calls with different relative forms map
	// to the same cache keys.
	const auto capturedRoot = std::filesystem::absolute(root).lexically_normal();
	std::lock_guard gate(lifecycleMutex);
	if(sceneRoot != capturedRoot) {
		RetireCacheAuthorityLocked(true);
		sceneRoot = capturedRoot;
	}
	// Emit debug output so asset lookups can be traced back to the configured root.
	TERRAENGINE_LOG(LogLevel::Debug, "Scene root set to " << sceneRoot);
}


std::filesystem::path SceneManager::GetSceneRoot() const
{
	std::lock_guard gate(lifecycleMutex);
	return sceneRoot;
}


void SceneManager::SetStartupScene(const std::filesystem::path& path)
{
	m_StartupScene = path;
}


std::filesystem::path SceneManager::GetStartupScene() const
{
	return m_StartupScene;
}


TerraEngine::UUID SceneManager::LoadScene(const std::filesystem::path& path)
{
	return LoadScene(path, {});
}

TerraEngine::UUID SceneManager::LoadScene(const std::filesystem::path& path, SceneLoadOptions options)
{
	return RequestLoad(path, sceneCache, SceneFileType::Scene, false, false, std::move(options)).get();
}

TerraEngine::UUID SceneManager::ReloadScene(const std::filesystem::path& path)
{
	return ReloadScene(path, {});
}

TerraEngine::UUID SceneManager::ReloadScene(const std::filesystem::path& path, SceneLoadOptions options)
{
	// A new token revokes older source work without waiting for its I/O. Only a
	// successful current replacement can exchange the retained document.
	return RequestLoad(path, sceneCache, SceneFileType::Scene, true, false, std::move(options)).get();
}

TerraEngine::UUID SceneManager::LoadPreparedScene(
	const std::filesystem::path& path,
	nlohmann::json sceneDocument,
	TerraEngine::Scene::PreparedSceneDecode preparedScene)
{
	if (!preparedScene.accepted || preparedScene.cancelled || !sceneDocument.is_object())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Rejected an invalid prepared scene document for " << path);
		return TerraEngine::EMPTY_UUID;
	}
	// Prepared packets are accepted only for the current authored contract. No
	// migration can safely infer an unbounded coordinate from an old payload.
	if (!Serializer::ValidateUnboundedSceneDocument(sceneDocument)) return TerraEngine::EMPTY_UUID;
	preparedScene = Serializer::PrepareSceneDecode(sceneDocument);
	if (!preparedScene.accepted || preparedScene.cancelled) return TerraEngine::EMPTY_UUID;
	auto request = std::make_shared<LoadRequest>();
	{
		std::lock_guard gate(lifecycleMutex);
		request->source.requestId = nextLoadRequestId++;
		if(!loadAdmissionOpen) {
			RecordLoadTerminalLocked(request, SceneLoadDisposition::ClosedAdmission);
			return TerraEngine::EMPTY_UUID;
		}
		std::unique_lock lock(sceneCache.mutex);
		request->source.root = std::filesystem::absolute(sceneRoot).lexically_normal();
		request->source.absolutePath = path.is_absolute() ? path.lexically_normal() : (request->source.root / path).lexically_normal();
		request->source.generation = sceneCache.generation;
		request->key = NormalizeKey(request->source.absolutePath);
		const auto resident = sceneCache.pathToId.find(request->key);
		request->resourceId = resident == sceneCache.pathToId.end() ? TerraEngine::GenerateUUID() : resident->second;
		PruneSettledLoadsLocked();
		outstandingLoads.push_back(request);
		try { sceneCache.inFlight.insert_or_assign(request->key, request); }
		catch(...) { outstandingLoads.pop_back(); throw; }
		++loadDiagnostics.admitted;
		loadDiagnostics.latestRequestId = request->source.requestId;
		loadDiagnostics.latestDisposition = SceneLoadDisposition::Pending;
	}
	try {
		auto document = std::make_unique<SceneDocument>();
		document->sourcePath = request->source.absolutePath;
		document->name = request->source.absolutePath.stem().string();
		document->parsedJson = std::move(sceneDocument);
		document->preparedScene = std::move(preparedScene);
		document->isPrefab = false;
		const auto result = PublishLoad(request, sceneCache, std::move(document));
		CleanupLoad(request, sceneCache);
		request->completion.set_value(result);
		return result;
	} catch(...) {
		CleanupLoad(request, sceneCache);
		ReportLoadTerminal(request, SceneLoadDisposition::PublicationFailure);
		request->completion.set_exception(std::current_exception());
		throw;
	}
}


TerraEngine::UUID SceneManager::LoadPrefab(const std::filesystem::path& path)
{
	return LoadPrefab(path, {});
}

TerraEngine::UUID SceneManager::LoadPrefab(const std::filesystem::path& path, SceneLoadOptions options)
{
	const auto id = RequestLoad(path, prefabCache, SceneFileType::Prefab, false, false, std::move(options)).get();
	if(const auto* prefab = GetPrefab(id)) {
		TerraEngine::Scene::PrefabSource source;
		source.sourceId = id;
		source.sourcePath = prefab->sourcePath;
		source.sourceGraph = TerraEngine::Scene::BuildSceneGraphFromJson(prefab->parsedJson);
		TerraEngine::Scene::ValidatePrefabSource(source);
		for(const auto& diagnostic : source.sourceGraph.diagnostics) {
			TERRAENGINE_LOG(LogLevel::Warning, "Prefab composition diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
		}
	}
	return id;
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadSceneAsync(const std::filesystem::path& path)
{
	return LoadSceneAsync(path, {});
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadSceneAsync(const std::filesystem::path& path, SceneLoadOptions options)
{
	return RequestLoad(path, sceneCache, SceneFileType::Scene, false, true, std::move(options));
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadPrefabAsync(const std::filesystem::path& path)
{
	return LoadPrefabAsync(path, {});
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadPrefabAsync(const std::filesystem::path& path, SceneLoadOptions options)
{
	return RequestLoad(path, prefabCache, SceneFileType::Prefab, false, true, std::move(options));
}

std::shared_future<TerraEngine::UUID> SceneManager::RequestLoad(const std::filesystem::path& path,
	SceneCache<SceneDocument>& cache, SceneFileType type, bool reload, bool asynchronous, SceneLoadOptions options)
{
	auto request = std::make_shared<LoadRequest>();
	request->options = std::move(options);
	{
		std::lock_guard gate(lifecycleMutex);
		PruneSettledLoadsLocked();
		request->source.requestId = nextLoadRequestId++;
		if(!loadAdmissionOpen) {
			RecordLoadTerminalLocked(request, SceneLoadDisposition::ClosedAdmission);
			return MakeReadyFuture(TerraEngine::EMPTY_UUID);
		}
		std::shared_lock lock(cache.mutex);
		request->source.root = std::filesystem::absolute(sceneRoot).lexically_normal();
		request->source.absolutePath = path.is_absolute() ? path.lexically_normal()
			: (request->source.root / path).lexically_normal();
		request->source.mountedLogicalPath = request->source.absolutePath.lexically_relative(request->source.root).generic_string();
		request->source.generation = cache.generation;
		request->source.isPrefab = type == SceneFileType::Prefab;
		request->key = NormalizeKey(request->source.absolutePath);
	}
	// Admission injection is outside locks and before reservation. A throwing
	// caller cannot leave an unpublished source occupying the pending index.
	try {
		if(request->options.checkpoint) request->options.checkpoint(SceneLoadCheckpoint::Admission, request->source);
	} catch(...) {
		ReportLoadTerminal(request, SceneLoadDisposition::AdmissionFailure);
		throw;
	}
	{
		std::lock_guard gate(lifecycleMutex);
		std::unique_lock lock(cache.mutex);
		if(!loadAdmissionOpen || request->source.generation != cache.generation
			|| request->source.root != sceneRoot) {
			RecordLoadTerminalLocked(request, loadAdmissionOpen ? SceneLoadDisposition::Retired : SceneLoadDisposition::ClosedAdmission);
			return MakeReadyFuture(TerraEngine::EMPTY_UUID);
		}
		if(!reload) {
			if(const auto pending = cache.inFlight.find(request->key); pending != cache.inFlight.end()) return pending->second->future;
			if(const auto resident = cache.pathToId.find(request->key); resident != cache.pathToId.end()
				&& cache.resources.contains(resident->second)) return MakeReadyFuture(resident->second);
		}
		const auto resident = cache.pathToId.find(request->key);
		request->resourceId = resident == cache.pathToId.end() ? TerraEngine::GenerateUUID() : resident->second;
		outstandingLoads.push_back(request);
		try { cache.inFlight.insert_or_assign(request->key, request); }
		catch(...) { outstandingLoads.pop_back(); throw; }
		++loadDiagnostics.admitted;
		loadDiagnostics.latestRequestId = request->source.requestId;
		loadDiagnostics.latestDisposition = SceneLoadDisposition::Pending;
	}
	try {
		if(asynchronous) {
			if(request->options.checkpoint) request->options.checkpoint(SceneLoadCheckpoint::Submission, request->source);
			// A value-returning task has no pooled void promise. Failed submission
			// can destroy it without leaving an unfulfilled cached task to await.
			auto task = threadPool.createTask([this, request, &cache, type]() {
				ExecuteLoad(request, cache, type);
				return true;
			});
			auto physical = task->getFuture().share();
			bool stillCurrent = false;
			{
				std::lock_guard gate(lifecycleMutex);
				std::shared_lock lock(cache.mutex);
				const auto pending = cache.inFlight.find(request->key);
				stillCurrent = loadAdmissionOpen && request->source.generation == cache.generation
					&& pending != cache.inFlight.end() && pending->second == request;
				if(stillCurrent) {
					request->physicalCompletion = physical;
					try { threadPool.enqueue(task); }
					catch(...) {
						// Enqueue failed before admission: never await this unqueued
						// task's promise while a shutdown owns the reservation.
						request->physicalCompletion = {};
						throw;
					}
				}
			}
			if(!stillCurrent) {
				CleanupLoad(request, cache);
				ReportLoadTerminal(request, SceneLoadDisposition::Retired);
				request->completion.set_value(TerraEngine::EMPTY_UUID);
			}
		} else ExecuteLoad(request, cache, type);
	} catch(...) {
		const auto failure = std::current_exception();
		CleanupLoad(request, cache);
		ReportLoadTerminal(request, SceneLoadDisposition::SubmissionFailure);
		request->completion.set_exception(failure);
		throw;
	}
	return request->future;
}

void SceneManager::CleanupLoad(const std::shared_ptr<LoadRequest>& request, SceneCache<SceneDocument>& cache) noexcept
{
	try {
		std::lock_guard gate(lifecycleMutex);
		std::unique_lock lock(cache.mutex);
		const auto pending = cache.inFlight.find(request->key);
		if(pending != cache.inFlight.end() && pending->second == request
			&& request->source.generation == cache.generation) cache.inFlight.erase(pending);
	} catch(...) {
		// Cleanup cannot replace the loader's initiating exception. No retained
		// document or newer request is ever erased by a failed old request.
	}
}

TerraEngine::UUID SceneManager::PublishLoad(const std::shared_ptr<LoadRequest>& request,
	SceneCache<SceneDocument>& cache, std::unique_ptr<SceneDocument> resource)
{
	std::lock_guard gate(lifecycleMutex);
	std::unique_lock lock(cache.mutex);
	const auto pending = cache.inFlight.find(request->key);
	if(!loadAdmissionOpen || request->source.generation != cache.generation
		|| pending == cache.inFlight.end() || pending->second != request) {
		RecordLoadTerminalLocked(request, SceneLoadDisposition::Retired);
		return TerraEngine::EMPTY_UUID;
	}
	if(!resource) {
		RecordLoadTerminalLocked(request, SceneLoadDisposition::InvalidDocument);
		return TerraEngine::EMPTY_UUID;
	}
	// Allocate map ownership before exchanging a resident. If either insertion
	// throws, existing authored data remains untouched and cleanup permits retry.
	const auto [pathEntry, newPath] = cache.pathToId.try_emplace(request->key, request->resourceId);
	try {
		const auto [documentEntry, inserted] = cache.resources.try_emplace(request->resourceId);
		(void)inserted;
		documentEntry->second.swap(resource);
	} catch(...) {
		if(newPath) cache.pathToId.erase(pathEntry);
		throw;
	}
	cache.inFlight.erase(pending);
	RecordLoadTerminalLocked(request, SceneLoadDisposition::Published);
	return request->resourceId;
}

void SceneManager::ExecuteLoad(const std::shared_ptr<LoadRequest>& request, SceneCache<SceneDocument>& cache, SceneFileType type)
{
	bool publishing = false;
	try {
		if(request->options.checkpoint) request->options.checkpoint(SceneLoadCheckpoint::BeforeRead, request->source);
		auto resource = LoadSceneDocument(request->source, type);
		if(request->options.checkpoint) request->options.checkpoint(SceneLoadCheckpoint::AfterInterpretation, request->source);
		publishing = resource != nullptr;
		if(resource && request->options.checkpoint) request->options.checkpoint(SceneLoadCheckpoint::Publication, request->source);
		const auto result = PublishLoad(request, cache, std::move(resource));
		CleanupLoad(request, cache);
		request->completion.set_value(result);
	} catch(...) {
		const auto failure = std::current_exception();
		CleanupLoad(request, cache);
		ReportLoadTerminal(request, publishing ? SceneLoadDisposition::PublicationFailure : SceneLoadDisposition::LoaderException);
		request->completion.set_exception(failure);
	}
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
	SkyComponent::registerType();
	// JSON is decoded into value-only records before the live scene changes.
	// PublishBulkScenePacket is the sole protected visibility boundary: no
	// system callback can observe a half-created hierarchy during activation.
	TerraEngine::Scene::PreparedSceneDecode decodedSource;
	const TerraEngine::Scene::PreparedSceneDecode* prepared = nullptr;
	if (doc->preparedScene.has_value())
	{
		prepared = &*doc->preparedScene;
	}
	else
	{
		decodedSource = Serializer::PrepareSceneDecode(doc->parsedJson);
		prepared = &decodedSource;
	}
	const TerraEngine::Scene::BulkScenePublicationPacket packet = TerraEngine::Scene::BuildBulkScenePublicationPacket(
		*prepared,
		objManager.GetSceneGeneration(),
		objManager.GetSceneGeneration());
	const TerraEngine::Scene::BulkScenePublicationResult publication = TerraEngine::Scene::PublishBulkScenePacket(packet, objManager);
	if (!publication.published)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Active scene publication rejected for " << doc->sourcePath << ": " << publication.reason);
		return false;
	}

	auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objManager);
	for (const TerraEngine::Scene::SceneDiagnostic& diagnostic : graph.diagnostics)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Scene graph load diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
	}

	activeSceneId = id;
	++activeSceneAuthoredRevision;
	activeSceneRenderSettings = LoadSceneRenderSettingsFromDocument(*doc);
	// New scenes start clean to avoid showing unsaved changes indicators
	// immediately after a successful load or swap.
	activeSceneDirty = false;
	RenderSystem::Instance().MarkSkyStateDirty();
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

std::vector<TerraEngine::UUID> SceneManager::GetLoadedSceneIds() const
{
	std::shared_lock lock(sceneCache.mutex);
	std::vector<TerraEngine::UUID> ids;
	ids.reserve(sceneCache.resources.size());
	for (const auto& [id, document] : sceneCache.resources)
	{
		if (document)
		{
			ids.push_back(id);
		}
	}
	std::sort(ids.begin(), ids.end(), [](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
		return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
	});
	return ids;
}


bool SceneManager::IsActiveSceneDirty() const
{
	return activeSceneDirty || activeSceneSettingsDirty;
}


const SceneRenderSettings& SceneManager::GetActiveSceneRenderSettings() const
{
	return activeSceneRenderSettings;
}


void SceneManager::SetActiveSceneRenderSettings(const SceneRenderSettings& settings)
{
	activeSceneRenderSettings = SanitizeSceneRenderSettings(settings);
	activeSceneDirty = true;
	++activeSceneAuthoredRevision;
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

	ObjectManager& objects = ObjectManager::instance();
	std::optional<Object> object = objects.getObject(*skyObject);
	return object ? objects.GetComponent<SkyComponent>(*object) : nullptr;
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
		std::optional<Object> object = objManager.getObject(objectId);
		if (!object.has_value())
		{
			continue;
		}
		if (!objManager.isObjectActiveInHierarchy(*object))
		{
			continue;
		}

		const SkyComponent* skyComponent = objManager.GetComponent<SkyComponent>(*object);
		if (skyComponent == nullptr || !skyComponent->active)
		{
			continue;
		}

		return object->EntityId();
	}

	return std::nullopt;
}

// SceneManager::HasActiveSkyComponent reports whether sky authoring exists on
// an active scene object without exposing any redundant scene-level state.
bool SceneManager::HasActiveSkyComponent() const
{
	return GetActiveSkyObject().has_value();
}


void SceneManager::MarkActiveSceneSkyStateDirty()
{
	RenderSystem::Instance().MarkSkyStateDirty();
}


void SceneManager::ForceMarkActiveSceneDirty()
{
	activeSceneDirty = true;
	++activeSceneAuthoredRevision;
	MarkActiveSceneSkyStateDirty();
}


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

TerraEngine::Scene::BulkScenePublicationResult SceneManager::PublishPreparedRegionPacket(
	const TerraEngine::Scene::BulkScenePublicationPacket& packet,
	const std::function<bool()>& cancellationRequested)
{
	// Region decode happens off-thread, but exact UUID restoration is a live
	// ObjectManager write. Reuse the one protected bulk path so a cancellation
	// or malformed packet exposes neither partial objects nor partial identity.
	TerraEngine::Scene::BulkScenePublicationResult result = TerraEngine::Scene::PublishBulkScenePacket(
		packet, ObjectManager::instance(), cancellationRequested);
	if (result.published)
	{
		ForceMarkActiveSceneDirty();
	}
	return result;
}


bool SceneManager::SaveScene(std::optional<std::filesystem::path> path)
{
	return SaveScene(std::move(path), {});
}

bool SceneManager::SaveScene(std::optional<std::filesystem::path> path,
	const TerraEngine::Utilities::AtomicFileWriteOptions& options)
{
	using namespace TerraEngine::Utilities;
	lastSaveResult = {};
	try
	{
		// Save is a protected synchronous authoring operation: live ECS capture
		// stays on its owner thread. Only owned bytes enter filesystem work.
		struct SaveTicket
		{
			TerraEngine::UUID sceneId;
			std::shared_ptr<const std::uint8_t> documentIdentity;
			std::filesystem::path sourcePath;
			std::string name;
			SceneRenderSettings settings;
			std::uint64_t authored, scene, component, spatial, hierarchy, membership;
			struct ObjectRevision { TerraEngine::UUID id; std::uint64_t lifetime, metadata; };
			std::vector<ObjectRevision> objectRevisions;
		};
		ObjectManager& objects = ObjectManager::instance();
		SaveTicket ticket;
		{
			std::shared_lock lock(sceneCache.mutex);
			const auto it = sceneCache.resources.find(activeSceneId);
			if(it == sceneCache.resources.end() || !it->second)
			{
				lastSaveResult.reason = "No active scene to save.";
				return false;
			}
			const auto& document = *it->second;
			ticket = {activeSceneId, document.saveIdentity, document.sourcePath, document.name,
				activeSceneRenderSettings, activeSceneAuthoredRevision, objects.GetSceneGeneration(),
				objects.GetComponentMutationRevision(), objects.GetSpatialRevision(),
				objects.GetHierarchyRevision(), objects.GetRuntimeActiveMembershipRevision()};
		}
		const auto target = ResolveScenePath(path.value_or(ticket.sourcePath));
		// Acquiring the target before capture orders same-target saves and retains
		// that order through replacement and persisted cache publication.
		AtomicFileWriteTransaction transaction(target);
		auto checkpoint = [&](AtomicFileWriteStage stage)
		{
			lastSaveResult.stage = stage;
			if(options.checkpoint) lastSaveResult.reason = options.checkpoint(stage, transaction.Target());
			return lastSaveResult.reason.empty();
		};
		auto revisionsMatch = [&]()
		{
			return activeSceneId == ticket.sceneId && activeSceneAuthoredRevision == ticket.authored
				&& objects.GetSceneGeneration() == ticket.scene
				&& objects.GetComponentMutationRevision() == ticket.component
				&& objects.GetSpatialRevision() == ticket.spatial
				&& objects.GetHierarchyRevision() == ticket.hierarchy
				&& objects.GetRuntimeActiveMembershipRevision() == ticket.membership
				&& std::all_of(ticket.objectRevisions.begin(), ticket.objectRevisions.end(), [&](const auto& revision)
				{
					const auto object = objects.getObject(revision.id);
					return object && objects.getObjectLifetimeGeneration(*object) == revision.lifetime
						&& objects.getObjectMetadataGeneration(*object) == revision.metadata;
				});
		};
		if(!checkpoint(AtomicFileWriteStage::Capture)) return false;
		const auto spatial = TerraEngine::Scene::WorldSpatialSystem::Instance().CurrentSnapshot();
		const auto residency = TerraEngine::Scene::WorldStreamingSystem::Instance().CurrentResidency();
		nlohmann::json sceneJson;
		sceneJson["format"] = TerraEngine::Scene::kTerraSceneUnboundedFormat;
		sceneJson["sourceSceneGeneration"] = ticket.scene;
		sceneJson["spatialPublicationGeneration"] = spatial.spatialGeneration;
		sceneJson["residencyGeneration"] = residency.generation;
		sceneJson["regions"] = nlohmann::json::array();
		for(const auto& [key, region] : residency.residentRegions)
		{
			if(!region) continue;
			TerraEngine::Scene::WorldRegionArtifactManifest manifest;
			manifest.key = key; manifest.contentHash = region->contentHash; manifest.sourceGeneration = residency.generation;
			sceneJson["regions"].push_back(TerraEngine::Scene::SerializeWorldRegionArtifactManifest(manifest));
		}
		sceneJson["name"] = ticket.name;
		sceneJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(ticket.settings);
		sceneJson["objects"] = nlohmann::json::array();
		const auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
		// Metadata setters have per-object generations rather than one global
		// token. Capture them too so rename/tag/layer edits cannot be missed.
		ticket.objectRevisions.reserve(graph.objects.size());
		for(const auto& [id, record] : graph.objects)
		{
			const auto object = objects.getObject(id);
			if(!object) { lastSaveResult.reason = "Object disappeared during authored capture."; return false; }
			ticket.objectRevisions.push_back({id, objects.getObjectLifetimeGeneration(*object), objects.getObjectMetadataGeneration(*object)});
		}
		for(const auto& diagnostic : graph.diagnostics)
		{
			if(diagnostic.severity == TerraEngine::Scene::SceneDiagnosticSeverity::Error
				|| diagnostic.severity == TerraEngine::Scene::SceneDiagnosticSeverity::Fatal)
			{
				lastSaveResult.reason = TerraEngine::Scene::FormatSceneDiagnostic(diagnostic);
				return false;
			}
		}
		for(const auto& rootId : graph.rootObjects)
		{
			if(auto root = objects.getObject(rootId)) sceneJson["objects"].push_back(Serializer::SerializeObject(*root));
			else { lastSaveResult.reason = "Scene root disappeared during authored capture."; return false; }
		}
		if(!checkpoint(AtomicFileWriteStage::Serialize)) return false;
		if(!Serializer::ValidateUnboundedSceneDocument(sceneJson, &lastSaveResult.reason)) return false;
		// JSON's text serializer silently writes nonfinite floating values as
		// null. Reject them explicitly so an invalid authored field cannot become
		// a superficially successful save with changed component meaning. Use an
		// iterative walk so deep custom component JSON does not consume the stack.
		std::vector<const nlohmann::json*> pendingValues{&sceneJson};
		while(!pendingValues.empty())
		{
			const auto* value = pendingValues.back(); pendingValues.pop_back();
			if(value->is_number_float() && !std::isfinite(value->get<double>()))
			{ lastSaveResult.reason = "Authored scene contains a nonfinite numeric value."; return false; }
			if(value->is_structured())
			{
				for(const auto& child : *value) pendingValues.push_back(&child);
			}
		}
		// dump is intentionally before any staging open. It validates text and
		// can throw for invalid UTF-8 or allocations without touching old bytes.
		std::string completeBytes = sceneJson.dump(4);
		if(!revisionsMatch()
			|| TerraEngine::Scene::WorldSpatialSystem::Instance().CurrentSnapshot().spatialGeneration != spatial.spatialGeneration
			|| TerraEngine::Scene::WorldStreamingSystem::Instance().CurrentResidency().generation != residency.generation)
		{
			lastSaveResult.reason = "Scene or published spatial/residency revision changed during authored capture.";
			return false;
		}
		{
			std::shared_lock lock(sceneCache.mutex);
			const auto it = sceneCache.resources.find(ticket.sceneId);
			if(it == sceneCache.resources.end() || !it->second || it->second->saveIdentity != ticket.documentIdentity)
			{ lastSaveResult.reason = "Scene document owner changed during authored capture."; return false; }
		}
		// Cache JSON and text already own all required allocations. After disk
		// commit publication uses swaps and cannot turn success into false.
		auto sourceKey = std::filesystem::absolute(ticket.sourcePath).lexically_normal().wstring();
		auto targetKey = transaction.Target().wstring();
#if defined(_WIN32)
		// Windows source paths are case-insensitive even when a Save As caller
		// uses a different spelling of the original path.
		std::transform(sourceKey.begin(), sourceKey.end(), sourceKey.begin(), [](wchar_t c) { return std::towlower(c); });
		std::transform(targetKey.begin(), targetKey.end(), targetKey.begin(), [](wchar_t c) { return std::towlower(c); });
#endif
		const bool updatesSourceCache = sourceKey == targetKey;
		bool publicationUnchanged = true;
		AtomicFileWriteOptions checkedOptions;
		checkedOptions.checkpoint = [&](AtomicFileWriteStage stage, const std::filesystem::path& destination)
		{
			std::string rejection = options.checkpoint ? options.checkpoint(stage, destination) : std::string{};
			if(stage == AtomicFileWriteStage::Replace && rejection.empty())
			{
				// Snapshot copies can allocate, so do this last publication observation
				// before replacement. Postcommit adoption then needs only scalar tokens.
				publicationUnchanged = TerraEngine::Scene::WorldSpatialSystem::Instance().CurrentSnapshot().spatialGeneration == spatial.spatialGeneration
					&& TerraEngine::Scene::WorldStreamingSystem::Instance().CurrentResidency().generation == residency.generation;
			}
			return rejection;
		};
		lastSaveResult = transaction.Commit(completeBytes, checkedOptions);
		if(!lastSaveResult.committed) return false;
		{
			std::unique_lock lock(sceneCache.mutex);
			const auto it = sceneCache.resources.find(ticket.sceneId);
			const bool sameOwner = it != sceneCache.resources.end() && it->second
				&& it->second->saveIdentity == ticket.documentIdentity;
			if(sameOwner && updatesSourceCache)
			{
				it->second->parsedJson.swap(sceneJson);
				it->second->jsonContents.swap(completeBytes);
				it->second->preparedScene.reset();
			}
			if(sameOwner && publicationUnchanged && revisionsMatch())
			{
				activeSceneDirty = false;
				activeSceneSettingsDirty = false;
			}
		}
		return true;
	}
	catch(const std::exception& error)
	{
		// A rare postcommit synchronization error cannot undo an already saved
		// file. Keep dirty state and report truthful committed status to callers.
		if(lastSaveResult.committed) return true;
		lastSaveResult.reason = error.what();
	}
	catch(...)
	{
		if(lastSaveResult.committed) return true;
		lastSaveResult.reason = "Unknown authored scene save failure.";
	}
	return false;
}

std::vector<std::filesystem::path> SceneManager::ListSceneFiles(const std::filesystem::path& projectRoot) const
{
	std::vector<std::filesystem::path> scenes;
	const std::filesystem::path assetsRoot = projectRoot / "Assets";
	std::error_code errorCode;
	if (!std::filesystem::exists(assetsRoot, errorCode))
	{
		return scenes;
	}
	for (std::filesystem::recursive_directory_iterator iterator(assetsRoot, errorCode), end; iterator != end && !errorCode; iterator.increment(errorCode))
	{
		if (iterator->is_regular_file(errorCode) && iterator->path().extension() == ".tescene")
		{
			scenes.push_back(std::filesystem::relative(iterator->path(), projectRoot, errorCode).lexically_normal());
		}
	}
	std::sort(scenes.begin(), scenes.end());
	return scenes;
}

bool SceneManager::CreateSceneFile(const std::filesystem::path& path, std::string_view name, std::string* outError)
{
	if (path.extension() != ".tescene" || std::filesystem::exists(path))
	{
		if (outError) *outError = "Scene destination must be a new .tescene file.";
		return false;
	}
	std::error_code errorCode;
	std::filesystem::create_directories(path.parent_path(), errorCode);
	if (errorCode)
	{
		if (outError) *outError = "Scene destination directory could not be created.";
		return false;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output)
	{
		if (outError) *outError = "Scene destination could not be opened.";
		return false;
	}
	output << nlohmann::json{
		{"format", TerraEngine::Scene::kTerraSceneUnboundedFormat},
		{"name", name.empty() ? path.stem().string() : std::string(name)},
		{"sceneSettings", nlohmann::json::object()},
		{"objects", nlohmann::json::array()}
	}.dump(4);
	return output.good();
}

bool SceneManager::DuplicateSceneFile(const std::filesystem::path& source, const std::filesystem::path& destination, std::string* outError)
{
	if (source.extension() != ".tescene" || destination.extension() != ".tescene"
		|| !std::filesystem::is_regular_file(source) || std::filesystem::exists(destination))
	{
		if (outError) *outError = "Scene source must exist and destination must be a new .tescene file.";
		return false;
	}
	std::error_code errorCode;
	std::filesystem::create_directories(destination.parent_path(), errorCode);
	const bool copied = !errorCode && std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, errorCode);
	if (!copied || errorCode)
	{
		if (outError) *outError = "Scene file could not be duplicated.";
		return false;
	}
	return true;
}

bool SceneManager::RenameSceneFile(const std::filesystem::path& source, const std::filesystem::path& destination, std::string* outError)
{
	if (!DuplicateSceneFile(source, destination, outError))
	{
		return false;
	}
	std::error_code errorCode;
	std::filesystem::remove(source, errorCode);
	if (errorCode)
	{
		// Roll back the copied destination so a failed rename never leaves two
		// identities that look like a successful protected commit.
		std::error_code rollbackError;
		std::filesystem::remove(destination, rollbackError);
		if (outError) *outError = "Scene source could not be removed; rename was rolled back.";
		return false;
	}
	return true;
}


void SceneManager::UnloadAll()
{
	std::lock_guard gate(lifecycleMutex);
	// Clear visible authority, not physical completion ownership. New requests
	// may use this fresh cache generation while old readers settle independently.
	RetireCacheAuthorityLocked(true);
	// Remove the generated preview entity so repeated initializes do not leave
	// stray objects in the registry when the manager is reused.

	if (previewObject)
	{
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(*previewObject))
		{
			objects.removeObject(*object);
		}
		previewObject.reset();
	}

	if (previewCamera)
	{
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(*previewCamera))
		{
			objects.removeObject(*object);
		}
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


void SceneManager::NullifyAssetReferences(const TerraEngine::UUID& assetId)
{
	// MainEngine mirrors this mesh/material asset scan into EcsQueryTaskGraph metadata so scheduler diagnostics can
	// reason about the component access before this protected scene-maintenance path mutates existing components.
	ObjectManager::instance().forEachObjectWithComponents<MeshRenderComponent>(
		[&](Object object, MeshRenderComponent& meshComp)
		{
			bool changed = false;
			const TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "meshId");
			const TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "materialId");


			if (meshId == assetId)
			{
				SetComponentProperty(meshComp, "meshId", TerraEngine::EMPTY_UUID);
				changed = true;
			}
			

			if (materialId == assetId)
			{
				SetComponentProperty(meshComp, "materialId", TerraEngine::EMPTY_UUID);
				changed = true;
			}


			if (changed)
			{
				activeSceneDirty = true;
				++activeSceneAuthoredRevision;
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

	for (std::uint32_t z = 0; z < safeGridDepth; ++z)
	{
		for (std::uint32_t x = 0; x < safeGridWidth; ++x)
		{
			Object lightObject = objects.createObject("StressPointLight");

			const float worldX = (static_cast<float>(x) - centerX) * safeSpacing;
			const float worldZ = (static_cast<float>(z) - centerZ) * safeSpacing;
			const float worldY = 1.5f + std::sin(static_cast<float>(x + z) * 0.37f) * 0.5f;
			if (!SetRootWorldPose(objects, lightObject, glm::dvec3(worldX, worldY, worldZ)))
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Stress point-light canonical pose was rejected.");
			}

			if (LightComponent* light = objects.AddComponent<LightComponent>(lightObject))
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
			objects.createObject("DenseClusterValidationLight"),
			position,
			color,
			24.0f,
			8.0f);
	}

	ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Spawned dense cluster-bin validation fixture with " << kDenseLightCount << " point lights.");
}


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
				objects.createObject("SparseClusterValidationLight"),
				position,
				color,
				8.0f,
				3.5f);
		}
	}

	ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Spawned sparse cluster-bin validation fixture with " << (kSparseWidth * kSparseDepth) << " point lights.");
}


std::unique_ptr<SceneDocument> SceneManager::LoadSceneDocument(const SceneLoadSourceSnapshot& source, SceneFileType type)
{
	const auto& path = source.absolutePath;
	const auto jsonText = ReadJsonFile(source, type);

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
	std::string formatError;
	const bool validFormat = type == SceneFileType::Prefab
		? Serializer::ValidateUnboundedPrefabDocument(parsed, &formatError)
		: Serializer::ValidateUnboundedSceneDocument(parsed, &formatError);
	if (!validFormat)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Rejected legacy or malformed authored document " << path << ": " << formatError);
		return nullptr;
	}
	if(parsed.contains("name") && !parsed["name"].is_string()) {
		TERRAENGINE_LOG(LogLevel::Error, "Rejected malformed scene name field in " << path);
		return nullptr;
	}
	// Check metadata before cache publication or protected activation can change
	// live objects. Conversion errors here are malformed authored input, while
	// allocation and external loader exceptions retain their exception channel.
	if(const auto settings = parsed.find("sceneSettings"); settings != parsed.end()) {
		if(!settings->is_object()) return nullptr;
		try { (void)Serializer::DeserializeSceneRenderSettings(*settings); }
		catch(const nlohmann::json::type_error& error) {
			TERRAENGINE_LOG(LogLevel::Error, "Rejected malformed scene settings in " << path << ": " << error.what());
			return nullptr;
		}
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


std::optional<std::string> SceneManager::ReadJsonFile(const SceneLoadSourceSnapshot& source, SceneFileType type) const
{
	const auto& path = source.absolutePath;
	if (!ValidateExtension(path, type))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Rejected load for " << path << " because the extension does not match the expected scene or prefab format.");
		return std::nullopt;
	}

	// In export builds, attempt to read the JSON document from the TEPAK archive.

	if (ContentMount::Instance().IsMounted())
	{
		// The mounted spelling belongs to admission's root, never to a mutable
		// root which may have changed while this worker waited for execution.
		const std::string& logicalPathString = source.mountedLogicalPath;


		if (ContentMount::Instance().Exists(logicalPathString))
		{
			const std::vector<uint8_t> bytes = ContentMount::Instance().ReadAll(logicalPathString);

			if (!bytes.empty())
			{
				TERRAENGINE_LOG(LogLevel::Debug, "Reading scene file from mount: " << logicalPathString);
				return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			}
		}

		TERRAENGINE_LOG(LogLevel::Warning, "Scene file not found in mount: " << logicalPathString);
	}

	std::ifstream file(path, std::ios::binary);

	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open scene file from disk: " << path);
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
	std::lock_guard gate(lifecycleMutex);
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


void SceneManager::SpawnPreviewObject(const TerraEngine::UUID& meshId, const TerraEngine::UUID& materialId, const TerraEngine::UUID& textureId)
{
	ObjectManager& objects = ObjectManager::instance();
	// Persist the preview object so input-driven camera controls can adjust it
	// every frame without re-querying the registry.
	Object previewObjectHandle = objects.createObject("GeneratedPreview");
	previewObject = previewObjectHandle.EntityId();

	// The default canonical pose is already identity, but publish it explicitly
	// so this preview path exercises the same strict contract as authored scenes.
	if (!SetRootWorldPose(objects, previewObjectHandle, glm::dvec3(0.0)))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Preview object canonical pose was rejected.");
	}

	// Attach a simple render binding component so downstream renderer work can
	// read which authored assets should be drawn together.
	if (MeshRenderComponent* renderComponent = objects.AddComponent<MeshRenderComponent>(previewObjectHandle))
	{
		// Bind authored assets via the reflection-based setter to ensure thread-safe writes.
		SetComponentProperty(*renderComponent, "meshId", meshId);
		SetComponentProperty(*renderComponent, "materialId", materialId);
	}
}


void SceneManager::SpawnPreviewCamera()
{
	ObjectManager& objects = ObjectManager::instance();
	Object previewCameraHandle = objects.createObject("PreviewCamera");
	previewCamera = previewCameraHandle.EntityId();

	// Position the preview camera three local metres from the exact origin while
	// keeping identity rotation and scale in its canonical WorldPose.
	if (!SetRootWorldPose(objects, previewCameraHandle, glm::dvec3(0.0, 0.0, 3.0)))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Preview camera canonical pose was rejected.");
	}

	// Add a camera component with projection defaults that mirror the previous
	// hardcoded parameters so the preview looks identical.
	if (CameraComponent* camera = objects.AddComponent<CameraComponent>(previewCameraHandle))
	{
		SetComponentProperty(*camera, "verticalFovDegrees", 45.0f);
		SetComponentProperty(*camera, "aspectRatio", 16.0f / 9.0f);
		SetComponentProperty(*camera, "nearPlane", 0.1f);
		SetComponentProperty(*camera, "farPlane", 100.0f);
		SetComponentProperty(*camera, "up", glm::vec3(0.0f, 1.0f, 0.0f));
	}
}


Transform* SceneManager::GetPreviewTransform()
{
	if (!previewObject)
	{
		return nullptr;
	}

	ObjectManager& objects = ObjectManager::instance();
	std::optional<Object> object = objects.getObject(*previewObject);
	return object ? objects.GetComponent<Transform>(*object) : nullptr;
}
