// MainEditorSubsystem.cpp — Core editor workspace implementation
//
// Implements the primary editor workspace logic, including the main menu bar, toolbars, and global hotkey handling. It manages the lifecycle of all native editor windows and provides the necessary hooks for scene state capture and restoration during play mode.

#include "MainEditorSubsystem.h"
#include "Editor/AssetCompilers/ModelPackageImporter.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "EditorSystem.h"
#include "ScriptCompilationSubsystem.h"
#include "UndoRedoSystem.h"
#include "../Profiling/EditorProfilerService.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Scheduling/EcsQueryTaskGraph.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/MeshRenderComponent.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneGraphRuntime.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Scripting/ScriptGlue.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/Utilities/PlatformUtils.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <unordered_map>
#include <nlohmann/json.hpp>

// Include Window Headers
#include "../Windows/HierarchyWindow.h"
#include "../Windows/InspectorWindow.h"
#include "../Windows/ProjectBrowserWindow.h"
#include "../Windows/GameViewWindow.h"
#include "../Windows/SceneViewWindow.h"
#include "../Windows/BuildProfilesWindow.h"
#include "../Windows/ProfilerWindow.h"
#include "../Windows/SchedulerDiagnosticsWindow.h"
#include "../Windows/ResourceResidencyWindow.h"
#include "../Windows/UIBuilderWindow.h"
#include "../Windows/ProjectSettingsWindow.h"
#include "../Windows/EditorMcpBridgeWindow.h"

namespace
{
	MainEditorSubsystem* g_ActiveMainEditorSubsystem = nullptr;

	// ResolveEditorRuntimeRoot returns the directory that contains the running
	// editor executable. Renderer-owned shaders and managed binaries are staged
	// beside that executable, so this must not depend on the process working
	// directory; --project launches often use the repository root as cwd.
	std::filesystem::path ResolveEditorRuntimeRoot()
	{
		const std::filesystem::path executableDirectory = TerraEngine::PlatformUtils::GetExecutableDirectory();
		if (!executableDirectory.empty())
		{
			return executableDirectory;
		}

		TERRAENGINE_LOG(LogLevel::Warning, "Could not resolve editor executable directory; falling back to current working directory for engine root.");
		return std::filesystem::current_path();
	}

	// StopPlayModeFromApplicationQuit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void StopPlayModeFromApplicationQuit()
	{
		if (g_ActiveMainEditorSubsystem != nullptr)
		{
			g_ActiveMainEditorSubsystem->StopPlayMode();
		}
	}

	std::optional<AssetType> AssetTypeFromImporter(std::string_view importerName)
	{
		if (importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(importerName))
		{
			return AssetType::Mesh;
		}
		if (importerName == "TextureImporter")
		{
			return AssetType::Texture;
		}
		if (importerName == "MaterialImporter")
		{
			return AssetType::Material;
		}
		if (importerName == "ShaderImporter")
		{
			return AssetType::ShaderCollection;
		}
		return std::nullopt;
	}

	std::optional<std::pair<TerraEngine::UUID, AssetType>> ResolveEditorAssetPath(const std::filesystem::path& assetPath)
	{
		const std::filesystem::path normalizedPath = std::filesystem::absolute(assetPath).lexically_normal();
		for (const auto& [sourcePath, metadata] : AssetDatabase::Instance().GetAssets())
		{
			if (std::filesystem::absolute(sourcePath).lexically_normal() != normalizedPath)
			{
				continue;
			}

			if (auto assetType = AssetTypeFromImporter(metadata.importerName))
			{
				return std::make_pair(metadata.guid, *assetType);
			}
			return std::nullopt;
		}
		return std::nullopt;
	}

	void PublishMeshRenderAssetNeeds(Object object, TerraEngine::Resource::StreamingNeedType needType, std::string_view reason)
	{
		MeshRenderComponent* meshRenderer = ObjectManager::instance().GetComponent<MeshRenderComponent>(object);
		if (meshRenderer == nullptr)
		{
			return;
		}

		ResourceManager& resources = ResourceManager::Instance();
		resources.MarkStreamingNeed(meshRenderer->meshId, AssetType::Mesh, needType, 0u, reason, 80);
		resources.MarkStreamingNeed(meshRenderer->materialId, AssetType::Material, needType, 0u, reason, 80);
	}

	void CollectPrefabAssetNeedsFromObjectJson(const nlohmann::json& objectJson, std::vector<std::pair<TerraEngine::UUID, AssetType>>& assets)
	{
		if (const auto componentsIt = objectJson.find("components"); componentsIt != objectJson.end() && componentsIt->is_object())
		{
			if (const auto meshRendererIt = componentsIt->find("MeshRenderComponent"); meshRendererIt != componentsIt->end() && meshRendererIt->is_object())
			{
				if (const auto meshIdIt = meshRendererIt->find("meshId"); meshIdIt != meshRendererIt->end() && meshIdIt->is_string())
				{
					if (const auto parsed = TerraEngine::ParseUUID(meshIdIt->get<std::string>()))
					{
						assets.emplace_back(*parsed, AssetType::Mesh);
					}
				}
				if (const auto materialIdIt = meshRendererIt->find("materialId"); materialIdIt != meshRendererIt->end() && materialIdIt->is_string())
				{
					if (const auto parsed = TerraEngine::ParseUUID(materialIdIt->get<std::string>()))
					{
						assets.emplace_back(*parsed, AssetType::Material);
					}
				}
			}
		}

		if (const auto childrenIt = objectJson.find("children"); childrenIt != objectJson.end() && childrenIt->is_array())
		{
			for (const nlohmann::json& childJson : *childrenIt)
			{
				CollectPrefabAssetNeedsFromObjectJson(childJson, assets);
			}
		}
	}

	void PublishPrefabAssetPreviewNeeds(const std::filesystem::path& prefabPath)
	{
		std::ifstream prefabInput(prefabPath, std::ios::binary);
		if (!prefabInput)
		{
			return;
		}

		nlohmann::json prefabJson;
		try
		{
			prefabInput >> prefabJson;
		}
		catch (const nlohmann::json::parse_error&)
		{
			return;
		}

		std::vector<std::pair<TerraEngine::UUID, AssetType>> assets;
		if (const auto objectsIt = prefabJson.find("objects"); objectsIt != prefabJson.end() && objectsIt->is_array())
		{
			for (const nlohmann::json& objectJson : *objectsIt)
			{
				CollectPrefabAssetNeedsFromObjectJson(objectJson, assets);
			}
		}

		ResourceManager& resources = ResourceManager::Instance();
		for (const auto& [assetId, assetType] : assets)
		{
			resources.MarkStreamingNeed(assetId, assetType, TerraEngine::Resource::StreamingNeedType::AssetPreview, 0u, "selected prefab asset preview", 70);
		}
	}

	std::optional<TerraEngine::UUID> ReadSerializedObjectUuid(const nlohmann::json& objectJson)
	{
		if (const auto uuidIt = objectJson.find("uuid"); uuidIt != objectJson.end() && uuidIt->is_string())
		{
			return TerraEngine::ParseUUID(uuidIt->get<std::string>());
		}
		return std::nullopt;
	}

	void CollectPrefabInstanceUuidRemap(nlohmann::json& objectJson, std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& uuidRemap)
	{
		if (std::optional<TerraEngine::UUID> sourceUuid = ReadSerializedObjectUuid(objectJson))
		{
			const TerraEngine::UUID instanceUuid = TerraEngine::GenerateUUID();
			uuidRemap[*sourceUuid] = instanceUuid;
			objectJson["uuid"] = TerraEngine::ToHexString(instanceUuid);
		}

		if (auto childrenIt = objectJson.find("children"); childrenIt != objectJson.end() && childrenIt->is_array())
		{
			for (nlohmann::json& childJson : *childrenIt)
			{
				CollectPrefabInstanceUuidRemap(childJson, uuidRemap);
			}
		}
	}

