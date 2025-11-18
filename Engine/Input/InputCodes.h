#pragma once

#include <cstddef>

// Represents a library independent view of all keyboard keys supported by the
// engine. The values intentionally avoid tying themselves to any specific
// backend so the input system can translate them for GLFW or any future
// platform without refactoring higher level code.
enum class Key {
	Unknown = 0,
	Space,
	Apostrophe,
	Comma,
	Minus,
	Period,
	Slash,
	Digit0,
	Digit1,
	Digit2,
	Digit3,
	Digit4,
	Digit5,
	Digit6,
	Digit7,
	Digit8,
	Digit9,
	Semicolon,
	Equal,
	A,
	B,
	C,
	D,
	E,
	F,
	G,
	H,
	I,
	J,
	K,
	L,
	M,
	N,
	O,
	P,
	Q,
	R,
	S,
	T,
	U,
	V,
	W,
	X,
	Y,
	Z,
	LeftBracket,
	Backslash,
	RightBracket,
	GraveAccent,
	World1,
	World2,
	Escape,
	Enter,
	Tab,
	Backspace,
	Insert,
	DeleteKey,
	RightArrow,
	LeftArrow,
	DownArrow,
	UpArrow,
	PageUp,
	PageDown,
	Home,
	End,
	CapsLock,
	ScrollLock,
	NumLock,
	PrintScreen,
	Pause,
	F1,
	F2,
	F3,
	F4,
	F5,
	F6,
	F7,
	F8,
	F9,
	F10,
	F11,
	F12,
	F13,
	F14,
	F15,
	F16,
	F17,
	F18,
	F19,
	F20,
	F21,
	F22,
	F23,
	F24,
	F25,
	KP0,
	KP1,
	KP2,
	KP3,
	KP4,
	KP5,
	KP6,
	KP7,
	KP8,
	KP9,
	KPDecimal,
	KPDivide,
	KPMultiply,
	KPSubtract,
	KPAdd,
	KPEnter,
	KPEqual,
	LeftShift,
	LeftControl,
	LeftAlt,
	LeftSuper,
	RightShift,
	RightControl,
	RightAlt,
	RightSuper,
	Menu,
	Count
};

// Describes the possible state transitions for keys and mouse buttons. The
// Repeat state allows callers to differentiate between a sustained press and a
// fresh key down event.
enum class KeyState {
	Released = 0,
	Pressed,
	Repeat
};

// Enumerates the available mouse buttons in a backend agnostic format. The
// ordering matches common desktop mice while leaving room to extend in the
// future without disrupting existing mappings.
enum class MouseButton {
	Left = 0,
	Right,
	Middle,
	Button4,
	Button5,
	Button6,
	Button7,
	Button8,
	Count
};

constexpr std::size_t KeyCount()
	{
	return static_cast<std::size_t>(Key::Count);
}

constexpr std::size_t MouseButtonCount()
	{
	return static_cast<std::size_t>(MouseButton::Count);
}

