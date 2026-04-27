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
#include <filesystem>
#include <memory>
#include "IEditorSubsystem.h"
#include "../Framework/EditorWindow.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Utilities/UUID.h"

class Object;

// class MainEditorSubsystem groups related state and operations so callers have one documented contract.
class MainEditorSubsystem : public IEditorSubsystem {
public:
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
	
	// Opens a scene and updates the last opened scene tracking.
	void OpenScene(const std::filesystem::path& path);
	
	// Request actions with dirty check
	void RequestOpenScene(const std::filesystem::path& path);
	void RequestCloseProject();
	void RequestExit();

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

private:
	void CapturePlayModeSceneState();
	void RestorePlayModeSceneState();
	void ClearPlayModeSceneState();
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

	void* sceneTextureID = nullptr;
	void* cachedSceneImageView = nullptr;
	void* gameTextureID = nullptr;
	void* cachedGameImageView = nullptr;
	bool sceneLoaded = false;
	
	std::optional<entt::entity> selectedEntity;
	std::optional<std::filesystem::path> selectedAsset;
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
