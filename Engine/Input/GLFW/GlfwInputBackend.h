// GlfwInputBackend.h — Interface for the GLFW-based input backend adapter.
//
// This file declares the GlfwInputBackend class, which provides a bridge between
// the GLFW windowing library and the engine's InputSystem. It maintains the
// necessary state and callback mappings to capture and route raw platform
// input events to the appropriate engine systems.

#pragma once

#include <functional>
#include <utility>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "../InputBackend.h"
#include "../InputCodes.h"

// GLFW implementation of the abstract input backend. All direct GLFW calls live
// here so the rest of the engine can interact with keyboard and mouse state via
// the backend agnostic interfaces.
class GlfwInputBackend : public IInputBackend {
public:
	GlfwInputBackend() = default;
	~GlfwInputBackend() override = default;

	// BackendName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string BackendName() const override { return "GLFW"; }
	void SetWindowHandle(void* windowHandle) override;
	void Initialize() override;
	void Update() override;
	void Shutdown() override;

	void SetKeyCallback(std::function<void(Key, KeyState)> onKeyEvent) override;
	void SetMouseButtonCallback(std::function<void(MouseButton, KeyState)> onMouseButton) override;
	void SetCursorPositionCallback(std::function<void(double, double)> onCursorMove) override;
	void SetScrollCallback(std::function<void(double, double)> onScroll) override;

	bool IsKeyPressed(Key key) const override;
	bool IsMouseButtonPressed(MouseButton button) const override;
	std::pair<double, double> GetCursorPosition() const override;
	void SetCursorPosition(double x, double y) override;

private:
	GLFWwindow* window = nullptr;
	std::function<void(Key, KeyState)> keyCallback;
	std::function<void(MouseButton, KeyState)> mouseButtonCallback;
	std::function<void(double, double)> cursorCallback;
	std::function<void(double, double)> scrollCallback;

	std::pair<double, double> ScaleCursorToFramebuffer(double x, double y) const;
	std::pair<double, double> ScaleCursorToWindow(double x, double y) const;

	static int TranslateKeyToGlfw(Key key);
	static int TranslateMouseButtonToGlfw(MouseButton button);
	static Key TranslateKeyFromGlfw(int glfwKey);
	static KeyState TranslateStateFromGlfw(int glfwAction);
};
