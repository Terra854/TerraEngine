#include "MainEngine.h"
#include "SystemBase.h"
#include "Logger/Logger.h"
#include "Timer.h"
#include "Window/GlfwWindowBackend.h"
#include "Window/WindowSystem.h"

#include <algorithm>
#include <chrono>
#include <thread>

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

    // The window system is initialized first so other systems can depend on
    // a valid window context if they need one later.
    WindowConfig windowConfig{};
    systems.push_back(std::make_unique<WindowSystem>(std::make_unique<GlfwWindowBackend>(), windowConfig, [this]() {
        SignalShutdown();
    }));

    // Additional systems can be appended after the window so they update in
    // a predictable order.

    for (auto& system : systems) {
        system->Initialize();
    }

    // Confirm engine initialization finished so later logs have a baseline.
    TERRAENGINE_LOG(LogLevel::Info, "Setup complete!");
}

void Engine::MainEngineLoop()
{
    // Legacy ad hoc demos are preserved for reference but disabled so the
    // engine loop can focus on real system updates and shutdown handling.
    /*
    void simpleTask(int& counter) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        counter++;
    }

    void exceptionTask() {
        throw std::runtime_error("Intentional exception for testing");
    }

    void anotherTask(){
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Mark execution of a background task to prove asynchronous work runs.
        TERRAENGINE_LOG(LogLevel::Debug, "Another task executed");
    }

    Object obj1 = objManager.createObject("Object1");
    obj1.addComponent<Transform>();

    Transform* transform = obj1.getComponent<Transform>();

    // Exercise the CRTP-backed Transform component by tweaking each field with
    // glm math types so RTTR can observe the updates at runtime.
    transform->position = glm::vec3(90.0f, 180.0f, 270.0f);
    transform->rotation = glm::quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
    transform->scale = glm::vec3(2.0f, 2.0f, 2.0f);

    Transform* transform2 = obj1.getComponent<Transform>();

    obj1.removeComponent<Transform>();

    objManager.removeObject(obj1);

    Transform* transform3 = obj1.getComponent<Transform>();

    // START TEST THREADPOOL
    // ...
    */

    FrameTimer frameTimer;

    while (running) {
        const double deltaSeconds = frameTimer.Tick();

        // Run each registered system. Systems can consult the engine's
        // state or the delta time if they need it for frame dependent
        // calculations.
        for (auto& system : systems) {
            system->Update();
        }

        // If a system requested shutdown (e.g., the window was closed),
        // exit the loop after this iteration.
        if (!running) {
            break;
        }

        // Simple pacing to avoid a busy loop when no systems perform a
        // blocking wait. Sleep for a tiny duration based on the measured
        // delta time to yield time slices back to the OS.
        const auto sleepDuration = std::chrono::duration<double>(std::max(0.0, 0.001 - deltaSeconds));
        if (sleepDuration.count() > 0.0) {
            std::this_thread::sleep_for(sleepDuration);
        }
    }
}

void Engine::Shutdown()
{
    for (auto& system : systems) {
        system->Shutdown();
    }

    pool.shutdown();

    // Notify that the engine shutdown path completed and logging can be stopped soon.
    TERRAENGINE_LOG(LogLevel::Info, "Shutdown complete!");

    // Stop the logger thread so queued messages flush before the application exits.
    Logger::Instance().Shutdown();
}
