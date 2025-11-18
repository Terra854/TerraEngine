#pragma once

#include "WindowSystem.h"

// Forward declaration keeps GLFW types out of engine headers while still allowing
// us to hold a pointer to the native window.
struct GLFWwindow;

// GLFW-backed window implementation isolated behind the IWindowSystem interface
// so callers never depend on GLFW headers directly.
class GlfwWindowSystem final : public IWindowSystem {
public:
	GlfwWindowSystem();
	~GlfwWindowSystem() override;

	bool Initialize(const WindowConfig& config, std::function<void()> shutdownCallback) override;
	void PollEvents() override;
	void SwapBuffers() override;
	bool ShouldClose() const override;
	void Shutdown() override;

private:
	void RegisterCallbacks();

	// Stored so we can notify the engine when the OS requests closure.
	std::function<void()> shutdownSignal;
	struct GLFWwindow* windowHandle = nullptr;
	bool glfwInitialized = false;
};
