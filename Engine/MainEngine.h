#pragma once
#include <iostream>
#include <memory>
#include <vector>

#ifdef _WIN32
#    include <windows.h>
#endif

#include "SystemBase.h"
#include "ObjectManager.h"
#include "ThreadPool.h"

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
};