	void RewritePrefabInstanceUuidReferences(nlohmann::json& value, const std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& uuidRemap)
	{
		if (value.is_string())
		{
			if (const std::optional<TerraEngine::UUID> parsedUuid = TerraEngine::ParseUUID(value.get<std::string>()))
			{
				if (const auto replacement = uuidRemap.find(*parsedUuid); replacement != uuidRemap.end())
				{
					// Object references inside the prefab should target the new
					// instance IDs, while asset GUIDs stay unchanged because they
					// are not present in this source-object remap.
					value = TerraEngine::ToHexString(replacement->second);
				}
			}
			return;
		}

		if (value.is_array())
		{
			for (nlohmann::json& childValue : value)
			{
				RewritePrefabInstanceUuidReferences(childValue, uuidRemap);
			}
			return;
		}

		if (value.is_object())
		{
			for (auto& [name, childValue] : value.items())
			{
				(void)name;
				RewritePrefabInstanceUuidReferences(childValue, uuidRemap);
			}
		}
	}

	std::filesystem::path ResolvePrefabAssetPath(const AssetMetadata& metadata)
	{
		std::filesystem::path prefabPath = metadata.sourcePath;
		if (!prefabPath.is_absolute())
		{
			prefabPath = AssetDatabase::Instance().GetAssetRoot() / prefabPath;
		}
		return std::filesystem::absolute(prefabPath).lexically_normal();
	}

	void AttachPrefabLink(entt::entity entity, const TerraEngine::UUID& prefabSourceId, const std::optional<TerraEngine::UUID>& sourceObjectId, const std::filesystem::path& prefabPath)
	{
		std::optional<Object> object = ObjectManager::instance().getObject(entity);
		if (!object)
		{
			return;
		}

		PrefabLinkComponent* existingLink = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
		PrefabLinkComponent* ensuredLink = existingLink != nullptr ? existingLink : ObjectManager::instance().AddComponent<PrefabLinkComponent>(*object);
		if (ensuredLink == nullptr)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to attach prefab link metadata to dropped prefab instance.");
			return;
		}

		PrefabLinkComponent& link = *ensuredLink;
		link.prefabSourceId = prefabSourceId;
		link.sourceObjectId = sourceObjectId.value_or(ObjectManager::instance().getUUID(*object));
		link.prefabName = prefabPath.stem().string();

		std::error_code relativeError;
		const std::filesystem::path relativePrefabPath = std::filesystem::relative(prefabPath, SceneManager::Instance().GetSceneRoot(), relativeError);
		link.prefabPath = relativeError ? prefabPath.generic_string() : relativePrefabPath.generic_string();
		link.overriddenProperties.clear();
	}
}

// MainEditorSubsystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Initialize(EditorContext& context) {
	ctx = &context;

	// Set engine root to the executable's directory so default shaders can be found
	// even if the project asset root is changed later.
	ResourceManager::Instance().SetEngineRoot(ResolveEditorRuntimeRoot());
	
	// Initialize Windows
	context.editorProfiler = std::make_shared<EditorProfilerService>();
	context.editorProfiler->Initialize(context);
	m_Windows.push_back(std::make_shared<HierarchyWindow>(this));
	m_Windows.push_back(std::make_shared<InspectorWindow>(this));
	m_Windows.push_back(std::make_shared<ProjectBrowserWindow>(this));
	m_Windows.push_back(std::make_shared<GameViewWindow>(this));
	m_Windows.push_back(std::make_shared<SceneViewWindow>(this));
	m_Windows.push_back(std::make_shared<ProjectSettingsWindow>(this));
	m_Windows.push_back(std::make_shared<BuildProfilesWindow>(this));
	m_Windows.push_back(std::make_shared<ProfilerWindow>(this));
	m_Windows.push_back(std::make_shared<SchedulerDiagnosticsWindow>(this));
	m_Windows.push_back(std::make_shared<ResourceResidencyWindow>(this));
	m_Windows.push_back(std::make_shared<UIBuilderWindow>(this));
	m_Windows.push_back(std::make_shared<EditorMcpBridgeWindow>(this));

	UndoRedoSystem::Instance().Initialize(context);
	ScriptCompilationSubsystem::Instance().Initialize(context);

	ObjectManager::instance().SetOnModifiedCallback([this]() {
		if (m_SuppressObjectModifiedCallback)
		{
			return;
		}
		if (ScriptingSystem::Instance().IsRunning())
		{
			// Play mode runs against a temporary scene snapshot. Runtime script
			// changes, spawned objects, and deferred mutation queue flushes should
			// be discarded on Stop instead of marking the authored scene file dirty.
			return;
		}
		if (m_IsPrefabEditingMode)
		{
			m_PrefabEditingDirty = true;
			return;
		}
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	});

	ScriptingSystem::Instance().SetRunning(false);
	ClearPlayModeSceneState();
	g_ActiveMainEditorSubsystem = this;
	SetApplicationEditorStopPlayModeCallback(&StopPlayModeFromApplicationQuit);
}

// MainEditorSubsystem::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OnProjectOpened() {
	if (!ctx || !ctx->projectManager->HasActiveProject()) return;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto browser = GetWindow<ProjectBrowserWindow>()) {
		browser->Reset();
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto buildProfiles = GetWindow<BuildProfilesWindow>()) {
		buildProfiles->OnProjectOpened();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx->editorProfiler) {
		ctx->editorProfiler->OnProjectOpened();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto uiBuilder = GetWindow<UIBuilderWindow>()) {
		// Reset the builder when projects change so hidden internal asset paths
		// never leak across workspaces and the next open starts from local state.
		uiBuilder->ResetForProjectOpen();
	}

	auto projectRoot = ctx->projectManager->GetProjectRoot();
	std::filesystem::path assetRoot = projectRoot / "Assets";
	std::filesystem::path sceneRoot = assetRoot / "Scenes";
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(sceneRoot)) {
		std::filesystem::create_directories(sceneRoot);
	}

	// Set Scene Root to Assets folder so we can load scenes from anywhere inside Assets
	SceneManager::Instance().SetSceneRoot(assetRoot.string());
	ResourceManager::Instance().SetAssetRoot(assetRoot);
	RenderSystem::Instance().LoadSettings();
	
	// Check for last opened scene
	std::filesystem::path lastScenePath = projectRoot / "Library" / "LastScene.txt";
	bool loaded = false;
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(lastScenePath)) {
		std::ifstream in(lastScenePath);
		std::string relPath;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::getline(in, relPath) && !relPath.empty()) {
			// Verify it still exists
			if (std::filesystem::exists(assetRoot / relPath)) {
				OpenScene(relPath);
				if (sceneLoaded) loaded = true;
			}
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!loaded) {
		bool foundAny = false;
		if (std::filesystem::exists(assetRoot)) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.is_regular_file() && entry.path().extension() == ".tescene") {
					std::error_code ec;
					auto rel = std::filesystem::relative(entry.path(), assetRoot, ec);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!ec) {
						OpenScene(rel);
						foundAny = true;
						break;
					}
				}
			}
		}
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!foundAny) {
			// Maybe create a new one? For now just do nothing or log.
		}
	}

	ScriptCompilationSubsystem::Instance().OnProjectOpened();
}

