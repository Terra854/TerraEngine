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

    void* GetSceneTextureID() const { return sceneTextureID; }

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
    
    EditorContext* ctx = nullptr;
    std::function<void()> onProjectClosed;
    
    // Windows
    std::vector<std::shared_ptr<EditorWindow>> m_Windows;

    // Internal State
    bool showDemoWindow = false;

    void* sceneTextureID = nullptr;
    void* cachedSceneImageView = nullptr;
    bool sceneLoaded = false;
    
    std::optional<entt::entity> selectedEntity;
    std::optional<std::filesystem::path> selectedAsset;
};
