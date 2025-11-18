#pragma once

#include <functional>
#include <string>

#include "WindowConfig.h"

// An interface that isolates concrete windowing libraries from the rest of the
// engine. Engine systems interact with this abstraction rather than with GLFW
// directly, allowing the backend to be swapped without touching the higher
// level code.
class IWindowBackend {
public:
    virtual ~IWindowBackend() = default;

    // Initialize the underlying windowing library and create a platform window.
    // Throws on failure so callers cannot proceed with a partially created
    // window.
    virtual void Initialize(const WindowConfig& config) = 0;

    // Poll platform events (keyboard, mouse, resize, close, etc.). This keeps
    // the event queue serviced without exposing GLFW specific calls to the
    // window system.
    virtual void PollEvents() = 0;

    // Present any queued frame buffers if the backend requires it. The default
    // implementation can be empty for backends that do not need a swap.
    virtual void SwapBuffers() = 0;

    // Query whether the user has requested that the window close. The window
    // system uses this to propagate shutdown requests to the engine.
    virtual bool ShouldClose() const = 0;

    // Supply a callback to run when the underlying platform reports a close
    // event. This indirection lets the engine perform custom cleanup or
    // shutdown signaling while keeping the backend decoupled.
    virtual void SetCloseCallback(std::function<void()> onClose) = 0;

    // Release backend specific state and destroy the window. Implementations
    // should be safe to call even if initialization failed part way through.
    virtual void Shutdown() = 0;
};