// MainEditorSubsystem::CapturePlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::CapturePlayModeSceneState()
{
	m_HasPlayModeSceneSnapshot = false;
	m_PlayModeSceneSnapshotJson.clear();
	m_PlayModeSelectedEntityUuid.reset();
	m_PlayModeSceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();

	nlohmann::json snapshotJson;
	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	snapshotJson["name"] = activeScene != nullptr ? activeScene->name : "ActiveScene";
	snapshotJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(SceneManager::Instance().GetActiveSceneRenderSettings());
	snapshotJson["objects"] = nlohmann::json::array();

	// Capture the full live hierarchy and reflected component payloads before
	// play starts so Stop can rebuild the exact authored scene state without
	// touching the on-disk scene file.
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		snapshotJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
	auto summary = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
	TERRAENGINE_LOG(LogLevel::Info, "Captured editor play-mode scene summary with " << summary.objects.size() << " objects.");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedEntity.has_value())
	{
		if (std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			m_PlayModeSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedObject);
		}
	}

	m_PlayModeSceneSnapshotJson = snapshotJson.dump();
	m_HasPlayModeSceneSnapshot = true;
}

// MainEditorSubsystem::RestorePlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RestorePlayModeSceneState()
{
	if (!m_HasPlayModeSceneSnapshot)
	{
		return;
	}

	nlohmann::json snapshotJson;
	try
	{
		snapshotJson = nlohmann::json::parse(m_PlayModeSceneSnapshotJson);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to parse the pre-play scene snapshot at byte " << parseError.byte << ": " << parseError.what());
		ClearPlayModeSceneState();
		SetSelectedEntity(std::nullopt);
		return;
	}

	ObjectManager::instance().clear();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (snapshotJson.contains("sceneSettings"))
	{
		SceneManager::Instance().SetActiveSceneRenderSettings(Serializer::DeserializeSceneRenderSettings(snapshotJson["sceneSettings"]));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (snapshotJson.contains("objects") && snapshotJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : snapshotJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}

	ScriptingSystem::Instance().RebuildRestoredSceneBindings();
	auto restoredGraph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
	auto restoredSummary = TerraEngine::Scene::BuildSceneRuntimeSummary(restoredGraph);
	TERRAENGINE_LOG(LogLevel::Info, "Restored editor play-mode scene summary with " << restoredSummary.objects.size() << " objects.");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_PlayModeSceneWasDirty)
	{
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
	else
	{
		SceneManager::Instance().ClearActiveSceneDirty();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_PlayModeSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PlayModeSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else
	{
		SetSelectedEntity(std::nullopt);
	}

	ClearPlayModeSceneState();
}

// MainEditorSubsystem::ClearPlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::ClearPlayModeSceneState()
{
	m_HasPlayModeSceneSnapshot = false;
	m_PlayModeSceneWasDirty = false;
	m_PlayModeSceneSnapshotJson.clear();
	m_PlayModeSelectedEntityUuid.reset();
}

void MainEditorSubsystem::CapturePrefabReturnSceneState()
{
	m_HasPrefabReturnSceneSnapshot = false;
	m_PrefabReturnSceneSnapshotJson.clear();
	m_PrefabReturnSelectedEntityUuid.reset();
	m_PrefabReturnSceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();
	m_PrefabReturnGameViewCameras.clear();

	const auto sceneCameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
	for (Object cameraObject : sceneCameras)
	{
		const Transform* transform = ObjectManager::instance().GetComponent<Transform>(cameraObject);
		const CameraComponent* camera = ObjectManager::instance().GetComponent<CameraComponent>(cameraObject);
		if (transform == nullptr || camera == nullptr)
		{
			continue;
		}

		// Copy only the render-facing camera fields needed to rebuild the same
		// Game View camera after prefab mode swaps in an isolated ObjectManager.
		PrefabReturnGameViewCamera cachedCamera;
		cachedCamera.position = transform->position;
		cachedCamera.rotation = transform->rotation;
		cachedCamera.up = camera->up;
		cachedCamera.verticalFovDegrees = camera->verticalFovDegrees;
		cachedCamera.nearPlane = camera->nearPlane;
		cachedCamera.farPlane = camera->farPlane;
		m_PrefabReturnGameViewCameras.push_back(cachedCamera);
	}

	nlohmann::json snapshotJson;
	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	snapshotJson["name"] = activeScene != nullptr ? activeScene->name : "ActiveScene";
	snapshotJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(SceneManager::Instance().GetActiveSceneRenderSettings());
	snapshotJson["objects"] = nlohmann::json::array();

	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		snapshotJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	if (selectedEntity.has_value())
	{
		if (std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			m_PrefabReturnSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedObject);
		}
	}

	m_PrefabReturnSceneSnapshotJson = snapshotJson.dump();
	m_HasPrefabReturnSceneSnapshot = true;
}

void MainEditorSubsystem::RestorePrefabReturnSceneState()
{
	if (!m_HasPrefabReturnSceneSnapshot)
	{
		return;
	}

	if (!LoadPrefabReturnSceneSnapshotIntoObjectManager(true))
	{
		return;
	}

	m_HasPrefabReturnSceneSnapshot = false;
	m_PrefabReturnSceneSnapshotJson.clear();
	m_PrefabReturnSelectedEntityUuid.reset();
	m_PrefabReturnGameViewCameras.clear();
}

void MainEditorSubsystem::CaptureCurrentPrefabEditingState()
{
	if (!m_IsPrefabEditingMode || !m_PrefabObjectsLoadedForEditor)
	{
		return;
	}

	nlohmann::json prefabJson;
	prefabJson["name"] = m_PrefabEditingPath.empty() ? "Prefab" : m_PrefabEditingPath.stem().string();
	prefabJson["objects"] = nlohmann::json::array();
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		prefabJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	if (selectedEntity.has_value())
	{
		if (std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			m_PrefabEditingSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedObject);
		}
	}
	else
	{
		m_PrefabEditingSelectedEntityUuid.reset();
	}

	m_PrefabEditingObjectSnapshotJson = prefabJson.dump();
}

bool MainEditorSubsystem::EnsurePrefabObjectsLoadedForEditor()
{
	if (!m_IsPrefabEditingMode || m_PrefabObjectsLoadedForEditor)
	{
		return false;
	}

	return LoadPrefabEditingSnapshotIntoObjectManager();
}

void MainEditorSubsystem::RestoreSceneObjectsForRuntimeDuringPrefabEdit()
{
	if (!m_IsPrefabEditingMode || !m_PrefabObjectsLoadedForEditor)
	{
		return;
	}

	if (!m_PrefabEditOpenedFromSceneObject)
	{
		return;
	}

	CaptureCurrentPrefabEditingState();
	if (LoadPrefabReturnSceneSnapshotIntoObjectManager(false))
	{
		m_PrefabObjectsLoadedForEditor = false;
		SetSelectedEntity(std::nullopt);
	}
}

