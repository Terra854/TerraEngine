#include "WindowSystem.h"

#include <stdexcept>

#include "Debug.h"
#include "Core/Logger/Logger.h"

WindowSystem::WindowSystem(std::unique_ptr<IWindowBackend> backendInstance, WindowConfig config, std::function<void()> shutdownCallback)
    : windowConfig(std::move(config)), backend(std::move(backendInstance)), onShutdownRequested(std::move(shutdownCallback))
{
}

void WindowSystem::Initialize()
{
    if (!backend) {
        throw std::runtime_error("WindowSystem requires a valid backend.");
    }

    // Route the backend close request back to the engine so the main loop can
    // exit cleanly.
    backend->SetCloseCallback([this]() {
        if (onShutdownRequested) {
            onShutdownRequested();
        }
    });

    backend->Initialize(windowConfig);
    initialized = true;

    // Let consumers know that window creation succeeded.
    TERRAENGINE_LOG(LogLevel::Info, "WindowSystem initialized using backend: " << SystemName());
}

void WindowSystem::Update()
{
    if (!initialized) {
        return;
    }

    backend->PollEvents();

    if (backend->ShouldClose()) {
        // Respect the request from the OS/user to close the application.
        if (onShutdownRequested) {
            onShutdownRequested();
        }
    }

    backend->SwapBuffers();
}

void WindowSystem::Shutdown()
{
    if (!backend) {
        return;
    }

    backend->Shutdown();
    initialized = false;

    TERRAENGINE_LOG(LogLevel::Info, "WindowSystem shutdown complete.");
}

std::string WindowSystem::SystemName()
{
    return "WindowSystem";
}
