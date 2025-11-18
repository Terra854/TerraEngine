#include "GlfwWindowBackend.h"

#include <stdexcept>

GlfwWindowBackend::GlfwWindowBackend() = default;

GlfwWindowBackend::~GlfwWindowBackend()
{
	// Make sure GLFW resources are cleaned up even if Shutdown was forgotten.
	Shutdown();
}

void GlfwWindowBackend::Initialize(const WindowConfig& config)
{
	// Initialize GLFW before creating any windows.
	if (!glfwInit()) {
		throw std::runtime_error("Failed to initialize GLFW.");
	}
	
	// Create the GLFW window using the configuration provided by the engine.
	windowHandle = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
	if (!windowHandle) {
		glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window.");
	}
	
	// Link this backend instance to the GLFW window so callbacks can access it.
	glfwSetWindowUserPointer(windowHandle, this);
	InstallCallbacks();
}

void GlfwWindowBackend::PollEvents()
{
	glfwPollEvents();
}

void GlfwWindowBackend::SwapBuffers()
{
	if (windowHandle) {
		glfwSwapBuffers(windowHandle);
	}
}

bool GlfwWindowBackend::ShouldClose() const
{
	return windowHandle && glfwWindowShouldClose(windowHandle);
}

void* GlfwWindowBackend::GetNativeHandle() const
{
	return windowHandle;
}

void GlfwWindowBackend::SetCloseCallback(std::function<void()> onClose)
{
	closeCallback = std::move(onClose);
}

void GlfwWindowBackend::Shutdown()
{
	if (windowHandle) {
		glfwDestroyWindow(windowHandle);
		windowHandle = nullptr;
	}
	
	// Terminate GLFW after destroying the window. Safe to call even if GLFW was
	// never fully initialized.
	glfwTerminate();
}

void GlfwWindowBackend::InstallCallbacks()
{
	// Forward GLFW close events to the user-defined callback so the engine can
	// handle shutdown gracefully.
	glfwSetWindowCloseCallback(windowHandle, [](GLFWwindow* window) {
		auto* backend = static_cast<GlfwWindowBackend*>(glfwGetWindowUserPointer(window));
		if (backend && backend->closeCallback) {
			backend->closeCallback();
		}
	});
}
