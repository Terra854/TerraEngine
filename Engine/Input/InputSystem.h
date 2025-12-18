#pragma once

#include <array>
#include <memory>
#include <string>
#include <utility>

#include "Core/SystemBase.h"
#include "InputBackend.h"
#include "InputCodes.h"

// Engine level input system responsible for translating backend events into
// easily queried state for the rest of the application. Like the window system
// it delegates all platform specific work to an IInputBackend implementation so
// the engine can swap GLFW out without touching consumer code.
class InputSystem : public SystemsBase {
public:
	static InputSystem& Instance();

	InputSystem(const InputSystem&) = delete;
	InputSystem& operator=(const InputSystem&) = delete;
	InputSystem(InputSystem&&) = delete;
	InputSystem& operator=(InputSystem&&) = delete;
	~InputSystem() override = default;

	// Configure the backend before initialization so callbacks can be registered
	// and resources allocated exactly once.
	void Configure(std::unique_ptr<IInputBackend> backendInstance);

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	// Public helpers for higher level systems to interrogate input state without
	// needing to know which backend is active.
	KeyState GetKeyState(Key key) const;
	bool IsKeyPressed(Key key) const;
	bool IsMouseButtonPressed(MouseButton button) const;
	std::pair<double, double> GetCursorPosition() const;
	void SetCursorPosition(double x, double y);
	std::pair<double, double> GetScrollDelta() const;

private:
	InputSystem() = default;

	void HandleKeyEvent(Key key, KeyState state);
	void HandleMouseButtonEvent(MouseButton button, KeyState state);
	void HandleCursorMove(double x, double y);
	void HandleScroll(double xOffset, double yOffset);

	std::array<KeyState, KeyCount()> keyStates{};
	std::array<KeyState, MouseButtonCount()> mouseButtonStates{};
	std::pair<double, double> cursorPosition{0.0, 0.0};
	std::pair<double, double> scrollDelta{0.0, 0.0};

	std::unique_ptr<IInputBackend> backend;
	bool configured = false;
	bool initialized = false;
};