bool MainEditorSubsystem::LoadPrefabEditingSnapshotIntoObjectManager()
{
	if (m_PrefabEditingObjectSnapshotJson.empty())
	{
		return false;
	}

	nlohmann::json snapshotJson;
	try
	{
		snapshotJson = nlohmann::json::parse(m_PrefabEditingObjectSnapshotJson);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to parse prefab editing snapshot at byte " << parseError.byte << ": " << parseError.what());
		SetSelectedEntity(std::nullopt);
		return false;
	}

	m_SuppressObjectModifiedCallback = true;
	ObjectManager::instance().clear();
	if (snapshotJson.contains("objects") && snapshotJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : snapshotJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}
	m_SuppressObjectModifiedCallback = false;

	m_PrefabObjectsLoadedForEditor = true;
	if (m_PrefabEditingSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PrefabEditingSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else
	{
		SetSelectedEntity(std::nullopt);
	}

	return true;
}

bool MainEditorSubsystem::LoadPrefabReturnSceneSnapshotIntoObjectManager(bool restoreSelection)
{
	if (!m_HasPrefabReturnSceneSnapshot)
	{
		return false;
	}

	nlohmann::json snapshotJson;
	try
	{
		snapshotJson = nlohmann::json::parse(m_PrefabReturnSceneSnapshotJson);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to parse prefab return scene snapshot at byte " << parseError.byte << ": " << parseError.what());
		SetSelectedEntity(std::nullopt);
		return false;
	}

	m_SuppressObjectModifiedCallback = true;
	ObjectManager::instance().clear();
	if (snapshotJson.contains("sceneSettings"))
	{
		SceneManager::Instance().SetActiveSceneRenderSettings(Serializer::DeserializeSceneRenderSettings(snapshotJson["sceneSettings"]));
	}
	if (snapshotJson.contains("objects") && snapshotJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : snapshotJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}
	m_SuppressObjectModifiedCallback = false;

	if (m_PrefabReturnSceneWasDirty)
	{
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
	else
	{
		SceneManager::Instance().ClearActiveSceneDirty();
	}

	if (restoreSelection && m_PrefabReturnSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PrefabReturnSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else if (restoreSelection)
	{
		SetSelectedEntity(std::nullopt);
	}

	return true;
}

bool MainEditorSubsystem::TryApplyPrefabModeGameViewCameras(float aspect) const
{
	if (!m_IsPrefabEditingMode || m_PrefabReturnGameViewCameras.empty())
	{
		return false;
	}

	if (aspect <= 0.0001f)
	{
		aspect = 1.0f;
	}

	for (const PrefabReturnGameViewCamera& camera : m_PrefabReturnGameViewCameras)
	{
		const glm::vec3 forward = camera.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 up = camera.rotation * camera.up;
		if (glm::length(up) < 0.0001f)
		{
			up = glm::vec3(0.0f, 1.0f, 0.0f);
		}

		// Feed the renderer through its normal Game View camera entry point, so
		// prefab editing only changes camera ownership, not render behavior.
		const glm::mat4 view = glm::lookAt(camera.position, camera.position + forward, glm::normalize(up));
		// Keep prefab-return Game View cameras in Vulkan's 0..1 depth space so
		// renderer depth tests and Hi-Z occlusion interpret the same projection.
		glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDegrees), aspect, camera.nearPlane, camera.farPlane);
		proj[1][1] *= -1.0f;
		RenderSystem::Instance().SetGameViewCamera(view, proj, camera.position);
	}

	return true;
}

bool MainEditorSubsystem::LoadPrefabIntoIsolatedObjectManager(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open prefab for editing: " << path);
		return false;
	}

	nlohmann::json prefabJson;
	try
	{
		input >> prefabJson;
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Prefab edit file failed JSON parse at byte " << parseError.byte << ": " << parseError.what() << " Path: " << path);
		return false;
	}

	m_SuppressObjectModifiedCallback = true;
	ObjectManager::instance().clear();
	if (prefabJson.contains("objects") && prefabJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : prefabJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}
	m_SuppressObjectModifiedCallback = false;

	SetSelectedEntity(std::nullopt);
	return true;
}

// MainEditorSubsystem::OpenScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OpenScene(const std::filesystem::path& path) {
	if (m_IsPrefabEditingMode)
	{
		ClosePrefabMode();
	}
	TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(path);
		if (sceneId != TerraEngine::EMPTY_UUID) {
			SceneManager::Instance().SetActiveScene(sceneId);
			sceneLoaded = true;
			m_CurrentScenePath = path;
			ClearPlayModeSceneState();
			UndoRedoSystem::Instance().Clear();

			auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
			auto summary = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
			TERRAENGINE_LOG(LogLevel::Info, "Editor scene graph loaded with " << summary.objects.size() << " objects and " << summary.diagnostics.size() << " diagnostics.");
			for (const auto& diagnostic : summary.diagnostics)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Editor scene graph diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
			}
	
			// Save to LastScene.txt
	
		if (ctx && ctx->projectManager->HasActiveProject()) {
			auto projectRoot = ctx->projectManager->GetProjectRoot();
			std::filesystem::path libDir = projectRoot / "Library";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(libDir)) {
				std::filesystem::create_directories(libDir);
			}
			
			std::ofstream out(libDir / "LastScene.txt");
			// path is typically relative to SceneRoot.
			// If path is absolute, we should make it relative if possible,
			// but SceneManager usually takes relative paths.
			// Assuming path is relative to SceneRoot (Assets/Scenes)
			out << path.generic_string();
		}
	}
}

bool MainEditorSubsystem::OpenPrefabAsset(const std::filesystem::path& path)
{
	return OpenPrefabAssetInternal(path, false);
}

bool MainEditorSubsystem::InstantiatePrefabAsset(const TerraEngine::UUID& prefabGuid, std::optional<entt::entity> parent)
{
	const AssetMetadata* metadata = AssetDatabase::Instance().FindAssetByGuid(prefabGuid);
	if (metadata == nullptr || metadata->importerName != "PrefabImporter")
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Dropped prefab payload does not reference a registered prefab asset.");
		return false;
	}

	const std::filesystem::path prefabPath = ResolvePrefabAssetPath(*metadata);
	std::ifstream prefabInput(prefabPath, std::ios::binary);
	if (!prefabInput)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to instantiate prefab because the prefab file could not be opened: " << prefabPath);
		return false;
	}

	nlohmann::json prefabJson;
	try
	{
		prefabInput >> prefabJson;
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Dropped prefab failed JSON parse at byte " << parseError.byte << ": " << parseError.what() << " Path: " << prefabPath);
		return false;
	}

	if (!prefabJson.contains("objects") || !prefabJson["objects"].is_array() || prefabJson["objects"].empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Dropped prefab has no root objects: " << prefabPath);
		return false;
	}

	std::optional<TerraEngine::UUID> parentUuid;
	if (parent.has_value())
	{
		if (std::optional<Object> parentObject = ObjectManager::instance().getObject(*parent))
		{
			parentUuid = ObjectManager::instance().getUUID(*parentObject);
		}
		else
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Dropped prefab parent no longer exists in the hierarchy.");
			return false;
		}
	}

	const TerraEngine::UUID prefabSourceId = SceneManager::Instance().LoadPrefab(prefabPath);
	const TerraEngine::UUID linkSourceId = prefabSourceId == TerraEngine::EMPTY_UUID ? prefabGuid : prefabSourceId;
	std::vector<entt::entity> instantiatedRoots;
	std::vector<std::unique_ptr<ICommand>> undoCommands;

	for (nlohmann::json objectJson : prefabJson["objects"])
	{
		const std::optional<TerraEngine::UUID> sourceRootObjectId = ReadSerializedObjectUuid(objectJson);
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID> uuidRemap;
		CollectPrefabInstanceUuidRemap(objectJson, uuidRemap);
		RewritePrefabInstanceUuidReferences(objectJson, uuidRemap);

		// Deserialize on the protected editor path so ObjectManager, component
		// reflection, hierarchy, and render invalidation all see one normal
		// authored scene mutation.
		const entt::entity rootEntity = Serializer::DeserializeObjectEntity(objectJson, parent);
		AttachPrefabLink(rootEntity, linkSourceId, sourceRootObjectId, prefabPath);
		instantiatedRoots.push_back(rootEntity);
		undoCommands.push_back(std::make_unique<CreateObjectCommand>(rootEntity));
		if (parentUuid.has_value())
		{
			undoCommands.push_back(std::make_unique<SetParentCommand>(rootEntity, std::nullopt, parentUuid));
		}
	}

	if (instantiatedRoots.empty())
	{
		return false;
	}

	UndoRedoSystem::Instance().PushTransactionGroup("Instantiate Prefab", std::move(undoCommands));
	SetSelectedEntity(instantiatedRoots.front());
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Instantiated prefab into hierarchy: " << prefabPath);
	return true;
}

