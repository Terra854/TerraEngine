// MainEditorSubsystem.h — Core editor workspace system
//
// Defines the primary workspace system responsible for managing the main editor UI and active project state. it coordinates between the various editor windows, handles scene opening requests, and manages the transition into and out of play mode.

#pragma once

#include "Core/Scheduling/FrameWorkload.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include "IEditorSubsystem.h"
#include "../Framework/EditorWindow.h"
#include "../Project/EditorPreferencesMetadata.h"
#include "../Project/ProjectSettingsMetadata.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Animation/AnimationTypes.h"
#include "Engine/Scene/BulkScenePublication.h"
#include "Engine/Scene/TransactionalMutation.h"
#include "Engine/Utilities/UUID.h"
#include "../Core/ViewportTextureRegistry.h"

class Object;
class VulkanContext;
namespace TerraEngine::Runtime { struct PhysicsInvariantReport; }

// PrefabAuthoringService owns the semantic prefab operations shared by the
// native editor and automation. Its session state is independent from MCP, so
// future UI callers can use the same stable identities and conflict rules.
class PrefabAuthoringService {
public:
	struct Result {
		bool ok = false;
		std::string errorCategory;
		std::string message;
		nlohmann::json data = nlohmann::json::object();
	};

	// BulkInstantiationRequest is the typed worker-prepared payload consumed by
	// the protected prefab publication boundary. Keeping grid math out of JSON
	// handlers makes native editor callers and MCP automation share one contract.
	struct BulkInstantiationRequest {
		std::uint32_t count = 1u;
		float spacing = 0.0f;
		std::uint32_t columns = 0u;
		glm::vec3 origin = glm::vec3(0.0f);
		std::string namePrefix;
		std::optional<entt::entity> parent;
		// An authored animation policy must be present before the protected
		// publication becomes visible. Applying it here prevents an automatic
		// GPU frame from racing a later per-object automation mutation.
		std::optional<TerraEngine::Animation::AnimationExecutionPolicy>
			requestedAnimationMode;
		// Optional direct clips are assigned round-robin before publication. This
		// supports ordinary crowd authoring and reproducible mixed-playback tests
		// without exposing partially configured AnimatorComponents to the runtime.
		std::vector<TerraEngine::UUID> defaultClipAssetIds;
		// A non-zero step offsets each instance on the fixed animation timeline.
		// Animation binding performs the final duration-aware clamp or loop.
		std::uint32_t defaultClipStartFrameStep = 0u;
	};

	Result Create(entt::entity sourceEntity, const std::filesystem::path& projectRelativePath, const std::filesystem::path& absolutePath);
	// RegisterExistingAsset lets editor-owned asset discovery attach a prefab
	// that predates this service session. The AssetDatabase GUID remains the
	// stable identity, while the generation tracks atomic package reimports.
	Result RegisterExistingAsset(
		const std::filesystem::path& projectRelativePath,
		const std::filesystem::path& absolutePath,
		const TerraEngine::UUID& assetGuid,
		std::uint64_t publishedGeneration = 1u);
	// Registered prefab assets are instantiated as their complete serialized
	// object graph. Session-created lightweight prefabs retain their historical
	// single-root behavior until AssetDatabase publishes them.
	Result Instantiate(
		std::string_view prefabIdentity,
		std::string_view requestedName,
		std::optional<entt::entity> parent = std::nullopt);
	// InstantiateMany publishes every requested graph in one protected bulk
	// transaction. Rejection leaves object storage, service identity state,
	// scene dirtiness, and editor history unchanged.
	Result InstantiateMany(
		std::string_view prefabIdentity,
		const BulkInstantiationRequest& request);
	Result InspectOverrides(std::string_view instanceIdentity) const;
	Result ApplyOverrides(std::string_view instanceIdentity, std::optional<std::uint64_t> expectedGeneration);
	Result RevertOverrides(std::string_view instanceIdentity);
	Result OpenEditContext(std::string_view prefabIdentity);
	Result CloseEditContext(std::string_view contextIdentity);

private:
	struct PrefabState {
		std::string prefabId;
		std::string relativePath;
		std::string sourceObjectId;
		std::string sourceName;
		std::uint64_t generation = 1u;
		// Only AssetDatabase-registered prefabs have an authoritative disk graph.
		// Keeping the absolute path here lets every caller use one protected
		// deserialization path without changing lightweight session semantics.
		std::filesystem::path absolutePath;
		TerraEngine::UUID assetGuid = TerraEngine::EMPTY_UUID;
		// Registration captures the validated document that owns the reported
		// generation, avoiding a file-watcher race between indexing and publish.
		nlohmann::json serializedDocument;
		bool hasSerializedGraph = false;
	};
	struct InstanceState {
		std::string prefabId;
		std::string sourceName;
	};

