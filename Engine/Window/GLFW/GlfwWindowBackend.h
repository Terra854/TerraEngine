#pragma once

#include <functional>
#include <memory>
#include <string>

// Avoid pulling in platform-specific OpenGL headers since rendering is not set
// up yet. The backend only needs access to the GLFW window handle and event
// polling entry points.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "../WindowBackend.h"

// GLFW implementation of the window backend interface. All direct GLFW calls
// live here so that the rest of the engine only depends on the abstract
// IWindowBackend API.
	class GlfwWindowBackend : public IWindowBackend {
	public:
		GlfwWindowBackend();
		~GlfwWindowBackend() override;

		std::string BackendName() const override { return "GLFW"; }
		void Initialize(const WindowConfig& config) override;
		void PollEvents() override;
		void SwapBuffers() override;
		bool ShouldClose() const override;
		void* GetNativeHandle() const override;
	void SetCloseCallback(std::function<void()> onClose) override;
	void SetResizeCallback(std::function<void(int, int)> onResize) override;
	void Shutdown() override;

private:
	GLFWwindow* windowHandle = nullptr;
	std::function<void()> closeCallback;

	void InstallCallbacks();
};

