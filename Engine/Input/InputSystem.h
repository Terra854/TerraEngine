// InputSystem.h — Interface for the engine's central input management system.
//
// This file declares the InputSystem class, which acts as the authoritative source
// for all input state within the engine. It manages the lifecycle of the active
// input backend and provides a normalized API for systems and scripts to
// interrogate input events in a backend-independent manner.

#pragma once

#include <array>
#include <cstdint>
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
	bool WasMouseButtonPressedThisFrame(MouseButton button) const;
	std::pair<double, double> GetCursorPosition() const;
	void SetCursorPosition(double x, double y);
	std::pair<double, double> GetScrollDelta() const;

	struct InputSnapshot
	{
		std::uint64_t frameRevision = 0;
		std::array<KeyState, KeyCount()> keyStates{};
		std::array<KeyState, MouseButtonCount()> mouseButtonStates{};
		std::array<bool, MouseButtonCount()> mouseButtonPressedThisFrame{};
		std::pair<double, double> cursorPosition{ 0.0, 0.0 };
		std::pair<double, double> scrollDelta{ 0.0, 0.0 };
		bool workerReadable = false;
		std::string boundarySummary;
	};

	[[nodiscard]] InputSnapshot GetLatestWorkerSnapshot() const;
	[[nodiscard]] std::string GetPollingBoundarySummary() const;

private:
	InputSystem() = default;

	void HandleKeyEvent(Key key, KeyState state);
	void HandleMouseButtonEvent(MouseButton button, KeyState state);
	void HandleCursorMove(double x, double y);
	void HandleScroll(double xOffset, double yOffset);

	std::array<KeyState, KeyCount()> keyStates{};
	std::array<KeyState, MouseButtonCount()> mouseButtonStates{};
	std::array<std::uint64_t, MouseButtonCount()> mouseButtonPressGenerations{};
	std::array<std::uint64_t, MouseButtonCount()> observedMouseButtonPressGenerations{};
	std::array<bool, MouseButtonCount()> mouseButtonPressedThisFrame{};
	std::pair<double, double> cursorPosition{0.0, 0.0};
	std::pair<double, double> scrollDelta{0.0, 0.0};
	InputSnapshot latestSnapshot{};
	std::uint64_t nextSnapshotRevision = 1;

	std::unique_ptr<IInputBackend> backend;
	bool configured = false;
	bool initialized = false;
};
