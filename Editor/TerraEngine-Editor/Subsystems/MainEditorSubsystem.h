#pragma once

#include <functional>
#include <optional>
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <filesystem>
#include "IEditorSubsystem.h"

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

private:
    void DrawMainMenuBar();
    void DrawProjectBrowser();
    void DrawHierarchy();
    void DrawInspector();
    void DrawEntityNode(Object& obj);
    
    void DrawDirectoryTree(const std::filesystem::path& path);
    void DrawDirectoryContent(const std::filesystem::path& path);

    EditorContext* ctx = nullptr;
    std::function<void()> onProjectClosed;
    
    // Internal State
    bool showDemoWindow = false;
    void* sceneTextureID = nullptr;
    void* cachedSceneImageView = nullptr;
    bool sceneLoaded = false;
    
    std::optional<entt::entity> selectedEntity;
    std::map<entt::entity, std::vector<std::string>> perObjectComponentOrders;
    entt::entity inspectorEntityId = entt::null;

    // Project Browser State
    std::filesystem::path m_CurrentDirectory;
    std::string m_SearchFilter;
    float m_ThumbnailSize = 64.0f;
    float m_Padding = 16.0f;
};