bool MainEditorSubsystem::OpenPrefabAssetInternal(const std::filesystem::path& path, bool openedFromSceneObject)
{
	const std::filesystem::path absolutePath = std::filesystem::absolute(path).lexically_normal();
	if (absolutePath.extension() != ".teprefab")
	{
		return false;
	}

	if (m_IsPrefabEditingMode && m_PrefabEditingDirty)
	{
		SaveOpenPrefab();
	}

	if (!m_IsPrefabEditingMode)
	{
		CapturePrefabReturnSceneState();
	}

	if (!LoadPrefabIntoIsolatedObjectManager(absolutePath))
	{
		if (!m_IsPrefabEditingMode)
		{
			RestorePrefabReturnSceneState();
		}
		return false;
	}

	m_IsPrefabEditingMode = true;
	m_PrefabEditingDirty = false;
	m_PrefabEditingPath = absolutePath;
	m_PrefabObjectsLoadedForEditor = true;
	m_PrefabEditOpenedFromSceneObject = openedFromSceneObject;
	CaptureCurrentPrefabEditingState();

	TERRAENGINE_LOG(LogLevel::Info, "Opened prefab in isolated edit mode: " << absolutePath);
	return true;
}

bool MainEditorSubsystem::OpenPrefabFromObject(entt::entity object)
{
	std::optional<Object> objectHandle = ObjectManager::instance().getObject(object);
	PrefabLinkComponent* link = objectHandle ? ObjectManager::instance().GetComponent<PrefabLinkComponent>(*objectHandle) : nullptr;
	if (link == nullptr || link->prefabPath.empty())
	{
		return false;
	}

	std::filesystem::path prefabPath = link->prefabPath;
	if (!prefabPath.is_absolute())
	{
		prefabPath = SceneManager::Instance().GetSceneRoot() / prefabPath;
	}
	return OpenPrefabAssetInternal(prefabPath, true);
}

bool MainEditorSubsystem::ResetPrefabInstanceToDefaults(entt::entity object)
{
	ObjectManager& objManager = ObjectManager::instance();
	std::optional<Object> objectHandle = objManager.getObject(object);
	PrefabLinkComponent* link = objectHandle ? objManager.GetComponent<PrefabLinkComponent>(*objectHandle) : nullptr;
	if (link == nullptr || link->prefabPath.empty())
	{
		return false;
	}

	std::filesystem::path prefabPath = link->prefabPath;
	if (!prefabPath.is_absolute())
	{
		prefabPath = SceneManager::Instance().GetSceneRoot() / prefabPath;
	}

	std::ifstream prefabInput(prefabPath, std::ios::binary);
	if (!prefabInput)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot reset prefab instance because the source file could not be opened: " << prefabPath);
		return false;
	}

	nlohmann::json prefabJson;
	try
	{
		prefabInput >> prefabJson;
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot reset prefab instance because the source file failed JSON parse at byte " << parseError.byte << ": " << parseError.what());
		return false;
	}

	if (!prefabJson.contains("objects") || !prefabJson["objects"].is_array() || prefabJson["objects"].empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Cannot reset prefab instance because the prefab has no root object: " << prefabPath);
		return false;
	}

	const TerraEngine::UUID prefabSourceId = link->prefabSourceId;
	const std::string prefabName = link->prefabName;
	std::optional<Object> parentHandle = objectHandle ? objManager.getParent(*objectHandle) : std::nullopt;
	std::optional<entt::entity> parentObject;
	if (parentHandle)
	{
		parentObject = parentHandle->EntityId();
	}

	objManager.removeObject(*objectHandle);
	entt::entity restoredObject = Serializer::DeserializeObjectEntity(prefabJson["objects"].front(), parentObject);
	std::optional<Object> restoredHandle = objManager.getObject(restoredObject);
	if (!restoredHandle)
	{
		return false;
	}
	PrefabLinkComponent* existingRestoredLink = objManager.GetComponent<PrefabLinkComponent>(*restoredHandle);
	PrefabLinkComponent* addedRestoredLink = existingRestoredLink != nullptr ? existingRestoredLink : objManager.AddComponent<PrefabLinkComponent>(*restoredHandle);
	if (addedRestoredLink == nullptr)
	{
		return false;
	}
	PrefabLinkComponent& restoredLink = *addedRestoredLink;
	restoredLink.prefabSourceId = prefabSourceId;
	restoredLink.sourceObjectId = objManager.getUUID(*restoredHandle);
	restoredLink.prefabName = prefabName.empty() ? prefabPath.stem().string() : prefabName;

	std::error_code relativeError;
	const std::filesystem::path relativePrefabPath = std::filesystem::relative(prefabPath, SceneManager::Instance().GetSceneRoot(), relativeError);
	restoredLink.prefabPath = relativeError ? prefabPath.generic_string() : relativePrefabPath.generic_string();
	restoredLink.overriddenProperties.clear();

	SetSelectedEntity(restoredObject);
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Reset prefab instance to defaults from " << prefabPath);
	return true;
}

bool MainEditorSubsystem::SaveOpenPrefab()
{
	if (!m_IsPrefabEditingMode || m_PrefabEditingPath.empty())
	{
		return false;
	}

	if (!m_PrefabObjectsLoadedForEditor && !m_PrefabEditingObjectSnapshotJson.empty())
	{
		nlohmann::json snapshotJson;
		try
		{
			snapshotJson = nlohmann::json::parse(m_PrefabEditingObjectSnapshotJson);
		}
		catch (const nlohmann::json::parse_error& parseError)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to save prefab because the cached edit snapshot failed JSON parse at byte " << parseError.byte << ": " << parseError.what());
			return false;
		}

		std::ofstream snapshotOutput(m_PrefabEditingPath, std::ios::binary);
		if (!snapshotOutput)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to save prefab: " << m_PrefabEditingPath);
			return false;
		}
		snapshotOutput << snapshotJson.dump(4);
		m_PrefabEditingDirty = false;
		TERRAENGINE_LOG(LogLevel::Info, "Saved prefab: " << m_PrefabEditingPath);
		return true;
	}

	nlohmann::json prefabJson;
	prefabJson["name"] = m_PrefabEditingPath.stem().string();
	prefabJson["objects"] = nlohmann::json::array();
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		prefabJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	std::ofstream output(m_PrefabEditingPath, std::ios::binary);
	if (!output)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to save prefab: " << m_PrefabEditingPath);
		return false;
	}

	output << prefabJson.dump(4);
	m_PrefabEditingDirty = false;
	TERRAENGINE_LOG(LogLevel::Info, "Saved prefab: " << m_PrefabEditingPath);
	return true;
}

void MainEditorSubsystem::ClosePrefabMode()
{
	if (!m_IsPrefabEditingMode)
	{
		return;
	}

	if (m_PrefabEditingDirty)
	{
		SaveOpenPrefab();
	}

	m_IsPrefabEditingMode = false;
	m_PrefabEditingDirty = false;
	m_PrefabEditingPath.clear();
	m_PrefabObjectsLoadedForEditor = false;
	m_PrefabEditOpenedFromSceneObject = false;
	m_PrefabEditingObjectSnapshotJson.clear();
	m_PrefabEditingSelectedEntityUuid.reset();
	RestorePrefabReturnSceneState();

	TERRAENGINE_LOG(LogLevel::Info, "Closed prefab edit mode and restored the active scene.");
}

// MainEditorSubsystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Shutdown() {
	ClearPlayModeSceneState();
	m_PrefabReturnGameViewCameras.clear();
	if (g_ActiveMainEditorSubsystem == this)
	{
		g_ActiveMainEditorSubsystem = nullptr;
	}
	SetApplicationEditorStopPlayModeCallback(nullptr);
	ScriptCompilationSubsystem::Instance().Shutdown();
	UndoRedoSystem::Instance().Shutdown();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr && ctx->editorProfiler) {
		ctx->editorProfiler->Shutdown();
		ctx->editorProfiler.reset();
	}
	ctx = nullptr;
	m_Windows.clear();
}

