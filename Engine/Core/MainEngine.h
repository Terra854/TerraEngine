#pragma once
#include <iostream>
#include <memory>
#include <vector>

#ifdef _WIN32
#    include <windows.h>
#endif

#include "SystemBase.h"
#include "ThreadPool/ThreadPool.h"

class WindowSystem;
class RendererSystem;

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

    ThreadPool& pool = ThreadPool::instance();

    WindowSystem* windowSystem = nullptr;
    RendererSystem* rendererSystem = nullptr;
};

