// Input.cs — Managed API for keyboard and mouse input handling.
//
// This file provides the C# interface for querying the state of the keyboard and
// mouse. It includes enumerations for all supported keys and buttons and
// provides simple static methods for checking if an input is currently active,
// mirroring the core engine's input system.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	public enum InputState : byte
	{
		Idle = 0,
		Released = 1,
		Pressed = 2,
		Held = 3
	}

	public sealed class InputSnapshot
	{
		public ulong FrameRevision { get; internal init; }
		public InputState[] KeyStates { get; internal init; } = Array.Empty<InputState>();
		public InputState[] MouseButtonStates { get; internal init; } = Array.Empty<InputState>();
		public bool[] MousePressedThisFrame { get; internal init; } = Array.Empty<bool>();
		public Vector2 MousePosition { get; internal init; }
		public Vector2 ScrollDelta { get; internal init; }
		public bool WorkerReadable { get; internal init; }
		public string Diagnostic { get; internal init; } = string.Empty;
	}
    // Key is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public enum Key : int
    {
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
        Menu
    }

    // MouseButton is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public enum MouseButton : int
    {
        Left = 0,
        Right,
        Middle,
        Button4,
        Button5,
        Button6,
        Button7,
        Button8
    }

    // Input is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public static class Input
	{
        public static bool IsKeyPressed(Key key)
        {
            return Native_Input_IsKeyPressed((int)key);
        }

        // IsMouseButtonPressed bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static bool IsMouseButtonPressed(MouseButton button)
        {
            return Native_Input_IsMouseButtonPressed((int)button);
        }

		public static Vector2 MousePosition
		{
			get
			{
				InternalCalls.Native_Input_GetMousePosition(out double x, out double y);
				return new Vector2((float)x, (float)y);
			}
			set => InternalCalls.Native_Input_SetMousePosition(value.X, value.Y);
		}

		public static unsafe InputSnapshot Snapshot()
		{
			NativeInputSnapshot native = new() { Header = NativeRuntimeAbiHeader.For<NativeInputSnapshot>() };
			InternalCalls.Native_Input_ReadSnapshot(ref native);
			int keyCount = (int)Math.Min(native.KeyCount, NativeInputSnapshot.KeyCapacity);
			int mouseCount = (int)Math.Min(native.MouseButtonCount, NativeInputSnapshot.MouseCapacity);
			InputState[] keys = new InputState[keyCount];
			InputState[] buttons = new InputState[mouseCount];
			bool[] pressed = new bool[mouseCount];
			// Local fixed-buffer structs already have a stable stack address for the
			// duration of this method, so they can be indexed without pinning again.
			for (int index = 0; index < keyCount; ++index) keys[index] = (InputState)native.KeyStates[index];
			for (int index = 0; index < mouseCount; ++index)
			{
				buttons[index] = (InputState)native.MouseButtonStates[index];
				pressed[index] = native.MousePressedThisFrame[index] != 0;
			}
			return new InputSnapshot
			{
				FrameRevision = native.FrameRevision, KeyStates = keys, MouseButtonStates = buttons,
				MousePressedThisFrame = pressed, MousePosition = new Vector2((float)native.CursorX, (float)native.CursorY),
				ScrollDelta = new Vector2((float)native.ScrollX, (float)native.ScrollY),
				WorkerReadable = native.WorkerReadable != 0, Diagnostic = native.Diagnostic
			};
		}

		public static InputState GetKeyState(Key key)
		{
			InputState[] states = Snapshot().KeyStates;
			int index = (int)key;
			return index >= 0 && index < states.Length ? states[index] : InputState.Idle;
		}

		public static bool WasKeyPressedThisFrame(Key key) => GetKeyState(key) == InputState.Pressed;
		public static bool WasKeyReleasedThisFrame(Key key) => GetKeyState(key) == InputState.Released;

		public static InputState GetMouseButtonState(MouseButton button)
		{
			InputState[] states = Snapshot().MouseButtonStates;
			int index = (int)button;
			return index >= 0 && index < states.Length ? states[index] : InputState.Idle;
		}

		public static bool WasMouseButtonPressedThisFrame(MouseButton button)
		{
			bool[] pressed = Snapshot().MousePressedThisFrame;
			int index = (int)button;
			return index >= 0 && index < pressed.Length && pressed[index];
		}

        [DllImport("TerraEngine")]
        // Native_Input_IsKeyPressed bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern bool Native_Input_IsKeyPressed(int key);

        [DllImport("TerraEngine")]
        // Native_Input_IsMouseButtonPressed bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern bool Native_Input_IsMouseButtonPressed(int button);

        [DllImport("TerraEngine")]
        // Native_Input_GetMousePosition bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern void Native_Input_GetMousePosition(out double x, out double y);
    }
}