// MainEditorSubsystem::StopPlayMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::StopPlayMode()
{
	if (!ScriptingSystem::Instance().IsRunning())
	{
		return;
	}

	// Route every stop request through one helper so toolbar clicks and
	// Application.Quit() both restore the same cached pre-play scene snapshot.
	ScriptingSystem::Instance().SetRunning(false);
	ScriptingSystem::Instance().SetPaused(false);
	RestorePlayModeSceneState();
}

// MainEditorSubsystem::OnBackendReady is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OnBackendReady() {
	if (!ctx || !ctx->engine) return;
	
	RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderSystem) return;

	RHIContext* rhi = renderSystem->GetContext();
	VulkanContext* vulkan = dynamic_cast<VulkanContext*>(rhi);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!vulkan) return;

	VulkanContext::EditorHandles handles;
	vulkan->GetEditorHandles(handles);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handles.sceneImageView && handles.sceneSampler) {
		sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
			(VkSampler)handles.sceneSampler,
			(VkImageView)handles.sceneImageView,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
		cachedSceneImageView = handles.sceneImageView;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handles.gameImageView && handles.gameSampler) {
		gameTextureID = (void*)ImGui_ImplVulkan_AddTexture(
			(VkSampler)handles.gameSampler,
			(VkImageView)handles.gameImageView,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
		cachedGameImageView = handles.gameImageView;
	}
}

// MainEditorSubsystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Update(float deltaTime) {
	// Advance the scripting coordinator after asset watcher events have been
	// processed so queued C# rebuilds can start and completed async builds can
	// clear their in-flight state during the same editor frame.
	ScriptCompilationSubsystem::Instance().Update(deltaTime);

	// Ensure UI system updates even when the game is not playing so we get live previews.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ScriptingSystem::Instance().IsRunning())
	{
		UISystem::Instance().Update(deltaTime);
	}

	PublishEditorStreamingNeeds();

	// A blocking script compile pauses editor-side interactions until the build
	// either reloads the new assembly or restores the previous working one.
	if (ScriptCompilationSubsystem::Instance().ShouldBlockEditorInteraction()) {
		return;
	}

	// Logic update

	if (ctx != nullptr)
	{
		std::optional<uint32_t> watchedObjectId;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (selectedEntity.has_value())
		{
			watchedObjectId = static_cast<uint32_t>(*selectedEntity);
		}

		ctx->scripting.hasWatchedObject = watchedObjectId.has_value();
		ctx->scripting.watchedObjectId = watchedObjectId.value_or(0);
		ScriptingSystem::Instance().UpdateInspectorWatch(watchedObjectId, ctx->scripting.selectionVersion);
	}

	// Process editor-wide hotkeys.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
		if (ImGui::IsKeyPressed(ImGuiKey_S)) {
			if (m_IsPrefabEditingMode) {
				SaveOpenPrefab();
			} else {
				SceneManager::Instance().SaveScene();
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
			UndoRedoSystem::Instance().Undo();
		}
		// Traditional Windows redo shortcut (Ctrl+Y).
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
			UndoRedoSystem::Instance().Redo();
		}
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_B)) {
			if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) {
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
				}
			} else {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
					win->StartExportAndRun();
				}
			}
		}
	}
}

void MainEditorSubsystem::PublishEditorStreamingNeeds()
{
	// This publisher runs once per editor frame and only emits compact need
	// records. ResourceManager owns the synchronized mutation and expiry, so the
	// inspector, project browser, and prefab workflow do not need to duplicate
	// residency bookkeeping in their ImGui draw code.
	if (selectedEntity.has_value())
	{
		if (std::optional<Object> object = ObjectManager::instance().getObject(*selectedEntity))
		{
			PublishMeshRenderAssetNeeds(*object, TerraEngine::Resource::StreamingNeedType::EditorSelection, "editor selected object");
			PublishMeshRenderAssetNeeds(*object, TerraEngine::Resource::StreamingNeedType::InspectorPreview, "inspector selected object preview");
		}
	}

	if (selectedAsset.has_value())
	{
		const std::filesystem::path selectedPath = std::filesystem::absolute(*selectedAsset).lexically_normal();
		if (auto resolvedAsset = ResolveEditorAssetPath(selectedPath))
		{
			ResourceManager::Instance().MarkStreamingNeed(
				resolvedAsset->first,
				resolvedAsset->second,
				TerraEngine::Resource::StreamingNeedType::AssetPreview,
				0u,
				"project browser selected asset preview",
				70);
		}
		else if (selectedPath.extension() == ".teprefab")
		{
			PublishPrefabAssetPreviewNeeds(selectedPath);
		}
	}

	if (m_IsPrefabEditingMode)
	{
		ObjectManager::instance().forEachObjectWithComponents<MeshRenderComponent>([](Object object, MeshRenderComponent&) {
			PublishMeshRenderAssetNeeds(object, TerraEngine::Resource::StreamingNeedType::PrefabEditing, "open prefab editing session");
		});
	}
}

// MainEditorSubsystem::SetOnProjectClosedCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetOnProjectClosedCallback(std::function<void()> callback) {
	onProjectClosed = callback;
}

// MainEditorSubsystem::SetSelectedEntity is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetSelectedEntity(std::optional<entt::entity> entity)
{
	selectedEntity = entity;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (entity)
	{
		selectedAsset = std::nullopt;
		selectedPackageChildAsset = std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = entity.has_value();
		ctx->scripting.watchedObjectId = entity.has_value() ? static_cast<std::uint32_t>(*entity) : 0;
	}
}

// MainEditorSubsystem::SetSelectedAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetSelectedAsset(std::optional<std::filesystem::path> asset)
{
	selectedAsset = asset;
	selectedPackageChildAsset = std::nullopt;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (asset)
	{
		selectedEntity = std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
	}
}

bool MainEditorSubsystem::RebuildAssetFromInspector(const std::filesystem::path& path, std::string* outError) {
	const std::filesystem::path absolutePath = std::filesystem::absolute(path);
	if (!std::filesystem::exists(absolutePath)) {
		if (outError != nullptr) {
			*outError = "Source asset does not exist.";
		}
		return false;
	}

	// Reuse the asset database import boundary so Inspector-triggered rebuilds
	// follow the same atomic compile, sidecar, ResourceManager, and renderer
	// reload behavior as Project Browser and file-watcher imports.
	AssetDatabase::Instance().ImportAssetWithProgress(absolutePath, true);
	return true;
}

// MainEditorSubsystem::SetSelectedPackageChildAsset records a read-only generated child selection from a model package.
void MainEditorSubsystem::SetSelectedPackageChildAsset(std::optional<SelectedPackageChildAsset> asset)
{
	selectedPackageChildAsset = asset;
	selectedAsset = asset ? std::optional<std::filesystem::path>(asset->packagePath) : std::nullopt;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (asset)
	{
		selectedEntity = std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
	}
}

// MainEditorSubsystem::IsInspectorValueDragging is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MainEditorSubsystem::IsInspectorValueDragging() const {
	auto inspector = const_cast<MainEditorSubsystem*>(this)->GetWindow<InspectorWindow>();
	if (inspector) return inspector->IsInspectorValueDragging();
	return false;
}

// MainEditorSubsystem::TryGetGameViewRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MainEditorSubsystem::TryGetGameViewRenderStats(RenderStatsSnapshot& outStats) const {
	if (!ctx || !ctx->engine) {
		return false;
	}

	RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderSystem) {
		return false;
	}

	outStats = renderSystem->GetLatestRenderStats(RenderViewType::Game);
	return outStats.valid;
}

void MainEditorSubsystem::OpenProjectSettings(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId)
{
	if (auto projectSettings = GetWindow<ProjectSettingsWindow>())
	{
		projectSettings->OpenToCategory(categoryId);
	}
}

std::vector<std::string> MainEditorSubsystem::GetWindowMenuTitlesForTests() const
{
	std::vector<std::string> titles;
	for (const auto& window : m_Windows)
	{
		if (window && TerraEngine::Editor::ProjectSettings::ShouldShowWindowInWindowMenu(window->GetTitle()))
		{
			titles.push_back(window->GetTitle());
		}
	}
	return titles;
}

