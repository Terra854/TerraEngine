// MainEditorSubsystem.h — Core editor workspace system
//
// Defines the primary workspace system responsible for managing the main editor UI and active project state. it coordinates between the various editor windows, handles scene opening requests, and manages the transition into and out of play mode.

#pragma once

#include <functional>
#include <optional>
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <memory>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include "IEditorSubsystem.h"
#include "../Framework/EditorWindow.h"
#include "../Project/ProjectSettingsMetadata.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Utilities/UUID.h"

class Object;

// class MainEditorSubsystem groups related state and operations so callers have one documented contract.
class MainEditorSubsystem : public IEditorSubsystem {
public:
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
	// GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual std::string GetName() const override { return "Main Editor"; }

	void OnProjectOpened();
	void SetOnProjectClosedCallback(std::function<void()> callback);
	void StopPlayMode();
	// Reports whether a drag control in the inspector is active this frame for cursor wrapping.
	bool IsInspectorValueDragging() const;

	// API for Windows
	// GetContext is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	EditorContext* GetContext() const { return ctx; }

	std::optional<entt::entity> GetSelectedEntity() const { return selectedEntity; }
	void SetSelectedEntity(std::optional<entt::entity> entity);

	// GetSelectedAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::optional<std::filesystem::path> GetSelectedAsset() const { return selectedAsset; }
	void SetSelectedAsset(std::optional<std::filesystem::path> asset);
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
	bool OpenPrefabFromObject(entt::entity object);
	bool ResetPrefabInstanceToDefaults(entt::entity object);
	bool SaveOpenPrefab();
	void ClosePrefabMode();
	bool IsPrefabEditingMode() const { return m_IsPrefabEditingMode; }
	bool IsAssetPrefabEditingMode() const { return m_IsPrefabEditingMode && !m_PrefabEditOpenedFromSceneObject; }
	std::filesystem::path GetPrefabEditingPath() const { return m_PrefabEditingPath; }
	bool IsPrefabEditingDirty() const { return m_PrefabEditingDirty; }
	bool TryApplyPrefabModeGameViewCameras(float aspect) const;
	bool ShouldSceneViewUseSceneContextForPrefabEdit() const { return m_IsPrefabEditingMode && m_PrefabEditOpenedFromSceneObject; }
	
	// Request actions with dirty check
	void RequestOpenScene(const std::filesystem::path& path);
	void RequestCloseProject();
	void RequestExit();
	bool RebuildAssetFromInspector(const std::filesystem::path& path, std::string* outError = nullptr);

	// GetSceneTextureID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* GetSceneTextureID() const { return sceneTextureID; }
	void* GetGameTextureID() const { return gameTextureID; }
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
	std::vector<std::string> GetWindowMenuTitlesForTests() const;
	static bool ShouldShowWindowInWindowMenuForTests(std::string_view title);

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
	std::string m_PlayModeSceneSnapshotJson;
	std::optional<TerraEngine::UUID> m_PlayModeSelectedEntityUuid;
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
		glm::vec3 position = glm::vec3(0.0f);
		glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
		float verticalFovDegrees = 45.0f;
		float nearPlane = 0.1f;
		float farPlane = 100.0f;
	};
	std::vector<PrefabReturnGameViewCamera> m_PrefabReturnGameViewCameras;

	void* sceneTextureID = nullptr;
	void* cachedSceneImageView = nullptr;
	void* gameTextureID = nullptr;
	void* cachedGameImageView = nullptr;
	bool sceneLoaded = false;
	
	std::optional<entt::entity> selectedEntity;
	std::optional<std::filesystem::path> selectedAsset;
	std::optional<SelectedPackageChildAsset> selectedPackageChildAsset;
	std::filesystem::path m_CurrentScenePath;

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
	
	void ExecutePendingAction();
	void DrawSaveConfirmationModal();
	void DrawScriptCompilationModal();
};