	std::string ResolvePrefabId(std::string_view identity) const;
	std::unordered_map<std::string, PrefabState> prefabsById;
	std::unordered_map<std::string, std::string> prefabIdByPath;
	std::unordered_map<std::string, InstanceState> instances;
	std::string activeContextId;
	std::string activeContextPrefabId;
	std::uint64_t nextPrefabId = 1u;
};

// class MainEditorSubsystem groups related state and operations so callers have one documented contract.
class MainEditorSubsystem : public IEditorSubsystem {
public:
	// Play Mode restoration is published at one protected boundary. Automation
	// reads this immutable summary after Stop instead of guessing whether a stale
	// scene generation or cancellation left authored state untouched.
	struct PlayModeRestorationResult
	{
		bool snapshotCaptured = false;
		bool completed = false;
		bool stale = false;
		bool cancelled = false;
		bool selectionRestored = false;
		bool dirtyStateRestored = false;
		bool prefabStatePreserved = false;
		std::uint64_t preparedGeneration = 0U;
		std::uint64_t publishedGeneration = 0U;
		std::string boundary = "play_mode_stop_restore";
		std::string reason;
	};

	struct SelectedPackageChildAsset
	{
		std::filesystem::path packagePath;
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::string kind;
		std::string displayName;
	};

	virtual void Initialize(EditorContext& context) override;
	virtual void OnBackendReady() override;
	virtual void Update(float deltaTime) override;
	virtual void DrawUI() override;
	virtual void Shutdown() override;
	// EditorSystem forwards this independent terrain system contract into the
	// frame scheduler without making ImGui callbacks publish terrain mutations.
	[[nodiscard]] std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareTerrainAuthoringWorkloads(float deltaTime);
	// GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual std::string GetName() const override { return "Main Editor"; }

	void OnProjectOpened();
	void SetOnProjectClosedCallback(std::function<void()> callback);
	// Native toolbar and loopback automation share these lifecycle methods so
	// scene snapshots, Jolt world ownership, scripting, and fixed-step controls
	// cannot drift into two different Play Mode implementations.
	bool StartPlayMode();
	bool SetPlayModePaused(bool paused);
	bool StepPlayMode();
	void StopPlayMode();
	[[nodiscard]] const PlayModeRestorationResult& GetLastPlayModeRestorationResult() const { return m_LastPlayModeRestorationResult; }
	// The installed acceptance CLI uses the same Play Mode entry, fixed-step,
	// publication, and stop methods as the toolbar. It returns engine-owned
	// invariant counts for comparison with the exported runtime report.
	TerraEngine::Runtime::PhysicsInvariantReport RunPhysicsExportInvariantFixture();
	[[nodiscard]] nlohmann::json RunTerrainLiveValidationFixture();
	// Inspector assignment becomes visible only after SceneMutationQueue flushes.
	// Keep restoration armed across that boundary so the terrain window can open
	// the newly referenced asset and publish its derived renderer resources.
	void RequestTerrainProxyRestore() noexcept { terrainProxyRestorePending = true; }
	// Reports whether a drag control in the inspector is active this frame for cursor wrapping.
	bool IsInspectorValueDragging() const;

	// API for Windows
	// GetContext is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	EditorContext* GetContext() const { return ctx; }

	std::optional<entt::entity> GetSelectedEntity() const;
	// Multi-selection stores authored UUIDs instead of transient EnTT handles, so
	// scene reloads and prefab-context swaps cannot silently retarget an edit.
	std::vector<entt::entity> GetSelectedEntities() const;
	const std::vector<TerraEngine::UUID>& GetSelectedEntityUuids() const { return selectedEntityUuids; }
	bool IsEntitySelected(entt::entity entity) const;
	void SetSelectedEntity(std::optional<entt::entity> entity);
	void SetSelectedEntities(const std::vector<entt::entity>& entities);
	void ToggleSelectedEntity(entt::entity entity);
	// Copies the selected terrain's parent asset plus its private immutable data
	// bundle, then queues the component reference change at the scene boundary.
	// The original terrain remains untouched for every other scene instance.
	bool CreateIndependentTerrainCopy(entt::entity entity, std::string& outError);

	// GetSelectedAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::optional<std::filesystem::path> GetSelectedAsset() const { return selectedAsset; }
	void SetSelectedAsset(std::optional<std::filesystem::path> asset);
	// Selects a cooked animation document and reveals its matching native editor.
	// The title mapping avoids coupling Project Browser to every window class.
	bool OpenAnimationAssetEditor(const std::filesystem::path& path);
	// Generated package children are not standalone source files, so the
	// inspector needs their package path plus GUID to show the right read-only
	// sub-asset details instead of falling back to the parent glTF/FBX summary.
	std::optional<SelectedPackageChildAsset> GetSelectedPackageChildAsset() const { return selectedPackageChildAsset; }
	void SetSelectedPackageChildAsset(std::optional<SelectedPackageChildAsset> asset);
	
	// Opens a scene and updates the last opened scene tracking.
	void OpenScene(const std::filesystem::path& path);
	// Opens a prefab asset in an isolated hierarchy so the existing scene is
	// protected while authoring prefab defaults.
	bool OpenPrefabAsset(const std::filesystem::path& path);
	// Instantiates a prefab asset into the live hierarchy. The payload is a
	// prefab asset GUID rather than a filesystem path so Project Browser drag
	// sources can also be reused by future prefab-aware property fields.
	bool InstantiatePrefabAsset(const TerraEngine::UUID& prefabGuid, std::optional<entt::entity> parent = std::nullopt);
	bool OpenPrefabFromObject(entt::entity object);
	bool ResetPrefabInstanceToDefaults(entt::entity object);
	bool SaveOpenPrefab();
	void ClosePrefabMode();
	bool IsPrefabEditingMode() const { return m_IsPrefabEditingMode; }
	bool IsAssetPrefabEditingMode() const { return m_IsPrefabEditingMode && !m_PrefabEditOpenedFromSceneObject; }
	std::filesystem::path GetPrefabEditingPath() const { return m_PrefabEditingPath; }
	bool IsPrefabEditingDirty() const { return m_PrefabEditingDirty; }
	bool TryApplyPrefabModeGameViewCameras(float aspect) const;
	PrefabAuthoringService& GetPrefabAuthoringService() { return prefabAuthoringService; }
	bool ShouldSceneViewUseSceneContextForPrefabEdit() const { return m_IsPrefabEditingMode && m_PrefabEditOpenedFromSceneObject; }
	
	// Request actions with dirty check
	void RequestOpenScene(const std::filesystem::path& path);
	void RequestCloseProject();
	void RequestExit();
	bool RebuildAssetFromInspector(const std::filesystem::path& path, std::string* outError = nullptr);

	// GetSceneTextureID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* GetSceneTextureID() const { return viewportTextures.ActiveDescriptor(TerraEngine::Editor::ViewportTextureKind::Scene); }
	void* GetGameTextureID() const { return viewportTextures.ActiveDescriptor(TerraEngine::Editor::ViewportTextureKind::Game); }
	void CaptureViewportDrawDataUses();
	void CommitViewportDrawDataUses(std::uint64_t submittedSerial);
	void AbandonViewportDrawDataUses();
	void ReleaseCompletedViewportTextures(std::uint64_t completedSerial);
	void ShutdownViewportTextures(bool deviceLost);
	bool TryGetGameViewRenderStats(RenderStatsSnapshot& outStats) const;

	template<typename T>
	// GetWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::shared_ptr<T> GetWindow() const {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& win : m_Windows) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto casted = std::dynamic_pointer_cast<T>(win)) {
				return casted;
			}
		}
		return nullptr;
	}

	// GetWindows is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::vector<std::shared_ptr<EditorWindow>>& GetWindows() const { return m_Windows; }
	void OpenProjectSettings(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId =
		TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Project);
	void OpenPreferences(TerraEngine::Editor::Preferences::PreferencesCategoryId categoryId =
		TerraEngine::Editor::Preferences::PreferencesCategoryId::Diagnostics);
	// The project hub renders only editor-wide windows. Project-dependent windows
	// remain hidden until an active project owns their underlying state.
	void DrawProjectHubWindows();

