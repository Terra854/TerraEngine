#pragma once

#include "IEditorSubsystem.h"
#include <functional>

class MainEditorSubsystem : public IEditorSubsystem {
public:
    virtual void Initialize(EditorContext& context) override;
    virtual void OnBackendReady() override;
    virtual void Update(float deltaTime) override;
    virtual void DrawUI() override;
    virtual void Shutdown() override;
    virtual std::string GetName() const override { return "Main Editor"; }

    void SetOnProjectClosedCallback(std::function<void()> callback);

private:
    void DrawMainMenuBar();
    void DrawProjectBrowser();
    
    EditorContext* ctx = nullptr;
    std::function<void()> onProjectClosed;
    
    // Internal State
    bool showDemoWindow = false;
    void* sceneTextureID = nullptr;
};
