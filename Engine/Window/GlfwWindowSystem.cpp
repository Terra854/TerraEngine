#include "GlfwWindowSystem.h"

// GLFW includes remain confined to this translation unit to keep the rest of the
// engine agnostic of the specific windowing backend.
#include <GLFW/glfw3.h>

#include <memory>
#include <stdexcept>

GlfwWindowSystem::GlfwWindowSystem() = default;

GlfwWindowSystem::~GlfwWindowSystem()
{
	Shutdown();
}

bool GlfwWindowSystem::Initialize(const WindowConfig& config, std::function<void()> shutdownCallback)
{
	shutdownSignal = std::move(shutdownCallback);

	if (!glfwInit()) {
	// Throwing makes initialization failures obvious to the caller while
	// leaving GLFW fully encapsulated here.
	throw std::runtime_error("Failed to initialize GLFW window system.");
	}

	glfwInitialized = true;

	// Create a simple windowed context; rendering configuration can be layered on
	// later without exposing GLFW to other modules.
	windowHandle = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
	if (!windowHandle) {
	glfwTerminate();
	throw std::runtime_error("Failed to create GLFW window.");
	}

	glfwMakeContextCurrent(windowHandle);
	glfwSwapInterval(config.enableVsync ? 1 : 0);

	RegisterCallbacks();
	return true;
}

void GlfwWindowSystem::RegisterCallbacks()
{
	// Store a pointer to this backend so the static callback can retrieve the
	// shutdown signal functor.
	glfwSetWindowUserPointer(windowHandle, this);

	glfwSetWindowCloseCallback(windowHandle, [](GLFWwindow* window) {
	auto* backend = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(window));
	if (backend && backend->shutdownSignal) {
	backend->shutdownSignal();
	}
	});
}

void GlfwWindowSystem::PollEvents()
{
	glfwPollEvents();
}

void GlfwWindowSystem::SwapBuffers()
{
	if (windowHandle) {
	glfwSwapBuffers(windowHandle);
	}
}

bool GlfwWindowSystem::ShouldClose() const
{
	return windowHandle == nullptr ? true : glfwWindowShouldClose(windowHandle) == GLFW_TRUE;
}

void GlfwWindowSystem::Shutdown()
{
	if (windowHandle) {
	glfwDestroyWindow(windowHandle);
	windowHandle = nullptr;
	}

	// Terminate GLFW only once the window is destroyed to avoid leaks or dangling
	// callbacks.
	if (glfwInitialized) {
	glfwTerminate();
	glfwInitialized = false;
	}
}

std::unique_ptr<IWindowSystem> CreateDefaultWindowSystem()
{
	// Keep construction of the GLFW-backed implementation here so callers only
	// depend on the interface declared in WindowSystem.h.
	return std::make_unique<GlfwWindowSystem>();
}
