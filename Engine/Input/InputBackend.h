// InputBackend.h — Abstract interface for engine input backend adapters.
//
// This file defines the IInputBackend interface, which serves as the contract
// for any hardware or library-specific input implementation. By abstracting the
// backend, the engine can remain platform-agnostic while still providing robust
// support for different windowing and input libraries.

#pragma once

#include <functional>
#include <utility>
#include <string>

#include "InputCodes.h"

// Interface describing the minimal contract required for the engine's input
// system. Concrete implementations (GLFW, platform specific, etc.) translate
// the backend events into the engine's Key/MouseButton types so callers never
	// include library headers directly.
	class IInputBackend {
	public:
		virtual ~IInputBackend() = default;

		// Identify the backend for logging and diagnostics.
		virtual std::string BackendName() const = 0;

		// Provide a pointer to the platform specific window so the backend can
		// subscribe to input events. The pointer is intentionally opaque here to
		// keep higher level code agnostic of the underlying windowing library.
		virtual void SetWindowHandle(void* windowHandle) = 0;

	// Initialize any backend specific state such as registering callbacks.
	virtual void Initialize() = 0;

	// Allow the backend to refresh its internal caches once per frame. For GLFW
	// this may be a no-op, but other platforms might need to poll hardware
	// devices explicitly.
	virtual void Update() = 0;

	// Release resources or unregister callbacks. Safe to call multiple times.
	virtual void Shutdown() = 0;

	// Register callbacks so the backend can report input changes back to the
	// engine. These indirections keep GLFW symbols out of higher level code and
	// make unit testing easier.
	virtual void SetKeyCallback(std::function<void(Key, KeyState)> onKeyEvent) = 0;
	virtual void SetMouseButtonCallback(std::function<void(MouseButton, KeyState)> onMouseButton) = 0;
	virtual void SetCursorPositionCallback(std::function<void(double, double)> onCursorMove) = 0;
	virtual void SetScrollCallback(std::function<void(double, double)> onScroll) = 0;

	// Direct state queries so consumers can check real time input without waiting
	// for a callback (useful for polling style interactions).
	virtual bool IsKeyPressed(Key key) const = 0;
	virtual bool IsMouseButtonPressed(MouseButton button) const = 0;
	virtual std::pair<double, double> GetCursorPosition() const = 0;
	virtual void SetCursorPosition(double x, double y) = 0;
};
