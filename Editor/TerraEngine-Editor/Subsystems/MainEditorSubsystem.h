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

class Object;

class MainEditorSubsystem : public IEditorSubsystem {
public:
    virtual void Initialize(EditorContext& context) override;
    virtual void OnBackendReady() override;
    virtual void Update(float deltaTime) override;
    virtual void DrawUI() override;
    virtual void Shutdown() override;
    virtual std::string GetName() const override { return "Main Editor"; }

    void OnProjectOpened();
    void SetOnProjectClosedCallback(std::function<void()> callback);
	// Reports whether a drag control in the inspector is active this frame for cursor wrapping.
	bool IsInspectorValueDragging() const;

    // API for Windows
    EditorContext* GetContext() const { return ctx; }

    std::optional<entt::entity> GetSelectedEntity() const { return selectedEntity; }
    void SetSelectedEntity(std::optional<entt::entity> entity) { 
        selectedEntity = entity; 
        if (entity) selectedAsset = std::nullopt;
    }

    std::optional<std::filesystem::path> GetSelectedAsset() const { return selectedAsset; }
    void SetSelectedAsset(std::optional<std::filesystem::path> asset) { 
        selectedAsset = asset; 
        if (asset) selectedEntity = std::nullopt;
    }
    
    // Opens a scene and updates the last opened scene tracking.
    void OpenScene(const std::filesystem::path& path);
    
    // Request actions with dirty check
    void RequestOpenScene(const std::filesystem::path& path);
    void RequestCloseProject();
    void RequestExit();

    void* GetSceneTextureID() const { return sceneTextureID; }
    void* GetGameTextureID() const { return gameTextureID; }
	bool TryGetGameViewRenderStats(RenderStatsSnapshot& outStats) const;

    template<typename T>
    std::shared_ptr<T> GetWindow() const {
        for (auto& win : m_Windows) {
            if (auto casted = std::dynamic_pointer_cast<T>(win)) {
                return casted;
            }
        }
        return nullptr;
    }

private:
    void DrawMainMenuBar();
    void CloseCurrentProject();
    
    EditorContext* ctx = nullptr;
    std::function<void()> onProjectClosed;
    
    // Windows
    std::vector<std::shared_ptr<EditorWindow>> m_Windows;

    // Internal State
    bool showDemoWindow = false;

    void* sceneTextureID = nullptr;
    void* cachedSceneImageView = nullptr;
    void* gameTextureID = nullptr;
    void* cachedGameImageView = nullptr;
    bool sceneLoaded = false;
    
    std::optional<entt::entity> selectedEntity;
    std::optional<std::filesystem::path> selectedAsset;

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
};
