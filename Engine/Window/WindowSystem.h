#pragma once

#include <functional>
#include <memory>
#include <string>

// Describes basic properties required to create a window without tying callers to
// a specific windowing library. Additional options can be added without changing
// the consumer-facing interface.
struct WindowConfig {
	int width = 1280;
	int height = 720;
	std::string title = "TerraEngine";
	bool enableVsync = true;
};

// Minimal interface for a windowing backend. The engine interacts exclusively
// with this abstraction so the underlying platform toolkit (GLFW or otherwise)
// can be swapped without touching engine code.
class IWindowSystem {
public:
	virtual ~IWindowSystem() = default;

	// Create the window using the provided configuration. The shutdown
	// callback allows the backend to notify the engine when the user
	// requests the application to close (e.g., hitting the close button).
	virtual bool Initialize(const WindowConfig& config,
	                        std::function<void()> shutdownCallback) = 0;

	// Process any pending OS events for the window.
	virtual void PollEvents() = 0;

	// Present the latest frame. Backends that do not rely on swap chains can
	// implement this as a no-op.
	virtual void SwapBuffers() = 0;

	// Whether the platform reported that the window should close.
	virtual bool ShouldClose() const = 0;

	// Tear down window resources. Called during engine shutdown.
	virtual void Shutdown() = 0;
};

// Simple factory helper so Engine can obtain the default window backend without
// leaking backend-specific types in headers.
std::unique_ptr<IWindowSystem> CreateDefaultWindowSystem();
