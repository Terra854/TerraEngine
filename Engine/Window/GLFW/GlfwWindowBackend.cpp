// GlfwWindowBackend.cpp — Contains the Glfw Window Backend backend adapter state and platform/API translation code implementation for GLFW window creation, callbacks, and platform window event translation.

#include "GlfwWindowBackend.h"

#include <stdexcept>
#include <unordered_map>

#include "Core/Logger/Logger.h"

// Static map to store callbacks without modifying class size (ABI fix)
static std::unordered_map<GLFWwindow*, std::function<void(int, int)>> g_ResizeCallbacks;

GlfwWindowBackend::GlfwWindowBackend() = default;

GlfwWindowBackend::~GlfwWindowBackend()
{
	// Make sure GLFW resources are cleaned up even if Shutdown was forgotten.
	Shutdown();
}

// GlfwWindowBackend::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::Initialize(const WindowConfig& config)
{
	// Initialize GLFW before creating any windows.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!glfwInit()) {
		throw std::runtime_error("Failed to initialize GLFW.");
	}
	
	// Explicitly disable OpenGL context creation so Vulkan can own the surface.
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	// Apply resizable hint based on configuration
	glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

	// Create the GLFW window using the configuration provided by the engine.
	windowHandle = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
	if (!windowHandle) {
		glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window.");
	}

	// Debug logging captures backend selection and window configuration for troubleshooting.
	TERRAENGINE_LOG(LogLevel::Debug, "GLFW backend created window " << config.title << " (" << config.width << "x" << config.height << ")");
	
	// Link this backend instance to the GLFW window so callbacks can access it.
	glfwSetWindowUserPointer(windowHandle, this);
	InstallCallbacks();
}

// GlfwWindowBackend::PollEvents is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::PollEvents()
{
	glfwPollEvents();
}

// GlfwWindowBackend::SwapBuffers is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::SwapBuffers()
{
	// TerraEngine creates GLFW windows with GLFW_NO_API so Vulkan owns the
	// swapchain and presentation. Calling glfwSwapBuffers without an OpenGL
	// context can trigger GLFW/driver validation work without presenting
	// anything, so this backend hook intentionally stays empty.
}

// GlfwWindowBackend::ShouldClose is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool GlfwWindowBackend::ShouldClose() const
{
	return windowHandle && glfwWindowShouldClose(windowHandle);
}

// GlfwWindowBackend::GetNativeHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void* GlfwWindowBackend::GetNativeHandle() const
{
	return windowHandle;
}

// GlfwWindowBackend::SetCloseCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::SetCloseCallback(std::function<void()> onClose)
{
	closeCallback = std::move(onClose);
}

// GlfwWindowBackend::SetResizeCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::SetResizeCallback(std::function<void(int, int)> onResize)
{
	if (windowHandle) {
		g_ResizeCallbacks[windowHandle] = std::move(onResize);
	}
}

// GlfwWindowBackend::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::Shutdown()
{
	if (windowHandle) {
		g_ResizeCallbacks.erase(windowHandle);
		glfwDestroyWindow(windowHandle);
		windowHandle = nullptr;
	}
	
	// Terminate GLFW after destroying the window. Safe to call even if GLFW was
	// never fully initialized.
	glfwTerminate();

	TERRAENGINE_LOG(LogLevel::Debug, "GLFW backend shutdown and window destroyed");
}

// GlfwWindowBackend::InstallCallbacks is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void GlfwWindowBackend::InstallCallbacks()
{
	// Forward GLFW close events to the user-defined callback so the engine can
	// handle shutdown gracefully.
	glfwSetWindowCloseCallback(windowHandle, [](GLFWwindow* window) {
		auto* backend = static_cast<GlfwWindowBackend*>(glfwGetWindowUserPointer(window));
		if (backend && backend->closeCallback) {
			TERRAENGINE_LOG(LogLevel::Debug, "GLFW backend received window close event");
			backend->closeCallback();
		}
	});

	glfwSetFramebufferSizeCallback(windowHandle, [](GLFWwindow* window, int width, int height) {
		// Use static map lookup
		auto it = g_ResizeCallbacks.find(window);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (it != g_ResizeCallbacks.end() && it->second) {
			try {
				it->second(width, height);
			} catch (const std::exception& e) {
				TERRAENGINE_LOG(LogLevel::Error, "GLFW Resize Callback Exception: " << e.what());
			} catch (...) {
				TERRAENGINE_LOG(LogLevel::Error, "GLFW Resize Callback Unknown Exception");
			}
		}
	});
}
