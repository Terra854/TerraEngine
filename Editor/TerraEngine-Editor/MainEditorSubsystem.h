#pragma once

#include "IEditorSubsystem.h"
#include <functional>
#include <optional>
#include <entt/entity/entity.hpp>

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
    
    EditorContext* ctx = nullptr;
    std::function<void()> onProjectClosed;
    
    // Internal State
    bool showDemoWindow = false;
    void* sceneTextureID = nullptr;
    bool sceneLoaded = false;
    
    std::optional<entt::entity> selectedEntity;
};
