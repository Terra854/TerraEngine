#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Core/SystemBase.h"
#include "WindowBackend.h"
#include "WindowConfig.h"

// A simple engine system responsible for owning the platform window. The class
// collaborates with an IWindowBackend instance so GLFW usage remains isolated
// and swappable.
class WindowSystem : public SystemsBase {
public:
    WindowSystem(std::unique_ptr<IWindowBackend> backend, WindowConfig config, std::function<void()> shutdownCallback);
    ~WindowSystem() override = default;

    void Initialize() override;
    void Update() override;
    void Shutdown() override;
    std::string SystemName() override;

private:
    WindowConfig windowConfig;
    std::unique_ptr<IWindowBackend> backend;
    std::function<void()> onShutdownRequested;
    bool initialized = false;
};
