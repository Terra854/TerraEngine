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
                double x, y;
                Native_Input_GetMousePosition(out x, out y);
                return new Vector2((float)x, (float)y);
            }
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
