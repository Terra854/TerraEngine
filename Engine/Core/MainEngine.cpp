#include "MainEngine.h"

#include "Platform/Window/WindowSystem.h"
#include "Rendering/RendererSystem.h"

#include <chrono>
#include <iostream>

void Engine::Run()
{
    Initialize();
    MainEngineLoop();
    Shutdown();
}

void Engine::SignalShutdown()
{
    running = false;
}

void Engine::Initialize()
{
    pool.initialize();

    auto window = std::make_unique<WindowSystem>();
    windowSystem = window.get();
    systems.push_back(std::move(window));

    auto renderer = std::make_unique<RendererSystem>(*windowSystem);
    rendererSystem = renderer.get();
    systems.push_back(std::move(renderer));

    for (auto& system : systems) {
        system->Initialize();
    }

    running = true;

    std::cout << "Setup complete!" << std::endl;
}

void Engine::MainEngineLoop()
{
    using clock = std::chrono::high_resolution_clock;
    auto lastTick = clock::now();

    while (running) {
        auto now = clock::now();
        (void)std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick);
        lastTick = now;

        for (auto& system : systems) {
            system->Update();
        }

        if (windowSystem && windowSystem->ShouldClose()) {
            running = false;
        }
    }
}

void Engine::Shutdown()
{
    for (auto& system : systems) {
        system->shutdown();
    }

    systems.clear();
    windowSystem = nullptr;
    rendererSystem = nullptr;

    pool.shutdown();

    std::cout << "Shutdown complete!" << std::endl;
}