bool MainEditorSubsystem::ShouldShowWindowInWindowMenuForTests(std::string_view title)
{
	return TerraEngine::Editor::ProjectSettings::ShouldShowWindowInWindowMenu(title);
}

// MainEditorSubsystem::DrawUI is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawUI() {
	EnsurePrefabObjectsLoadedForEditor();

	// Check for resize/recreation of offscreen resources
	if (ctx && ctx->engine) {
		RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
		if (renderSystem) {
			// Asset-style prefab editing is an isolated authoring mode, not a
			// gameplay preview. Suppressing the next Game View render avoids a
			// hidden/blank offscreen pass while Scene View remains responsive.
			renderSystem->SetGameViewSuppressed(IsAssetPrefabEditingMode());
			VulkanContext* vulkan = dynamic_cast<VulkanContext*>(renderSystem->GetContext());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vulkan) {
				VulkanContext::EditorHandles handles;
				vulkan->GetEditorHandles(handles);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (handles.sceneImageView != cachedSceneImageView) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (handles.sceneImageView && handles.sceneSampler) {
						sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
							(VkSampler)handles.sceneSampler,
							(VkImageView)handles.sceneImageView,
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
						);
						cachedSceneImageView = handles.sceneImageView;
					}
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (handles.gameImageView != cachedGameImageView) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (handles.gameImageView && handles.gameSampler) {
						gameTextureID = (void*)ImGui_ImplVulkan_AddTexture(
							(VkSampler)handles.gameSampler,
							(VkImageView)handles.gameImageView,
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
						);
						cachedGameImageView = handles.gameImageView;
					}
				}
			}
		}
	}

	// Setup Dockspace
	static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
	
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("TerraEngine DockSpace", nullptr, window_flags);
	ImGui::PopStyleVar();
	ImGui::PopStyleVar(2);

	DrawMainMenuBar();

	// Toolbar
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		auto& colors = ImGui::GetStyle().Colors;
		const auto& buttonHovered = colors[ImGuiCol_ButtonHovered];
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.5f));
		const auto& buttonActive = colors[ImGuiCol_ButtonActive];
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.5f));

		float size = ImGui::GetWindowHeight() - 4.0f; // Constrain to not take too much space? No, this is the main window.
		// We want a small toolbar strip.
		// Since we are inside the DockSpace window, but before DockSpace node, we are just drawing at the top.
		
		// Let's use a fixed height for the toolbar area to ensure DockSpace doesn't overlap weirdly or just let flow handle it.
		// But we want to center the buttons.
		
		float availWidth = ImGui::GetContentRegionAvail().x;
		// Buttons: Play/Stop, Pause, Step
		float buttonSize = 20.0f; // Approx height
		const char* playLabel = ScriptingSystem::Instance().IsRunning() ? "Stop" : "Play";
		const char* pauseLabel = "Pause";
		const char* stepLabel = "Next";
		
		// Calculate width
		ImVec2 playSize = ImGui::CalcTextSize(playLabel);
		ImVec2 pauseSize = ImGui::CalcTextSize(pauseLabel);
		ImVec2 stepSize = ImGui::CalcTextSize(stepLabel);
		
		float padding = 10.0f;
		float totalWidth = playSize.x + pauseSize.x + stepSize.x + (padding * 2) + (ImGui::GetStyle().ItemSpacing.x * 2);
		
		ImGui::SetCursorPosX((availWidth - totalWidth) * 0.5f);
		
		// Play/Stop
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button(playLabel)) {
			bool running = ScriptingSystem::Instance().IsRunning();
			if (!running) {
				CapturePlayModeSceneState();
				if (ctx != nullptr && ctx->engine != nullptr) {
					ctx->engine->ResetFrameTimer();
				}
				ScriptingSystem::Instance().SetRunning(true);
				ScriptingSystem::Instance().SetPaused(false);
			} else {
				StopPlayMode();
			}
		}
		ImGui::SameLine();
		
		// Pause
		// Highlight if paused
		bool isPaused = ScriptingSystem::Instance().IsPaused();
		if (isPaused) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
		if (ImGui::Button(pauseLabel)) {
			ScriptingSystem::Instance().SetPaused(!isPaused);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (isPaused) ImGui::PopStyleColor();
		ImGui::SameLine();
		
		// Step
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button(stepLabel)) {
			if (ScriptingSystem::Instance().IsRunning() && ScriptingSystem::Instance().IsPaused()) {
				ScriptingSystem::Instance().Step(1);
			}
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
	}

	ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

	if (m_IsPrefabEditingMode && !m_PrefabObjectsLoadedForEditor)
	{
		// Runtime rendering keeps the scene snapshot loaded after each frame in
		// scene-context prefab mode. Reload the isolated prefab snapshot before
		// editor UI windows read hierarchy/inspector state so those windows do
		// not flicker between scene objects and prefab objects.
		EnsurePrefabObjectsLoadedForEditor();
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& win : m_Windows) {
		const bool drawSceneViewInSceneContext = ShouldSceneViewUseSceneContextForPrefabEdit()
			&& std::dynamic_pointer_cast<SceneViewWindow>(win) != nullptr
			&& m_PrefabObjectsLoadedForEditor;
		if (drawSceneViewInSceneContext)
		{
			// Scene-context prefab editing should look like Unity's in-context
			// mode: Scene View uses the real scene snapshot, while the hierarchy
			// and inspector keep editing the isolated prefab data.
			CaptureCurrentPrefabEditingState();
			LoadPrefabReturnSceneSnapshotIntoObjectManager(true);
			m_PrefabObjectsLoadedForEditor = false;
			win->Draw();
			EnsurePrefabObjectsLoadedForEditor();
		}
		else
		{
			if (m_IsPrefabEditingMode && !m_PrefabObjectsLoadedForEditor)
			{
				EnsurePrefabObjectsLoadedForEditor();
			}
			win->Draw();
		}
	}

	DrawSaveConfirmationModal();
	DrawImportCloseConfirmationModal();
	DrawScriptCompilationModal();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

	ImGui::End();
	RestoreSceneObjectsForRuntimeDuringPrefabEdit();
}

// MainEditorSubsystem::DrawMainMenuBar is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawMainMenuBar() {
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
				const bool saved = m_IsPrefabEditingMode ? SaveOpenPrefab() : SceneManager::Instance().SaveScene(m_CurrentScenePath);
				if (saved && !m_IsPrefabEditingMode) {
					UndoRedoSystem::Instance().SetSavePoint();
				}
			}
			ImGui::Separator();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Build Profiles...", "Ctrl+Shift+B")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Build and Run", "Ctrl+B")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
					win->StartExportAndRun();
				}
			}
			ImGui::Separator();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Close Project")) {
				RequestCloseProject();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Exit")) {
				RequestExit();
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Edit")) {
			// Context-aware menu enabling ensures users know when history is 
			// available or exhausted.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, UndoRedoSystem::Instance().CanUndo())) {
				UndoRedoSystem::Instance().Undo();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, UndoRedoSystem::Instance().CanRedo())) {
				UndoRedoSystem::Instance().Redo();
			}
			ImGui::Separator();
			// Project Settings lives under Edit because it changes persistent
			// project configuration rather than opening another transient tool.
			if (ImGui::MenuItem(TerraEngine::Editor::ProjectSettings::kProjectSettingsMenuLabel.data())) {
				OpenProjectSettings();
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("View")) {
			ImGui::MenuItem("ImGui Demo", nullptr, &showDemoWindow);
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Scripting")) {
			// Keep script actions reachable from the menu, but leave build status
			// text out of the chrome so attaching scripts stays drag-and-drop only.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Open Workspace")) {
				ScriptCompilationSubsystem::Instance().OpenWorkspace();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Regenerate Workspace")) {
				ScriptCompilationSubsystem::Instance().RegenerateWorkspace();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Rebuild Scripts")) {
				ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ManualRefresh);
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Windows")) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (auto& win : m_Windows) {
				if (!TerraEngine::Editor::ProjectSettings::ShouldShowWindowInWindowMenu(win->GetTitle())) {
					continue;
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::MenuItem(win->GetTitle().c_str())) {
					win->SetVisible(true);
				}
			}
			
			ImGui::Separator();

			EditorSystem* editorSystem = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx && ctx->engine) {
				editorSystem = dynamic_cast<EditorSystem*>(ctx->engine->GetEditorSystem());
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (editorSystem) {
				editorSystem->DrawWindowMenu();
			}
			
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}
}

