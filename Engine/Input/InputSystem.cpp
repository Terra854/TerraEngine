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

void InputSystem::Configure(std::unique_ptr<IInputBackend> backendInstance)
{
	backend = std::move(backendInstance);
	configured = true;
	initialized = false;
	
	for (auto& state : keyStates) {
		state = KeyState::Idle;
	}

	for (auto& state : mouseButtonStates) {
		state = KeyState::Idle;
	}
	
	scrollDelta = {0.0, 0.0};
}

void InputSystem::Initialize()
{
	if (!configured) {
		throw std::runtime_error("InputSystem must be configured before initialization.");
	}
	
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
	
	TERRAENGINE_LOG(LogLevel::Info, "InputSystem initialized using backend: " << SystemName());
}

void InputSystem::Update()
{
	if (!initialized) {
		return;
	}

	// Input state must update after the window pumps platform events. This
	// sequential ordering keeps callbacks in the same frame while avoiding extra
	// synchronization primitives that would complicate multithreaded use.
	// Clear the scroll delta before polling so each frame reflects only the
	// most recent scroll input. Without this reset, consumers would continue to
	// observe the previous frame's delta and react as if scrolling never ended.
	scrollDelta = {0.0, 0.0};
	backend->Update();
	cursorPosition = backend->GetCursorPosition();
}

void InputSystem::Shutdown()
{
	if (!backend) {
		return;
	}
	
	backend->Shutdown();
	initialized = false;
	
	TERRAENGINE_LOG(LogLevel::Info, "InputSystem shutdown complete.");
}

std::string InputSystem::SystemName()
{
	return "InputSystem";
}

KeyState InputSystem::GetKeyState(Key key) const
{
	const auto index = static_cast<std::size_t>(key);
	if (index >= keyStates.size()) {
		return KeyState::Idle;
	}

	return keyStates[index];
}

bool InputSystem::IsKeyPressed(Key key) const
{
	const auto state = GetKeyState(key);
	return state == KeyState::Pressed || state == KeyState::Held;
}

bool InputSystem::IsMouseButtonPressed(MouseButton button) const
{
	const auto index = static_cast<std::size_t>(button);
	if (index >= mouseButtonStates.size()) {
		return false;
	}
	
	const auto state = mouseButtonStates[index];
	return state == KeyState::Pressed || state == KeyState::Held;
}

std::pair<double, double> InputSystem::GetCursorPosition() const
{
	return cursorPosition;
}

void InputSystem::SetCursorPosition(double x, double y)
{
	cursorPosition = {x, y};
	
	if (backend) {
		backend->SetCursorPosition(x, y);
	}
}

std::pair<double, double> InputSystem::GetScrollDelta() const
{
	return scrollDelta;
}

void InputSystem::HandleKeyEvent(Key key, KeyState state)
{
	const auto index = static_cast<std::size_t>(key);
	if (index >= keyStates.size()) {
		return;
	}
	
	keyStates[index] = state;
}

void InputSystem::HandleMouseButtonEvent(MouseButton button, KeyState state)
{
	const auto index = static_cast<std::size_t>(button);
	if (index >= mouseButtonStates.size()) {
		return;
	}
	
	mouseButtonStates[index] = state;
}

void InputSystem::HandleCursorMove(double x, double y)
{
	cursorPosition = {x, y};
}

void InputSystem::HandleScroll(double xOffset, double yOffset)
{
	scrollDelta = {xOffset, yOffset};
}

