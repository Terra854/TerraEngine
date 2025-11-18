#pragma once
#include <iostream>
#include <memory>
#include <vector>

#ifdef _WIN32
// Ensure Windows.h doesn't inject the legacy min/max macros that break RTTR's
// std::numeric_limits usage when building the engine on MSVC.
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include "SystemBase.h"
#include "Object/ObjectManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Timer.h"
#include "Window/WindowSystem.h"

class Engine {
public:
    void Run();
    void SignalShutdown();

private:
    void Initialize();
    void MainEngineLoop();
    void Shutdown();

    bool running = true;
    std::vector<std::unique_ptr<SystemsBase>> systems;

    ObjectManager& objManager = ObjectManager::instance();
    ThreadPool& pool = ThreadPool::instance();

    // Window backend is owned through the abstraction so different toolkits can be
    // swapped in without touching the core engine logic.
    std::unique_ptr<IWindowSystem> windowSystem;
    Timer frameTimer;
};