// MainEditorSubsystem::RequestOpenScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestOpenScene(const std::filesystem::path& path) {
	if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
		m_PendingAction = PendingAction::OpenScene;
		m_PendingScenePath = path;
		m_ShowSaveConfirmation = true;
	} else {
		OpenScene(path);
	}
}

// MainEditorSubsystem::RequestCloseProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestCloseProject() {
	if (AssetDatabase::Instance().IsCompiling()) {
		m_PendingAction = PendingAction::CloseProject;
		m_ShowImportCloseConfirmation = true;
		return;
	}
	if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
		m_PendingAction = PendingAction::CloseProject;
		m_ShowSaveConfirmation = true;
	} else {
		CloseCurrentProject();
	}
}

// MainEditorSubsystem::RequestExit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestExit() {
	if (AssetDatabase::Instance().IsCompiling()) {
		m_PendingAction = PendingAction::Exit;
		m_ShowImportCloseConfirmation = true;
		return;
	}
	if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
		m_PendingAction = PendingAction::Exit;
		m_ShowSaveConfirmation = true;
	} else {
		CloseCurrentProject();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ctx && ctx->engine) ctx->engine->SignalShutdown();
	}
}

// MainEditorSubsystem::ExecutePendingAction is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::ExecutePendingAction() {
	switch (m_PendingAction) {
		case PendingAction::OpenScene:
			OpenScene(m_PendingScenePath);
			break;
		case PendingAction::CloseProject:
			CloseCurrentProject();
			break;
		case PendingAction::Exit:
			CloseCurrentProject();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx && ctx->engine) ctx->engine->SignalShutdown();
			break;
		default:
			break;
	}
	m_PendingAction = PendingAction::None;
	m_PendingScenePath.clear();
}

// MainEditorSubsystem::DrawSaveConfirmationModal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawSaveConfirmationModal() {
	if (m_ShowSaveConfirmation) {
		ImGui::OpenPopup("Save Changes?");
		m_ShowSaveConfirmation = false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopupModal("Save Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("There are unsaved changes to the current scene or prefab.\nDo you want to save them?");
		ImGui::Separator();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Yes", ImVec2(120, 0))) {
			if (m_IsPrefabEditingMode) {
				const bool restoreSceneNeedsSave = SceneManager::Instance().IsActiveSceneDirty();
				SaveOpenPrefab();
				ClosePrefabMode();
				if (restoreSceneNeedsSave) {
					SceneManager::Instance().SaveScene(m_CurrentScenePath);
					UndoRedoSystem::Instance().SetSavePoint();
				}
			} else {
				SceneManager::Instance().SaveScene();
				UndoRedoSystem::Instance().SetSavePoint();
			}
			ExecutePendingAction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("No", ImVec2(120, 0))) {
			if (m_IsPrefabEditingMode) {
				m_PrefabEditingDirty = false;
			}
			ExecutePendingAction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_PendingAction = PendingAction::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void MainEditorSubsystem::DrawImportCloseConfirmationModal() {
	if (m_ShowImportCloseConfirmation) {
		ImGui::OpenPopup("Import In Progress");
		m_ShowImportCloseConfirmation = false;
	}

	if (ImGui::BeginPopupModal("Import In Progress", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		const size_t jobCount = AssetDatabase::Instance().GetCompileJobsCount();
		ImGui::TextWrapped("Asset import is still running (%zu job%s). Closing now will cancel active imports and remove partial imported assets from the project.",
			jobCount,
			jobCount == 1u ? "" : "s");
		ImGui::Separator();

		if (ImGui::Button("Continue Closing", ImVec2(150, 0))) {
			AssetDatabase::Instance().RequestCancelAllImports(true);
			ImGui::CloseCurrentPopup();
			if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
				m_ShowSaveConfirmation = true;
			} else {
				ExecutePendingAction();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_PendingAction = PendingAction::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// MainEditorSubsystem::DrawScriptCompilationModal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawScriptCompilationModal() {
	const ScriptCompilationSubsystem& scriptCompilation = ScriptCompilationSubsystem::Instance();
	const ScriptCompilationStatus& scriptStatus = scriptCompilation.GetStatus();
	const bool isCompilingScripts = scriptCompilation.IsBusy();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (isCompilingScripts) {
		ImGui::OpenPopup("Compiling C# Scripts");
	}

	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopupModal("Compiling C# Scripts", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!isCompilingScripts) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextWrapped("TerraEngine paused gameplay script execution while Assembly-CSharp recompiles.");
		ImGui::Spacing();
		ImGui::TextUnformatted(scriptStatus.lastSummary.c_str());
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
		const char* progressLabel = scriptStatus.state == ScriptCoordinatorState::Reloading ? "Reloading..." : "Compiling...";
		// Use a negative fraction animated by ImGui time to render an
		// indeterminate progress bar while the build runs in the background.
		ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(360.0f, 0.0f), progressLabel);
		ImGui::PopStyleColor();
		ImGui::EndPopup();
	}
}

// MainEditorSubsystem::CloseCurrentProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::CloseCurrentProject() {
	ScriptingSystem::Instance().SetRunning(false);
	ScriptingSystem::SetScriptingDiagnosticsRequested(false);
	TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
	ClearPlayModeSceneState();
	ScriptCompilationSubsystem::Instance().OnProjectClosed();
	m_IsPrefabEditingMode = false;
	m_PrefabEditingDirty = false;
	m_HasPrefabReturnSceneSnapshot = false;
	m_PrefabReturnSceneSnapshotJson.clear();
	m_PrefabReturnSelectedEntityUuid.reset();
	m_PrefabReturnGameViewCameras.clear();
	m_PrefabEditingPath.clear();
	m_PrefabObjectsLoadedForEditor = false;
	m_PrefabEditOpenedFromSceneObject = false;
	m_PrefabEditingObjectSnapshotJson.clear();
	m_PrefabEditingSelectedEntityUuid.reset();

	// Clear Scene
	SceneManager::Instance().UnloadAll();
	
	// Clear Objects
	ObjectManager::instance().clear();

	// Clear Undo History
	UndoRedoSystem::Instance().Clear();
	
	// Clear Resources
	ResourceManager::Instance().ClearCaches();
	ResourceManager::Instance().LoadDefaultResources();
	
	// Clear Renderer Resources
	// This ensures GPU resources are flushed and binding indices are reset.
	if (ctx && ctx->engine) {
		RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
		if (renderSystem) {
			renderSystem->Clear();
		}
	}

	// Clear Selection
	selectedEntity = std::nullopt;
	selectedAsset = std::nullopt;
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
		ScriptingSystem::Instance().UpdateInspectorWatch(std::nullopt, ctx->scripting.selectionVersion);
	}
	
	// Trigger callbacks BEFORE closing the project in ProjectManager
	// so that subsystems can save data to the project directory.
	if (ctx && ctx->editorProfiler) {
		ctx->editorProfiler->OnProjectClosed();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (onProjectClosed) onProjectClosed();

	// Close Project
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx && ctx->projectManager) {
		ctx->projectManager->CloseProject();
	}
}
