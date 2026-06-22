// InputSystem.cpp — Implementation of the central input management system.
//
// This file implements the InputSystem singleton, which coordinates with the
// active input backend to track and expose the global state of the keyboard and
// mouse. It provides high-level query functions for checking key presses and
// cursor positions, serving as the primary input interface for gameplay code.

#include "InputSystem.h"

#include <stdexcept>

#include "Debug.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"

InputSystem& InputSystem::Instance()
{
	// Provide a singleton instance so the main engine loop can register the
	// system without managing heap allocations or ownership semantics.
	static InputSystem instance;
	return instance;
}

// InputSystem::Configure is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::Configure(std::unique_ptr<IInputBackend> backendInstance)
{
	backend = std::move(backendInstance);
	configured = true;
	initialized = false;
	// Capture backend selection early so later initialization failures are easier to trace.
	TERRAENGINE_LOG(LogLevel::Debug, "InputSystem configured with backend " << (backend ? backend->BackendName() : "null"));
	
	for (auto& state : keyStates) {
		state = KeyState::Idle;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& state : mouseButtonStates) {
		state = KeyState::Idle;
	}
	for (auto& generation : mouseButtonPressGenerations) {
		generation = 0;
	}
	for (auto& generation : observedMouseButtonPressGenerations) {
		generation = 0;
	}
	for (auto& pressedThisFrame : mouseButtonPressedThisFrame) {
		pressedThisFrame = false;
	}
	
	scrollDelta = {0.0, 0.0};
}

// InputSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::Initialize()
{
	if (!configured) {
		throw std::runtime_error("InputSystem must be configured before initialization.");
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!backend) {
		throw std::runtime_error("InputSystem requires a valid backend.");
	}
	
	// The input backend needs access to the native window so callbacks can be
	// registered. Because the window system initializes first in the engine's
	// system list, the handle is guaranteed to be ready here.
	void* nativeWindow = WindowSystem::Instance().GetNativeHandle();
	if (!nativeWindow) {
		throw std::runtime_error("InputSystem could not locate a valid native window handle.");
	}
	
	backend->SetWindowHandle(nativeWindow);
	
	backend->SetKeyCallback([this](Key key, KeyState state) {
		HandleKeyEvent(key, state);
	});
	
	backend->SetMouseButtonCallback([this](MouseButton button, KeyState state) {
		HandleMouseButtonEvent(button, state);
	});
	
	backend->SetCursorPositionCallback([this](double x, double y) {
		HandleCursorMove(x, y);
	});
	
	backend->SetScrollCallback([this](double xOffset, double yOffset) {
		HandleScroll(xOffset, yOffset);
	});
	
	backend->Initialize();
	initialized = true;
	
	TERRAENGINE_LOG(LogLevel::Info, "InputSystem initialized using backend: " << backend->BackendName());
}

// InputSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}

	backend->Update();
	cursorPosition = backend->GetCursorPosition();

	// WindowSystem polls platform events before InputSystem updates, so GLFW
	// callbacks may already have advanced a press generation for this frame.
	// Publishing a separate one-frame latch preserves quick press/release clicks
	// whose final button state is Released by the time UI performs hit testing.
	for (std::size_t index = 0; index < mouseButtonStates.size(); ++index) {
		const bool sawNewPress = mouseButtonPressGenerations[index] != observedMouseButtonPressGenerations[index];
		mouseButtonPressedThisFrame[index] = sawNewPress;
		observedMouseButtonPressGenerations[index] = mouseButtonPressGenerations[index];
	}
}

// InputSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::Shutdown()
{
	if (!backend) {
		return;
	}
	
	backend->Shutdown();
	initialized = false;
	
	TERRAENGINE_LOG(LogLevel::Info, "InputSystem shutdown complete (backend " << backend->BackendName() << ").");
}

// InputSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string InputSystem::SystemName()
{
	return "InputSystem";
}

// InputSystem::GetKeyState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
KeyState InputSystem::GetKeyState(Key key) const
{
	const auto index = static_cast<std::size_t>(key);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (index >= keyStates.size()) {
		return KeyState::Idle;
	}

	return keyStates[index];
}

// InputSystem::IsKeyPressed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool InputSystem::IsKeyPressed(Key key) const
{
	const auto state = GetKeyState(key);
	return state == KeyState::Pressed || state == KeyState::Held;
}

// InputSystem::IsMouseButtonPressed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool InputSystem::IsMouseButtonPressed(MouseButton button) const
{
	const auto index = static_cast<std::size_t>(button);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (index >= mouseButtonStates.size()) {
		return false;
	}
	
	const auto state = mouseButtonStates[index];
	return state == KeyState::Pressed || state == KeyState::Held;
}

// InputSystem::WasMouseButtonPressedThisFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool InputSystem::WasMouseButtonPressedThisFrame(MouseButton button) const
{
	const auto index = static_cast<std::size_t>(button);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (index >= mouseButtonPressedThisFrame.size()) {
		return false;
	}

	return mouseButtonPressedThisFrame[index];
}

// InputSystem::GetCursorPosition is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::pair<double, double> InputSystem::GetCursorPosition() const
{
	return cursorPosition;
}

// InputSystem::SetCursorPosition is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::SetCursorPosition(double x, double y)
{
	cursorPosition = {x, y};
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (backend) {
		backend->SetCursorPosition(x, y);
	}
}

// InputSystem::GetScrollDelta is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::pair<double, double> InputSystem::GetScrollDelta() const
{
	return scrollDelta;
}

// InputSystem::HandleKeyEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::HandleKeyEvent(Key key, KeyState state)
{
	const auto index = static_cast<std::size_t>(key);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (index >= keyStates.size()) {
		return;
	}
	
	keyStates[index] = state;
}

// InputSystem::HandleMouseButtonEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::HandleMouseButtonEvent(MouseButton button, KeyState state)
{
	const auto index = static_cast<std::size_t>(button);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (index >= mouseButtonStates.size()) {
		return;
	}
	
	mouseButtonStates[index] = state;
	if (state == KeyState::Pressed) {
		// Count press transitions independently from the final button state so
		// a complete click inside one event-poll cycle is still observable by UI.
		++mouseButtonPressGenerations[index];
	}
}

// InputSystem::HandleCursorMove is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::HandleCursorMove(double x, double y)
{
	cursorPosition = {x, y};
}

// InputSystem::HandleScroll is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InputSystem::HandleScroll(double xOffset, double yOffset)
{
	scrollDelta = {xOffset, yOffset};
}
