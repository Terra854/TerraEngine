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

	static int TranslateKeyToGlfw(Key key);
	static int TranslateMouseButtonToGlfw(MouseButton button);
	static Key TranslateKeyFromGlfw(int glfwKey);
	static KeyState TranslateStateFromGlfw(int glfwAction);
};