private:
	void CapturePlayModeSceneState();
	void RestorePlayModeSceneState();
	void ClearPlayModeSceneState();
	void CapturePrefabReturnSceneState();
	void RestorePrefabReturnSceneState();
	void CaptureCurrentPrefabEditingState();
	bool EnsurePrefabObjectsLoadedForEditor();
	void RestoreSceneObjectsForRuntimeDuringPrefabEdit();
	bool LoadPrefabEditingSnapshotIntoObjectManager();
	bool LoadPrefabReturnSceneSnapshotIntoObjectManager(bool restoreSelection);
	bool LoadPrefabIntoIsolatedObjectManager(const std::filesystem::path& path);
	bool OpenPrefabAssetInternal(const std::filesystem::path& path, bool openedFromSceneObject);
	void PublishEditorStreamingNeeds();
	void PublishCommittedEditorTransactionsToUndo();
	void RefreshViewportTextures(VulkanContext& vulkan);
	void DrawMainMenuBar();
	void CloseCurrentProject();
	
	EditorContext* ctx = nullptr;
	std::function<void()> onProjectClosed;
	
	// Windows
	std::vector<std::shared_ptr<EditorWindow>> m_Windows;

	// Internal State
	bool showDemoWindow = false;
	bool m_HasPlayModeSceneSnapshot = false;
	bool m_PlayModeSceneWasDirty = false;
	std::optional<TerraEngine::Scene::BulkScenePublicationPacket> m_PlayModeSceneSnapshotPacket;
	SceneRenderSettings m_PlayModeSceneRenderSettings;
	PlayModeRestorationResult m_LastPlayModeRestorationResult;
	std::optional<TerraEngine::UUID> m_PlayModeSelectedEntityUuid;
	std::vector<TerraEngine::UUID> m_PlayModeSelectedEntityUuids;
	bool m_IsPrefabEditingMode = false;
	bool m_PrefabEditingDirty = false;
	bool m_HasPrefabReturnSceneSnapshot = false;
	bool m_PrefabReturnSceneWasDirty = false;
	std::string m_PrefabReturnSceneSnapshotJson;
	std::optional<TerraEngine::UUID> m_PrefabReturnSelectedEntityUuid;
	std::filesystem::path m_PrefabEditingPath;
	bool m_SuppressObjectModifiedCallback = false;
	bool m_PrefabObjectsLoadedForEditor = false;
	bool m_PrefabEditOpenedFromSceneObject = false;
	std::string m_PrefabEditingObjectSnapshotJson;
	std::optional<TerraEngine::UUID> m_PrefabEditingSelectedEntityUuid;

	// Cached camera data lets prefab isolation replace the hierarchy ObjectManager
	// without changing which authored scene camera drives the Game View.
	struct PrefabReturnGameViewCamera
	{
		// Preserve the authored canonical pose while prefab isolation temporarily
		// replaces the live scene. The view matrix is rebuilt in a bounded frame.
		TerraEngine::Scene::WorldPose worldPose{};
		glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
		float verticalFovDegrees = 45.0f;
		float nearPlane = 0.1f;
		float farPlane = 100.0f;
	};
	std::vector<PrefabReturnGameViewCamera> m_PrefabReturnGameViewCameras;

	TerraEngine::Editor::ViewportTextureRegistry viewportTextures;
	bool sceneLoaded = false;
	// Project asset discovery can finish after the startup scene is deserialized.
	// Keep one bounded retry active until the scene's terrain UUID is registered.
	bool terrainProxyRestorePending = false;
	// This set records component-owned assets observed at the prior protected
	// editor boundary. A disappearance retires derived render and physics state.
	std::unordered_set<TerraEngine::UUID> observedTerrainComponentAssets;
	
	std::optional<entt::entity> selectedEntity;
	std::vector<TerraEngine::UUID> selectedEntityUuids;
	std::optional<std::filesystem::path> selectedAsset;
	std::optional<SelectedPackageChildAsset> selectedPackageChildAsset;
	PrefabAuthoringService prefabAuthoringService;
	std::filesystem::path m_CurrentScenePath;
	// Receipts intentionally omit their forward payload, so editor transactions
	// are cached before the next protected flush and paired by stable ID later.

	// PendingAction names the supported states explicitly so serialization and switches stay readable.
	enum class PendingAction {
		None,
		OpenScene,
		CloseProject,
		Exit
	};

	PendingAction m_PendingAction = PendingAction::None;
	std::filesystem::path m_PendingScenePath;
	bool m_ShowSaveConfirmation = false;
	bool m_ShowImportCloseConfirmation = false;
	
	void ExecutePendingAction();
	void DrawSaveConfirmationModal();
	void DrawImportCloseConfirmationModal();
	void DrawScriptCompilationModal();
};
