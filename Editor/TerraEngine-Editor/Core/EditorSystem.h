#pragma once

#include "Core/SystemBase.h"
#include "EditorContext.h"
#include <memory>

class ProjectHubSubsystem;
class MainEditorSubsystem;
class Engine;

class EditorSystem : public SystemsBase {
public:
	void Setup(Engine* engine);
	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	void Draw();

private:
	EditorContext context;
	std::shared_ptr<ProjectHubSubsystem> projectHub;
    std::shared_ptr<MainEditorSubsystem> mainEditor;

    // Managed Editor Hooks
    typedef void (*ManagedInitFn)(void*);
    typedef void (*ManagedUpdateFn)(float);
    typedef void (*ManagedShutdownFn)();
    typedef void (*ManagedDrawWindowMenuFn)();

    ManagedInitFn managedInitialize = nullptr;
    ManagedUpdateFn managedUpdate = nullptr;
    ManagedShutdownFn managedShutdown = nullptr;
    ManagedDrawWindowMenuFn managedDrawWindowMenu = nullptr;

    bool initialized = false;
public:
    void DrawWindowMenu();
};

