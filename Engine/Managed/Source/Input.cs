using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
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

    public static class Input
    {
        public static bool IsKeyPressed(Key key)
        {
            return Native_Input_IsKeyPressed((int)key);
        }

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
        private static extern bool Native_Input_IsKeyPressed(int key);

        [DllImport("TerraEngine")]
        private static extern bool Native_Input_IsMouseButtonPressed(int button);

        [DllImport("TerraEngine")]
        private static extern void Native_Input_GetMousePosition(out double x, out double y);
    }
}
